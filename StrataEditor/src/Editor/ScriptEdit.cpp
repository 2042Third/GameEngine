#include "Editor/ScriptEdit.h"

#include "Editor/CommandUtils.h"
#include "Editor/EditorContext.h"

#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scripting/ScriptSystem.h>

#include <algorithm>

namespace Strata
{

	namespace ScriptEdit
	{

		namespace
		{

			bool Fail(std::string* outError, std::string message)
			{
				if (outError)
					*outError = std::move(message);
				return false;
			}

			// The script system of a playing scene (null in edit mode or simulate mode).
			ScriptSystem* GetRunningScriptSystem(Entity entity)
			{
				Scene* scene = entity.GetScene();
				return scene && scene->IsRunning() ? scene->GetSystem<ScriptSystem>() : nullptr;
			}

			std::string ListFieldNames(const ScriptClassInfo& info)
			{
				std::string names;
				for (const ScriptFieldInfo& field : info.Fields)
					names += (names.empty() ? "" : ", ") + field.Name;
				return names.empty() ? std::string("none") : names;
			}

		}

		std::optional<PropertyValue> FieldValueFromJson(const nlohmann::json& json, const ScriptFieldInfo& field, Scene& scene, std::string* outError)
		{
			auto fail = [&](const std::string& message) -> std::optional<PropertyValue>
			{
				if (outError)
					*outError = fmt::format("Field '{}' ({}): {}", field.Name, PropertyTypeToString(field.Type), message);
				return std::nullopt;
			};

			if (field.Type == PropertyType::Asset)
			{
				if (json.is_null() || (json.is_string() && json.get_ref<const std::string&>().empty()))
					return PropertyValue(UUID::Null());
				if (!json.is_string())
					return fail("expected an asset handle or a path relative to the asset directory");
				const std::optional<CommandUtils::FoundAsset> asset = CommandUtils::FindAsset(json.get_ref<const std::string&>());
				if (!asset)
					return fail(fmt::format("no asset '{}'", json.get<std::string>()));
				return PropertyValue(asset->Handle);
			}
			if (field.Type == PropertyType::Entity)
			{
				if (json.is_null())
					return PropertyValue(UUID::Null());
				const std::optional<UUID> id = UUIDFromJson(json);
				if (!id)
					return fail("expected an entity ID");
				if (id->IsValid() && !scene.GetEntityByUUID(*id))
					return fail(fmt::format("no entity {} in the scene", id->ToString()));
				return PropertyValue(*id);
			}

			PropertyInfo info;
			info.Name = field.Name;
			info.Type = field.Type;
			return PropertyValueFromJson(info, json, outError);
		}

		nlohmann::json FieldValueToJson(const PropertyValue& value, PropertyType type)
		{
			PropertyInfo info;
			info.Type = type;
			if (value.index() != GetPropertyValueIndex(type))
				return nullptr;
			return PropertyValueToJson(info, value);
		}

		nlohmann::json DescribeField(const ScriptFieldInfo& field)
		{
			return { { "name", field.Name }, { "type", PropertyTypeToString(field.Type) }, { "default", FieldValueToJson(field.DefaultValue, field.Type) } };
		}

		nlohmann::json DescribeClass(const ScriptClassInfo& info)
		{
			nlohmann::json fields = nlohmann::json::array();
			for (const ScriptFieldInfo& field : info.Fields)
				fields.push_back(DescribeField(field));
			nlohmann::json callbacks = nlohmann::json::array();
			for (ScriptCallback callback : { ScriptCallback::OnCreate, ScriptCallback::OnUpdate, ScriptCallback::OnFixedUpdate, ScriptCallback::OnLateUpdate,
					 ScriptCallback::OnDestroy, ScriptCallback::OnReload })
			{
				if (info.Implements(callback))
					callbacks.push_back(ScriptCallbackToString(callback));
			}
			return { { "name", info.Name }, { "fields", std::move(fields) }, { "callbacks", std::move(callbacks) } };
		}

