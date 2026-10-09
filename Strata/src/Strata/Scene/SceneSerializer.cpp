#include "stpch.h"
#include "Strata/Scene/SceneSerializer.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scene/ComponentAccess.h"

#include <unordered_set>

namespace Strata
{

	namespace
	{

		nlohmann::json SerializeEntity(Entity entity, bool includeParent)
		{
			nlohmann::json json;
			json["ID"] = UUIDToJson(entity.GetUUID());
			const UUID parent = entity.GetComponent<RelationshipComponent>().Parent;
			if (includeParent && parent.IsValid())
				json["Parent"] = UUIDToJson(parent);
			json["Components"] = ComponentAccess::SerializeEntityComponents(entity);
			return json;
		}

		void CollectSubtree(Entity entity, std::vector<Entity>& outEntities, std::unordered_set<UUID>& visited)
		{
			if (!visited.insert(entity.GetUUID()).second)
				return;
			outEntities.push_back(entity);
			for (Entity child : entity.GetChildren())
				CollectSubtree(child, outEntities, visited);
		}

		void RemapEntityReferences(entt::registry& registry, entt::entity handle, const std::unordered_map<UUID, UUID>& mapping)
		{
			auto remap = [&mapping](UUID uuid) -> UUID
			{
				auto it = mapping.find(uuid);
				return it != mapping.end() ? it->second : uuid;
			};

			for (const ComponentInfo* info : ComponentRegistry::GetAll())
			{
				if (HasFlag(info->Flags, ComponentFlags::NoSerialize))
					continue;
				void* component = info->Get(registry, handle);
				if (!component)
					continue;

				for (const PropertyInfo& property : info->Properties)
				{
					if (property.Type != PropertyType::Entity)
						continue;
					const UUID current = std::get<UUID>(property.GetValue(component));
					const UUID remapped = remap(current);
					if (remapped != current)
						property.Setter(component, remapped);
				}
			}

			// Script fields are dynamic and not covered by the reflected properties above.
			if (ScriptComponent* scripts = registry.try_get<ScriptComponent>(handle))
			{
				for (ScriptEntry& script : scripts->Scripts)
				{
					for (ScriptFieldValue& field : script.Fields)
					{
						if (field.Type == PropertyType::Entity && std::holds_alternative<UUID>(field.Value))
							field.Value = remap(std::get<UUID>(field.Value));
					}
				}
			}
		}

	}

	nlohmann::json SceneSerializer::Serialize(const Scene& scene)
	{
		nlohmann::json settings;
		const glm::vec3& gravity = scene.GetSettings().Gravity;
		settings["Gravity"] = nlohmann::json::array({ FloatToJson(gravity.x), FloatToJson(gravity.y), FloatToJson(gravity.z) });
		settings["FixedTimestep"] = FloatToJson(scene.GetSettings().FixedTimestep);
		settings["MaxFixedStepsPerFrame"] = scene.GetSettings().MaxFixedStepsPerFrame;

		nlohmann::json entities = nlohmann::json::array();
		for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
			entities.push_back(SerializeEntity(entity, true));

		nlohmann::json json;
		json["Strata"] = { { "Format", "Scene" }, { "Version", c_FormatVersion } };
		json["Scene"] = { { "Name", scene.GetName() }, { "Settings", settings }, { "Entities", entities } };
		return json;
	}

