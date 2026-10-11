#include "stpch.h"
#include "Strata/Scene/SceneSerializer.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/UnknownComponents.h"

#include <algorithm>
#include <optional>
#include <string>
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
			nlohmann::json components = ComponentAccess::SerializeEntityComponents(entity);
			// Components this build does not register are written back as they were read.
			if (const UnknownComponentsComponent* unknown = entity.TryGetComponent<UnknownComponentsComponent>())
				unknown->AppendTo(components);
			json["Components"] = std::move(components);
			return json;
		}

		void CollectSubtree(Entity root, std::vector<Entity>& outEntities, std::unordered_set<UUID>& visited)
		{
			// Parents before children, iteratively (hierarchies can be arbitrarily deep).
			std::vector<Entity> stack = { root };
			while (!stack.empty())
			{
				const Entity entity = stack.back();
				stack.pop_back();
				if (!visited.insert(entity.GetUUID()).second)
					continue;
				outEntities.push_back(entity);
				const std::vector<Entity> children = entity.GetChildren();
				stack.insert(stack.end(), children.rbegin(), children.rend());
			}
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

	namespace
	{

		// Reads one capacity: an integer from 0 (automatic) to `limit`; anything else is reported and leaves the capacity
		// automatic, a larger value is reported and clamped.
		uint32_t ReadCapacity(const nlohmann::json& capacity, std::string_view key, uint32_t limit, std::vector<std::string>* outWarnings)
		{
			const nlohmann::json* value = JsonUtils::Find(capacity, key);
			if (!value)
				return 0;
			if (!value->is_number_integer() || (!value->is_number_unsigned() && value->get<int64_t>() < 0))
			{
				if (outWarnings)
					outWarnings->push_back(fmt::format("Scene settings: invalid 'PhysicsCapacity.{}' ignored (automatic)", key));
				return 0;
			}
			const uint64_t requested = value->get<uint64_t>();
			if (requested > limit)
			{
				if (outWarnings)
					outWarnings->push_back(fmt::format("Scene settings: 'PhysicsCapacity.{}' of {} is limited to {}", key, requested, limit));
				return limit;
			}
			return static_cast<uint32_t>(requested);
		}

		void ReadPhysicsCapacity(const nlohmann::json& capacity, PhysicsCapacitySettings& outCapacity, std::vector<std::string>* outWarnings)
		{
			if (!capacity.is_object())
			{
				if (outWarnings)
					outWarnings->push_back("Scene settings: invalid 'PhysicsCapacity' ignored (automatic)");
				return;
			}
			outCapacity.MaxBodies = ReadCapacity(capacity, "MaxBodies", PhysicsCapacitySettings::c_MaxBodiesLimit, outWarnings);
			outCapacity.MaxBodyPairs = ReadCapacity(capacity, "MaxBodyPairs", PhysicsCapacitySettings::c_MaxBodyPairsLimit, outWarnings);
			outCapacity.MaxContactConstraints = ReadCapacity(capacity, "MaxContactConstraints", PhysicsCapacitySettings::c_MaxContactConstraintsLimit, outWarnings);
		}

		// {"Names": {"<index>": "<name>", ...}, "NoCollisions": [[a, b], ...]}: the named layers, and the pairs of layers that
		// do not collide (each once, a <= b), so a scene where everything collides stores an empty list.
		nlohmann::json WritePhysicsLayers(const PhysicsLayerSettings& layers)
		{
			nlohmann::json names = nlohmann::json::object();
			for (uint32_t layer = 0; layer < c_PhysicsLayerCount; layer++)
			{
				if (!layers.Names[layer].empty())
					names[std::to_string(layer)] = layers.Names[layer];
			}
			nlohmann::json noCollisions = nlohmann::json::array();
			for (uint32_t a = 0; a < c_PhysicsLayerCount; a++)
			{
				for (uint32_t b = a; b < c_PhysicsLayerCount; b++)
				{
					if (!layers.DoCollide(a, b))
						noCollisions.push_back({ a, b });
				}
			}
			return { { "Names", std::move(names) }, { "NoCollisions", std::move(noCollisions) } };
		}

		// A layer index: an integer from 0 to c_PhysicsLayerCount - 1 (parsed files hold unsigned numbers, documents built in
		// code signed ones).
		std::optional<uint32_t> ReadLayerIndex(const nlohmann::json& value)
		{
			if (!value.is_number_integer())
				return std::nullopt;
			const bool inRange = value.is_number_unsigned() ? value.get<uint64_t>() < c_PhysicsLayerCount
				: (value.get<int64_t>() >= 0 && value.get<int64_t>() < static_cast<int64_t>(c_PhysicsLayerCount));
			if (!inRange)
				return std::nullopt;
			return static_cast<uint32_t>(value.get<int64_t>());
		}

		// What a file holds that is not a layer name or pair is reported and skipped; the rest of the settings still load.
		void ReadPhysicsLayers(const nlohmann::json& json, PhysicsLayerSettings& outLayers, std::vector<std::string>* outWarnings)
		{
			auto warn = [outWarnings](std::string message)
			{
				if (outWarnings)
					outWarnings->push_back(std::move(message));
			};
			if (!json.is_object())
			{
				warn("Scene settings: invalid 'PhysicsLayers' ignored (every layer collides)");
				return;
			}
			if (const nlohmann::json* names = JsonUtils::Find(json, "Names"))
			{
				if (!names->is_object())
				{
					warn("Scene settings: invalid 'PhysicsLayers.Names' ignored");
				}
				else
				{
					for (const auto& [key, name] : names->items())
					{
						const bool digits = !key.empty() && key.size() <= 2 && std::all_of(key.begin(), key.end(), [](char character) { return character >= '0' && character <= '9'; });
						uint32_t layer = digits ? 0 : c_PhysicsLayerCount;
						for (size_t index = 0; digits && index < key.size(); index++)
							layer = layer * 10 + static_cast<uint32_t>(key[index] - '0');
						if (layer >= c_PhysicsLayerCount || !name.is_string())
						{
							warn(fmt::format("Scene settings: physics layer name '{}' ignored: names are given by layer index (0 to {})", key, c_PhysicsLayerCount - 1));
							continue;
						}
						if (std::optional<std::string> problem = outLayers.ValidateName(layer, name.get<std::string>()))
						{
							warn(fmt::format("Scene settings: the name of physics layer {} is ignored: {}", layer, *problem));
							continue;
						}
						outLayers.Names[layer] = name.get<std::string>();
					}
				}
			}
			if (const nlohmann::json* pairs = JsonUtils::Find(json, "NoCollisions"))
			{
				if (!pairs->is_array())
				{
					warn("Scene settings: invalid 'PhysicsLayers.NoCollisions' ignored (every layer collides)");
					return;
				}
				for (const nlohmann::json& pair : *pairs)
				{
					const std::optional<uint32_t> a = pair.is_array() && pair.size() == 2 ? ReadLayerIndex(pair[0]) : std::nullopt;
					const std::optional<uint32_t> b = pair.is_array() && pair.size() == 2 ? ReadLayerIndex(pair[1]) : std::nullopt;
					if (!a || !b)
					{
						warn(fmt::format("Scene settings: physics layer pair {} ignored: pairs are two layer indices (0 to {})", pair.dump(), c_PhysicsLayerCount - 1));
						continue;
					}
					outLayers.SetCollide(*a, *b, false);
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
		const PhysicsCapacitySettings& capacity = scene.GetSettings().PhysicsCapacity;
		settings["PhysicsCapacity"] = {
			{ "MaxBodies", capacity.MaxBodies },
			{ "MaxBodyPairs", capacity.MaxBodyPairs },
			{ "MaxContactConstraints", capacity.MaxContactConstraints } };
		settings["PhysicsLayers"] = WritePhysicsLayers(scene.GetSettings().PhysicsLayers);

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
			// Scenes saved before capacities existed have none: every capacity is automatic.
			if (const nlohmann::json* capacity = JsonUtils::Find(*settings, "PhysicsCapacity"))
				ReadPhysicsCapacity(*capacity, sceneSettings.PhysicsCapacity, outWarnings);
			// Scenes saved before layer settings existed have none: no names, every layer collides. (Older readers skip the
			// key: they misread nothing, so the format version stays.)
			if (const nlohmann::json* layers = JsonUtils::Find(*settings, "PhysicsLayers"))
				ReadPhysicsLayers(*layers, sceneSettings.PhysicsLayers, outWarnings);
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
		std::unordered_set<UUID> listed;
		for (const Entity root : roots)
		{
			if (root.IsValid() && root.GetScene() == &scene)
				listed.insert(root.GetUUID());
		}
		for (const Entity root : roots)
		{
			// Skip roots nested inside another listed root; they are serialized as part of that subtree. Looking at each root's
			// ancestors keeps this linear in the number of roots (copying a selection of 100,000 entities).
			if (!root.IsValid() || root.GetScene() != &scene)
				continue;
			bool nested = false;
			for (Entity ancestor = root.GetParent(); ancestor && !nested; ancestor = ancestor.GetParent())
				nested = listed.contains(ancestor.GetUUID());
			if (!nested)
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
		const size_t existing = std::min(scene.GetRegistryEntityCount(), Scene::c_MaxEntities);
		if (entities.size() > Scene::c_MaxEntities - existing)
			return fail(fmt::format("Cannot add {} entities to a scene with {}: a scene holds at most {} entities", entities.size(), existing, Scene::c_MaxEntities));

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
		// Components whose names this build does not register are kept verbatim (UnknownComponentsComponent) and reported
		// once per name.
		entt::registry& registry = scene.m_Registry;
		std::vector<entt::entity> handles;
		handles.reserve(entities.size());
		std::vector<std::string> unknownNames; // In the order they were first seen
		std::unordered_map<std::string, size_t> unknownCounts;
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

				nlohmann::json unknown = nlohmann::json::object();
				for (const auto& [componentName, componentJson] : components.items())
				{
					const ComponentInfo* info = ComponentRegistry::Find(componentName);
					if (!info)
					{
						unknown[componentName] = componentJson;
						auto [count, firstSeen] = unknownCounts.try_emplace(componentName, 0);
						if (firstSeen)
							unknownNames.push_back(componentName);
						count->second++;
						continue;
					}
					if (HasFlag(info->Flags, ComponentFlags::NoSerialize))
					{
						if (outWarnings)
							outWarnings->push_back(fmt::format("Entity {}: component '{}' is not read from entity data; skipped", sourceIds[index].ToString(), componentName));
						continue;
					}

					void* component = info->Add(registry, handle);
					std::string error;
					if (!ComponentAccess::Deserialize(*info, component, componentJson, false, &error, outWarnings) && outWarnings)
						outWarnings->push_back(fmt::format("Entity {}: {}", sourceIds[index].ToString(), error));
				}
				if (!unknown.empty())
					registry.emplace<UnknownComponentsComponent>(handle, UnknownComponentsComponent { std::move(unknown) });
			}
		}

		if (outWarnings && options.ReportUnknownComponents)
		{
			for (const std::string& name : unknownNames)
			{
				const size_t count = unknownCounts.at(name);
				outWarnings->push_back(fmt::format("Unknown component '{}' ({} {}) kept unchanged: no module of this build registers it", name, count,
					count == 1 ? "entity" : "entities"));
			}
		}

		// Pass 3: hierarchy. Parent links are resolved within the snapshot; a link that would close a cycle
		// (possible in hand-written data) is dropped and the entity is attached at the top level instead.
		std::unordered_map<UUID, UUID> acceptedParents; // Source id -> source parent id
		// Accepted links form a forest in which each child is still a root when its link is considered (ids are unique),
		// so a link closes a cycle exactly when both ends are already connected. Union-find answers that in near-constant
		// time, keeping deep hierarchies linear to load.
		std::unordered_map<UUID, UUID> connected; // Union-find parent; ids without an entry are their own representative
		auto findRepresentative = [&](UUID id)
		{
			UUID representative = id;
			for (auto it = connected.find(representative); it != connected.end(); it = connected.find(representative))
				representative = it->second;
			while (id != representative) // Path compression
			{
				UUID& next = connected[id];
				id = std::exchange(next, representative);
			}
			return representative;
		};
		auto createsCycle = [&](UUID child, UUID parent)
		{
			const UUID childRepresentative = findRepresentative(child);
			const UUID parentRepresentative = findRepresentative(parent);
			if (childRepresentative == parentRepresentative)
				return true;
			connected[childRepresentative] = parentRepresentative;
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

		// The runtime links (HierarchyComponent) are built now that every entity exists: children may be listed before their
		// parents. Linking also fills the RelationshipComponents and the root list.
		for (const entt::entity handle : handles)
			registry.emplace<HierarchyComponent>(handle);
		std::vector<entt::entity> rootHandles;
		for (size_t index = 0; index < entities.size(); index++)
		{
			const entt::entity handle = handles[index];

			entt::entity parentHandle = entt::null;
			auto accepted = acceptedParents.find(sourceIds[index]);
			if (accepted != acceptedParents.end())
			{
				parentHandle = scene.m_EntityMap.at(mapping.at(accepted->second));
			}
			else
			{
				roots.emplace_back(handle, &scene);
				rootHandles.push_back(handle);
				if (options.Parent.IsValid())
					parentHandle = options.Parent.GetHandle();
			}
			scene.LinkLast(handle, parentHandle);
		}
		// Depths, activity and the (stale) world transforms of the new subtrees.
		scene.FinishLinking(rootHandles);
		scene.m_HierarchyVersion++;

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
