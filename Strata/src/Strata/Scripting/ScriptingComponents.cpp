#include "stpch.h"
#include "Strata/Scripting/ScriptingComponents.h"

#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scripting/ScriptingRegistration.h"

#include <optional>
#include <string>

namespace Strata
{

	ScriptFieldValue* ScriptEntry::FindField(std::string_view name)
	{
		for (ScriptFieldValue& field : Fields)
		{
			if (field.Name == name)
				return &field;
		}
		return nullptr;
	}

	const ScriptFieldValue* ScriptEntry::FindField(std::string_view name) const
	{
		for (const ScriptFieldValue& field : Fields)
		{
			if (field.Name == name)
				return &field;
		}
		return nullptr;
	}

	ScriptEntry* ScriptComponent::FindScript(std::string_view className)
	{
		for (ScriptEntry& script : Scripts)
		{
			if (script.ClassName == className)
				return &script;
		}
		return nullptr;
	}

	const ScriptEntry* ScriptComponent::FindScript(std::string_view className) const
	{
		for (const ScriptEntry& script : Scripts)
		{
			if (script.ClassName == className)
				return &script;
		}
		return nullptr;
	}

	namespace
	{

		void SerializeScriptComponent(const ScriptComponent& component, nlohmann::json& out)
		{
			nlohmann::json scripts = nlohmann::json::array();
			for (const ScriptEntry& script : component.Scripts)
			{
				nlohmann::json fields = nlohmann::json::object();
				for (const ScriptFieldValue& field : script.Fields)
				{
					// Script fields have no named enum options, so enum values are stored as plain integers.
					const PropertyType type = field.Type == PropertyType::Enum ? PropertyType::Int : field.Type;
					PropertyInfo info;
					info.Name = field.Name;
					info.Type = type;
					if (field.Value.index() != GetPropertyValueIndex(type))
						continue;
					fields[field.Name] = { { "Type", PropertyTypeToString(type) }, { "Value", PropertyValueToJson(info, field.Value) } };
				}
				scripts.push_back({ { "Class", script.ClassName }, { "Fields", fields } });
			}
			out["Scripts"] = scripts;
		}

		bool DeserializeScriptComponent(ScriptComponent& component, const nlohmann::json& in, std::string* outError)
		{
			if (!in.contains("Scripts"))
				return true;
			if (!in["Scripts"].is_array())
			{
				if (outError)
					*outError = "'Scripts' must be an array";
				return false;
			}

			component.Scripts.clear();
			for (const nlohmann::json& scriptJson : in["Scripts"])
			{
				if (!scriptJson.is_object() || !scriptJson.contains("Class") || !scriptJson["Class"].is_string())
				{
					if (outError)
						*outError = "Each script needs a 'Class' name";
					return false;
				}

				ScriptEntry& script = component.Scripts.emplace_back();
				script.ClassName = scriptJson["Class"].get<std::string>();
				if (!scriptJson.contains("Fields") || !scriptJson["Fields"].is_object())
					continue;

				for (const auto& [fieldName, fieldJson] : scriptJson["Fields"].items())
				{
					// A malformed field is skipped (keeping every other script and field) rather than failing the component.
					if (!fieldJson.is_object() || !fieldJson.contains("Type") || !fieldJson["Type"].is_string() || !fieldJson.contains("Value"))
					{
						ST_CORE_WARN("Script field '{}.{}' needs 'Type' and 'Value'; skipped", script.ClassName, fieldName);
						continue;
					}

					std::optional<PropertyType> type = PropertyTypeFromString(fieldJson["Type"].get<std::string>());
					if (type == PropertyType::Enum)
						type = PropertyType::Int;
					if (!type)
					{
						ST_CORE_WARN("Script field '{}.{}' has an unsupported type; skipped", script.ClassName, fieldName);
						continue;
					}

					PropertyInfo info;
					info.Name = fieldName;
					info.Type = *type;
					std::string error;
					std::optional<PropertyValue> value = PropertyValueFromJson(info, fieldJson["Value"], &error);
					if (!value)
					{
						ST_CORE_WARN("{}; skipped", error);
						continue;
					}
					script.Fields.push_back(ScriptFieldValue { fieldName, *type, *value });
				}
			}
			return true;
		}

		// The entity and asset references of script fields: slot is the script, subslot the field.
		void VisitScriptReferences(const ScriptComponent& component, const ComponentExtraReferences::Visitor& visitor)
		{
			for (uint32_t script = 0; script < component.Scripts.size(); script++)
			{
				const std::vector<ScriptFieldValue>& fields = component.Scripts[script].Fields;
				for (uint32_t field = 0; field < fields.size(); field++)
				{
					const ScriptFieldValue& value = fields[field];
					if ((value.Type == PropertyType::Entity || value.Type == PropertyType::Asset) && std::holds_alternative<UUID>(value.Value))
						visitor(script, field, value.Type, std::get<UUID>(value.Value));
				}
			}
		}

		void SetScriptReference(ScriptComponent& component, uint32_t script, uint32_t field, UUID value)
		{
			if (script < component.Scripts.size() && field < component.Scripts[script].Fields.size())
				component.Scripts[script].Fields[field].Value = value;
		}

		// The same scripts with the same field overrides, entity fields of `a` mapped through `remap`.
		bool ScriptsEqual(const ScriptComponent& a, const ScriptComponent& b, const ComponentExtraReferences::Remap& remap)
		{
			if (a.Scripts.size() != b.Scripts.size())
				return false;
			for (size_t script = 0; script < a.Scripts.size(); script++)
			{
				const ScriptEntry& x = a.Scripts[script];
				const ScriptEntry& y = b.Scripts[script];
				if (x.ClassName != y.ClassName || x.Fields.size() != y.Fields.size())
					return false;
				for (size_t field = 0; field < x.Fields.size(); field++)
				{
					const ScriptFieldValue& first = x.Fields[field];
					const ScriptFieldValue& second = y.Fields[field];
					if (first.Name != second.Name || first.Type != second.Type)
						return false;
					PropertyValue value = first.Value;
					if (const UUID* entity = std::get_if<UUID>(&value); entity && first.Type == PropertyType::Entity)
						value = remap(*entity);
					if (value != second.Value)
						return false;
				}
			}
			return true;
		}

		void VisitScriptReferencesJson(const nlohmann::json& extra, const ComponentExtraReferences::JsonVisitor& visitor)
		{
			ScriptComponent scripts;
			if (!DeserializeScriptComponent(scripts, extra, nullptr))
				return;
			VisitScriptReferences(scripts, [&visitor](uint32_t, uint32_t, PropertyType type, UUID value) { visitor(type, value); });
		}

	}

	void RegisterScriptingComponents()
	{
		ComponentRegistry::Register<ScriptComponent>("Script")
			.Category("Scripting")
			.Description("C++ behaviours from the project's script module")
			.Extra(SerializeScriptComponent, DeserializeScriptComponent)
			.ExtraReferences(VisitScriptReferences, SetScriptReference, ScriptsEqual, VisitScriptReferencesJson);
	}

}