		bool AddScript(EditorContext& context, Entity entity, const ScriptClassInfo& info, const std::vector<ScriptFieldValue>& fields, std::string* outError)
		{
			if (!entity)
				return Fail(outError, "The entity does not exist");
			ScriptComponent* existing = entity.TryGetComponent<ScriptComponent>();
			if (existing && existing->FindScript(info.Name))
				return Fail(outError, fmt::format("'{}' already has the script {}", entity.GetName(), info.Name));
			for (const ScriptFieldValue& fieldValue : fields)
			{
				const ScriptFieldInfo* field = info.FindField(fieldValue.Name);
				if (!field)
					return Fail(outError, fmt::format("{} has no field '{}' (fields: {})", info.Name, fieldValue.Name, ListFieldNames(info)));
				if (fieldValue.Type != field->Type || fieldValue.Value.index() != GetPropertyValueIndex(field->Type))
					return Fail(outError, fmt::format("Field '{}' of {} is {}", field->Name, info.Name, PropertyTypeToString(field->Type)));
			}

			SceneEditTransaction transaction(*entity.GetScene(), fmt::format("Add Script {}", info.Name), { entity.GetUUID() });
			ScriptComponent& component = existing ? *existing : entity.AddComponent<ScriptComponent>();
			ScriptEntry& entry = component.Scripts.emplace_back();
			entry.ClassName = info.Name;
			entry.Fields = fields;
			// Playing scenes create the instance at their next sync point.
			entity.MarkModified<ScriptComponent>();
			context.CommitEdit(transaction);
			return true;
		}

		bool RemoveScript(EditorContext& context, Entity entity, std::string_view className, std::string* outError)
		{
			if (!entity)
				return Fail(outError, "The entity does not exist");
			ScriptComponent* component = entity.TryGetComponent<ScriptComponent>();
			if (!component || !component->FindScript(className))
				return Fail(outError, fmt::format("'{}' has no script {}", entity.GetName(), className));

			SceneEditTransaction transaction(*entity.GetScene(), fmt::format("Remove Script {}", className), { entity.GetUUID() });
			std::erase_if(component->Scripts, [&](const ScriptEntry& entry) { return entry.ClassName == className; });
			if (component->Scripts.empty())
				entity.RemoveComponent<ScriptComponent>();
			else
				entity.MarkModified<ScriptComponent>();
			context.CommitEdit(transaction);
			return true;
		}

		bool SetField(EditorContext& context, Entity entity, std::string_view className, const ScriptFieldInfo& field, const std::optional<PropertyValue>& value,
			std::string* outError)
		{
			if (!entity)
				return Fail(outError, "The entity does not exist");
			ScriptComponent* component = entity.TryGetComponent<ScriptComponent>();
			ScriptEntry* entry = component ? component->FindScript(className) : nullptr;
			if (!entry)
				return Fail(outError, fmt::format("'{}' has no script {}", entity.GetName(), className));
			if (value && value->index() != GetPropertyValueIndex(field.Type))
				return Fail(outError, fmt::format("Field '{}' of {} is {}", field.Name, className, PropertyTypeToString(field.Type)));

			const std::string mergeKey = fmt::format("ScriptField/{}/{}/{}", entity.GetUUID().ToString(), className, field.Name);
			SceneEditTransaction transaction(*entity.GetScene(), fmt::format("Edit {} {}", className, field.Name), { entity.GetUUID() }, mergeKey);
			ScriptFieldValue* existing = entry->FindField(field.Name);
			if (value)
			{
				if (existing)
				{
					existing->Type = field.Type;
					existing->Value = *value;
				}
				else
				{
					entry->Fields.push_back(ScriptFieldValue { field.Name, field.Type, *value });
				}
			}
			else if (existing)
			{
				std::erase_if(entry->Fields, [&](const ScriptFieldValue& stored) { return stored.Name == field.Name; });
			}
			entity.MarkModified<ScriptComponent>();

			// Overrides only apply when instances are created: a running instance gets the value directly.
			if (ScriptSystem* system = GetRunningScriptSystem(entity))
				system->SetFieldValue(entity, className, field.Name, value ? *value : field.DefaultValue);
			context.CommitEdit(transaction);
			return true;
		}

	}

}