	bool SceneSerializer::Deserialize(Scene& scene, const nlohmann::json& json, std::string* outError, std::vector<std::string>* outWarnings)
	{
		auto fail = [outError](const std::string& message)
		{
			if (outError)
				*outError = message;
			return false;
		};

		if (!json.is_object() || !json.contains("Strata") || !json.contains("Scene"))
			return fail("Not a Strata scene document (missing 'Strata' or 'Scene')");

		const nlohmann::json& header = json["Strata"];
		if (!header.is_object() || JsonUtils::GetString(header, "Format") != "Scene")
			return fail("Document format is not 'Scene'");
		const uint64_t version = JsonUtils::GetUInt(header, "Version", 0);
		if (version == 0 || version > c_FormatVersion)
			return fail(fmt::format("Unsupported scene format version {} (supported: 1-{})", version, c_FormatVersion));

		const nlohmann::json& sceneJson = json["Scene"];
		if (!sceneJson.is_object())
			return fail("'Scene' must be an object");
		if (scene.GetEntityCount() != 0)
			return fail("Scenes can only be deserialized into an empty scene");

		scene.SetName(JsonUtils::GetString(sceneJson, "Name", "Untitled"));
		if (const nlohmann::json* settings = JsonUtils::Find(sceneJson, "Settings"); settings && settings->is_object())
		{
			SceneSettings& sceneSettings = scene.GetSettings();
			if (const nlohmann::json* gravity = JsonUtils::Find(*settings, "Gravity"))
			{
				PropertyInfo gravityProperty;
				gravityProperty.Name = "Gravity";
				gravityProperty.Type = PropertyType::Vec3;
				if (std::optional<PropertyValue> value = PropertyValueFromJson(gravityProperty, *gravity))
					sceneSettings.Gravity = std::get<glm::vec3>(*value);
				else if (outWarnings)
					outWarnings->push_back("Scene settings: invalid 'Gravity' ignored");
			}
			const float fixedTimestep = JsonUtils::GetFloat(*settings, "FixedTimestep", sceneSettings.FixedTimestep);
			if (fixedTimestep > 0.0f && fixedTimestep <= 1.0f)
				sceneSettings.FixedTimestep = fixedTimestep;
			sceneSettings.MaxFixedStepsPerFrame = static_cast<uint32_t>(std::clamp<uint64_t>(JsonUtils::GetUInt(*settings, "MaxFixedStepsPerFrame", sceneSettings.MaxFixedStepsPerFrame), 1, 64));
		}

		// A scene without an entity list is simply empty.
		if (!sceneJson.contains("Entities"))
			return true;

		EntityInstantiationOptions options;
		options.GenerateNewUUIDs = false;
		std::string error;
		DeserializeEntities(scene, sceneJson, options, &error, outWarnings);
		if (!error.empty())
			return fail(error);
		return true;
	}

	bool SceneSerializer::SaveToFile(const Scene& scene, const std::filesystem::path& path, std::string* outError)
	{
		const std::string text = JsonUtils::Dump(Serialize(scene), 1, '\t');
		if (!FileSystem::WriteText(path, text + "\n"))
		{
			if (outError)
				*outError = fmt::format("Failed to write scene file '{}'", FileSystem::ToUTF8(path));
			return false;
		}
		return true;
	}

	Ref<Scene> SceneSerializer::LoadFromFile(const std::filesystem::path& path, std::string* outError)
	{
		const std::optional<std::string> text = FileSystem::ReadText(path);
		if (!text)
		{
			if (outError)
				*outError = fmt::format("Failed to read scene file '{}'", FileSystem::ToUTF8(path));
			return nullptr;
		}

		std::optional<nlohmann::json> json = JsonUtils::Parse(*text);
		if (!json)
		{
			if (outError)
				*outError = fmt::format("Scene file '{}' is not valid JSON", FileSystem::ToUTF8(path));
			return nullptr;
		}

		Ref<Scene> scene = CreateRef<Scene>();
		std::vector<std::string> warnings;
		if (!Deserialize(*scene, *json, outError, &warnings))
			return nullptr;

		for (const std::string& warning : warnings)
			ST_CORE_WARN("Scene '{}': {}", FileSystem::ToUTF8(path), warning);
		return scene;
	}

	nlohmann::json SceneSerializer::SerializeEntities(const Scene& scene, const std::vector<Entity>& roots)
	{
		std::vector<Entity> entities;
		std::unordered_set<UUID> visited;
		for (const Entity root : roots)
		{
			// Skip roots nested inside another listed root; they are serialized as part of that subtree.
			bool nested = false;
			for (const Entity other : roots)
				nested |= other != root && scene.IsDescendantOf(root, other);
			if (root.IsValid() && !nested)
				CollectSubtree(root, entities, visited);
		}

		nlohmann::json array = nlohmann::json::array();
		for (const Entity entity : entities)
		{
			// Parent links are only kept within the snapshot; top-level entities get re-parented on instantiation.
			const UUID parent = entity.GetComponent<RelationshipComponent>().Parent;
			array.push_back(SerializeEntity(entity, visited.count(parent) > 0));
		}
		return nlohmann::json { { "Entities", array } };
	}

	std::vector<Entity> SceneSerializer::DeserializeEntities(Scene& scene, const nlohmann::json& json, const EntityInstantiationOptions& options,
		std::string* outError, std::vector<std::string>* outWarnings)
	{
		std::vector<Entity> roots;
		auto fail = [&](const std::string& message)
		{
			if (outError)
				*outError = message;
			return std::vector<Entity>();
		};

		if (!json.is_object() || !json.contains("Entities") || !json["Entities"].is_array())
			return fail("Entity data must be an object with an 'Entities' array");
		if (options.Parent.IsValid() && options.Parent.GetScene() != &scene)
			return fail("Parent entity belongs to a different scene");

		const nlohmann::json& entities = json["Entities"];

		// Pass 1: validate ids and build the old -> new UUID mapping.
		std::unordered_map<UUID, UUID> mapping;
		std::vector<UUID> sourceIds;
		sourceIds.reserve(entities.size());
		for (const nlohmann::json& entityJson : entities)
		{
			if (!entityJson.is_object())
				return fail("Entity entries must be objects");

			const std::optional<UUID> id = entityJson.contains("ID") ? UUIDFromJson(entityJson["ID"]) : std::optional<UUID>(UUID());
			if (!id || !id->IsValid())
				return fail("Entity has an invalid 'ID'");
			if (mapping.count(*id))
				return fail(fmt::format("Duplicate entity ID {} in entity data", id->ToString()));

			UUID target = *id;
			if (options.GenerateNewUUIDs || scene.GetEntityByUUID(target).IsValid())
				target = UUID();
			mapping.emplace(*id, target);
			sourceIds.push_back(*id);
		}

		// Pass 2: create entities and components. Entities are created directly (not through CreateEntity) so
		// building large hierarchies stays linear: nothing is appended to the root list and then removed again.
		entt::registry& registry = scene.m_Registry;
		std::vector<entt::entity> handles;
		handles.reserve(entities.size());
		for (size_t index = 0; index < entities.size(); index++)
		{
			const nlohmann::json& entityJson = entities[index];
			const UUID uuid = mapping.at(sourceIds[index]);

			const entt::entity handle = registry.create();
			registry.emplace<IDComponent>(handle, uuid);
			registry.emplace<NameComponent>(handle, "Entity");
			registry.emplace<TransformComponent>(handle);
			registry.emplace<RelationshipComponent>(handle);
			registry.emplace<WorldTransformComponent>(handle);
			scene.m_EntityMap.emplace(uuid, handle);
			handles.push_back(handle);

			if (entityJson.contains("Components"))
			{
				const nlohmann::json& components = entityJson["Components"];
				if (!components.is_object())
				{
					if (outWarnings)
						outWarnings->push_back(fmt::format("Entity {}: 'Components' must be an object", sourceIds[index].ToString()));
					continue;
				}

				for (const auto& [componentName, componentJson] : components.items())
				{
					const ComponentInfo* info = ComponentRegistry::Find(componentName);
					if (!info || HasFlag(info->Flags, ComponentFlags::NoSerialize))
					{
						if (outWarnings)
							outWarnings->push_back(fmt::format("Entity {}: unknown component '{}' skipped", sourceIds[index].ToString(), componentName));
						continue;
					}

					void* component = info->Add(registry, handle);
					std::string error;
					if (!ComponentAccess::Deserialize(*info, component, componentJson, false, &error, outWarnings) && outWarnings)
						outWarnings->push_back(fmt::format("Entity {}: {}", sourceIds[index].ToString(), error));
				}
			}
		}

		// Pass 3: hierarchy. Parent links are resolved within the snapshot; a link that would close a cycle
		// (possible in hand-written data) is dropped and the entity is attached at the top level instead.
		std::unordered_map<UUID, UUID> acceptedParents; // Source id -> source parent id
		auto createsCycle = [&](UUID child, UUID parent)
		{
			UUID current = parent;
			for (size_t steps = 0; steps <= entities.size() && current.IsValid(); steps++)
			{
				if (current == child)
					return true;
				auto it = acceptedParents.find(current);
				current = it != acceptedParents.end() ? it->second : UUID::Null();
			}
			return false;
		};

		for (size_t index = 0; index < entities.size(); index++)
		{
			const nlohmann::json& entityJson = entities[index];
			const UUID sourceId = sourceIds[index];
			std::optional<UUID> parentSource = entityJson.contains("Parent") ? UUIDFromJson(entityJson["Parent"]) : std::optional<UUID>(UUID::Null());
			if (!parentSource)
			{
				if (outWarnings)
					outWarnings->push_back(fmt::format("Entity {}: invalid 'Parent'; attached at the top level", sourceId.ToString()));
				continue;
			}

			if (parentSource->IsValid() && mapping.count(*parentSource))
			{
				if (createsCycle(sourceId, *parentSource))
				{
					if (outWarnings)
						outWarnings->push_back(fmt::format("Entity {}: parent {} would create a cycle; attached at the top level", sourceId.ToString(), parentSource->ToString()));
				}
				else
				{
					acceptedParents.emplace(sourceId, *parentSource);
				}
			}
			else if (parentSource->IsValid() && outWarnings && !options.Parent.IsValid())
			{
				outWarnings->push_back(fmt::format("Entity {}: parent {} not found; attached at the top level", sourceId.ToString(), parentSource->ToString()));
			}
		}

		for (size_t index = 0; index < entities.size(); index++)
		{
			const entt::entity handle = handles[index];
			const UUID uuid = mapping.at(sourceIds[index]);

			entt::entity parentHandle = entt::null;
			auto accepted = acceptedParents.find(sourceIds[index]);
			if (accepted != acceptedParents.end())
			{
				parentHandle = scene.m_EntityMap.at(mapping.at(accepted->second));
			}
			else
			{
				roots.emplace_back(handle, &scene);
				if (options.Parent.IsValid())
					parentHandle = options.Parent.GetHandle();
			}

			RelationshipComponent& relationship = registry.get<RelationshipComponent>(handle);
			if (parentHandle != entt::null)
			{
				relationship.Parent = registry.get<IDComponent>(parentHandle).ID;
				registry.get<RelationshipComponent>(parentHandle).Children.push_back(uuid);
			}
			else
			{
				relationship.Parent = UUID::Null();
				scene.m_RootEntities.push_back(uuid);
			}
		}

		// Pass 4: entity references between the created entities point at the new UUIDs.
		std::unordered_map<UUID, UUID> remapping;
		for (const auto& [source, target] : mapping)
		{
			if (source != target)
				remapping.emplace(source, target);
		}
		for (size_t index = 0; index < handles.size(); index++)
		{
			if (!remapping.empty())
				RemapEntityReferences(registry, handles[index], remapping);
			// Link prefab instances to the prefab's own entity ids (the ids in the snapshot).
			if (options.SourcePrefab.IsValid())
				registry.emplace_or_replace<PrefabInstanceComponent>(handles[index], PrefabInstanceComponent { options.SourcePrefab, sourceIds[index] });
		}

		// Components were constructed with default values and filled in afterwards; now that data and hierarchy
		// are complete, notify systems (on_update) so they see the final state, e.g. when spawning during play.
		for (const entt::entity handle : handles)
		{
			for (const ComponentInfo* info : ComponentRegistry::GetAll())
			{
				if (info->Has(registry, handle))
					info->MarkModified(registry, handle);
			}
		}

		return roots;
	}

}
