#include "stpch.h"
#include "Strata/Scene/PrefabInstances.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"

#include <unordered_set>

namespace Strata
{

	namespace
	{

		constexpr const char* c_LinkComponent = "PrefabInstance";

		// The id of a snapshot entity (null when it has none).
		UUID GetId(const nlohmann::json& entity)
		{
			const nlohmann::json* id = JsonUtils::Find(entity, "ID");
			return id ? UUIDFromJson(*id).value_or(UUID::Null()) : UUID::Null();
		}

		UUID GetParentId(const nlohmann::json& entity)
		{
			const nlohmann::json* parent = JsonUtils::Find(entity, "Parent");
			return parent ? UUIDFromJson(*parent).value_or(UUID::Null()) : UUID::Null();
		}

		// Replaces every string of the values that names an entity of the mapping (entity references are UUIDs written as
		// strings) by the entity it maps to.
		void RemapStrings(nlohmann::json& value, const std::unordered_map<std::string, std::string>& mapping)
		{
			std::vector<nlohmann::json*> pending = { &value };
			while (!pending.empty())
			{
				nlohmann::json& current = *pending.back();
				pending.pop_back();
				if (current.is_string())
				{
					const auto found = mapping.find(current.get_ref<const std::string&>());
					if (found != mapping.end())
						current = found->second;
				}
				else if (current.is_object() || current.is_array())
				{
					for (nlohmann::json& child : current)
						pending.push_back(&child);
				}
			}
		}

		std::string ToText(UUID id)
		{
			return UUIDToJson(id).get<std::string>();
		}

		// The instance's entities (the root's subtree), depth first.
		std::vector<Entity> CollectSubtree(Entity root)
		{
			std::vector<Entity> entities;
			std::vector<Entity> stack = { root };
			while (!stack.empty())
			{
				const Entity entity = stack.back();
				stack.pop_back();
				entities.push_back(entity);
				const std::vector<Entity> children = entity.GetChildren();
				stack.insert(stack.end(), children.rbegin(), children.rend());
			}
			return entities;
		}

		// The template entity ids of an instance's linked entities -> their UUIDs.
		std::unordered_map<UUID, UUID> MapLinkedEntities(Entity root)
		{
			std::unordered_map<UUID, UUID> linked;
			const AssetHandle prefab = root.GetComponent<PrefabInstanceComponent>().Prefab;
			for (const Entity entity : CollectSubtree(root))
			{
				const PrefabInstanceComponent* link = entity.TryGetComponent<PrefabInstanceComponent>();
				if (link && link->Prefab == prefab && link->PrefabEntityID.IsValid())
					linked.emplace(link->PrefabEntityID, entity.GetUUID());
			}
			return linked;
		}

	}

	namespace PrefabInstances
	{

		bool IsInstanceRoot(Entity entity)
		{
			if (!entity)
				return false;
			const PrefabInstanceComponent* link = entity.TryGetComponent<PrefabInstanceComponent>();
			if (!link)
				return false;
			const Entity parent = entity.GetParent();
			const PrefabInstanceComponent* parentLink = parent ? parent.TryGetComponent<PrefabInstanceComponent>() : nullptr;
			return !parentLink || parentLink->Prefab != link->Prefab;
		}

		Entity FindInstanceRoot(Entity entity)
		{
			if (!entity || !entity.HasComponent<PrefabInstanceComponent>())
				return {};
			Entity current = entity;
			while (!IsInstanceRoot(current))
				current = current.GetParent();
			return current;
		}

		AssetHandle GetPrefab(Entity entity)
		{
			const PrefabInstanceComponent* link = entity ? entity.TryGetComponent<PrefabInstanceComponent>() : nullptr;
			return link ? link->Prefab : UUID::Null();
		}

		std::vector<Entity> FindInstanceRoots(Scene& scene, AssetHandle prefab)
		{
			std::vector<Entity> roots;
			auto view = scene.GetRegistry().view<PrefabInstanceComponent>();
			for (const entt::entity handle : view)
			{
				const Entity entity(handle, &scene);
				if (view.get<PrefabInstanceComponent>(handle).Prefab == prefab && IsInstanceRoot(entity))
					roots.push_back(entity);
			}
			return roots;
		}

		nlohmann::json ExtractTemplate(const nlohmann::json& snapshot, UUID templateRoot)
		{
			nlohmann::json part = nlohmann::json::array();
			const nlohmann::json* entities = JsonUtils::Find(snapshot, "Entities");
			if (entities && entities->is_array())
			{
				// Parents come before their children: one pass collects the subtree.
				std::unordered_set<UUID> members;
				for (const nlohmann::json& entity : *entities)
				{
					const UUID id = GetId(entity);
					if (id == templateRoot)
					{
						members.insert(id);
						nlohmann::json root = entity;
						root.erase("Parent");
						part.push_back(std::move(root));
					}
					else if (!members.empty() && members.contains(GetParentId(entity)))
					{
						members.insert(id);
						part.push_back(entity);
					}
				}
			}
			return nlohmann::json { { "Entities", std::move(part) } };
		}

		nlohmann::json MakeTemplate(const Scene& scene, Entity root, const nlohmann::json* rootTransform, const nlohmann::json* rootName,
			std::unordered_map<UUID, UUID>& outIds)
		{
			nlohmann::json snapshot = SceneSerializer::SerializeEntities(scene, { root });
			nlohmann::json& entities = snapshot["Entities"];
			const AssetHandle prefab = GetPrefab(root);

			// The prefab's id of each entity: its link's, or a new one (an entity added to the instance, or one whose id another
			// entity of the instance has, as a copy within it).
			std::unordered_set<UUID> used;
			std::unordered_map<std::string, std::string> remap;
			for (const nlohmann::json& entity : entities)
			{
				const UUID id = GetId(entity);
				const Entity sceneEntity = scene.GetEntityByUUID(id);
				const PrefabInstanceComponent* link = sceneEntity ? sceneEntity.TryGetComponent<PrefabInstanceComponent>() : nullptr;
				UUID templateId = link && link->Prefab == prefab ? link->PrefabEntityID : UUID::Null();
				if (!templateId.IsValid() || !used.insert(templateId).second)
				{
					do
						templateId = UUID();
					while (!used.insert(templateId).second);
				}
				outIds[id] = templateId;
				remap.emplace(ToText(id), ToText(templateId));
			}

			for (nlohmann::json& entity : entities)
			{
				if (nlohmann::json* components = entity.contains("Components") ? &entity["Components"] : nullptr; components && components->is_object())
					components->erase(c_LinkComponent);
				RemapStrings(entity, remap);
			}
			if (!entities.empty())
			{
				nlohmann::json& rootEntity = entities.front();
				rootEntity.erase("Parent");
				if (rootTransform)
					rootEntity["Components"]["Transform"] = *rootTransform;
				if (rootName)
					rootEntity["Components"]["Name"] = *rootName;
			}
			return snapshot;
		}

		PrefabInstanceDifference Compare(const Scene& scene, Entity root, const nlohmann::json& snapshot)
		{
			PrefabInstanceDifference difference;
			const UUID templateRoot = root.GetComponent<PrefabInstanceComponent>().PrefabEntityID;
			const nlohmann::json part = ExtractTemplate(snapshot, templateRoot);
			std::unordered_map<UUID, const nlohmann::json*> templates;
			for (const nlohmann::json& entity : part["Entities"])
				templates.emplace(GetId(entity), &entity);

			// The instance as a template, with the prefab's own root transform and name: those never count.
			const nlohmann::json* rootTemplate = templates.contains(templateRoot) ? templates.at(templateRoot) : nullptr;
			const nlohmann::json* rootComponents = rootTemplate ? JsonUtils::Find(*rootTemplate, "Components") : nullptr;
			const nlohmann::json* rootTransform = rootComponents ? JsonUtils::Find(*rootComponents, "Transform") : nullptr;
			const nlohmann::json* rootName = rootComponents ? JsonUtils::Find(*rootComponents, "Name") : nullptr;
			std::unordered_map<UUID, UUID> ids;
			const nlohmann::json instance = MakeTemplate(scene, root, rootTransform, rootName, ids);
			std::unordered_map<UUID, UUID> entityOf; // Template id -> instance UUID
			for (const auto& [entity, templateId] : ids)
				entityOf.emplace(templateId, entity);

			std::unordered_set<UUID> seen;
			for (const nlohmann::json& entity : instance["Entities"])
			{
				const UUID templateId = GetId(entity);
				seen.insert(templateId);
				const auto found = templates.find(templateId);
				if (found == templates.end())
					difference.Added.push_back(entityOf.at(templateId));
				else if (*found->second != entity)
					difference.Changed.push_back(entityOf.at(templateId));
			}
			for (const nlohmann::json& entity : part["Entities"])
			{
				if (!seen.contains(GetId(entity)))
					difference.Removed.push_back(GetId(entity));
			}
			return difference;
		}

		bool Replace(Scene& scene, Entity root, const nlohmann::json& snapshot, std::vector<UUID>* outCreated, std::string* outError)
		{
			auto fail = [outError](std::string message)
			{
				if (outError)
					*outError = std::move(message);
				return false;
			};

			const PrefabInstanceComponent link = root.GetComponent<PrefabInstanceComponent>();
			nlohmann::json part = ExtractTemplate(snapshot, link.PrefabEntityID);
			nlohmann::json& entities = part["Entities"];
			if (entities.empty())
				return fail("The prefab no longer has the part this instance comes from");

			// Each template entity becomes the instance's entity of that id (its UUID kept), or a new entity.
			const std::unordered_map<UUID, UUID> linked = MapLinkedEntities(root);
			const std::vector<Entity> subtree = CollectSubtree(root);
			if (scene.GetEntityCount() - subtree.size() + entities.size() > Scene::c_MaxEntities)
				return fail(fmt::format("The scene is full: it holds at most {} entities", Scene::c_MaxEntities));
			std::unordered_map<std::string, std::string> remap;
			std::vector<UUID> created;
			std::unordered_set<UUID> templateIds;
			for (const nlohmann::json& entity : entities)
			{
				// Checked here so that nothing changes when the prefab's data is broken (DeserializeEntities would refuse it
				// after the instance is gone).
				const UUID templateId = GetId(entity);
				if (!templateId.IsValid() || !templateIds.insert(templateId).second)
					return fail("The prefab's part this instance comes from has entities with missing or repeated ids");
				const auto found = linked.find(templateId);
				UUID target = found != linked.end() ? found->second : UUID::Null();
				if (!target.IsValid())
				{
					do
						target = UUID();
					while (scene.GetEntityByUUID(target));
					created.push_back(target);
				}
				remap.emplace(ToText(templateId), ToText(target));
			}
			for (nlohmann::json& entity : entities)
			{
				const UUID templateId = GetId(entity);
				RemapStrings(entity, remap);
				entity["Components"][c_LinkComponent] = { { "Prefab", UUIDToJson(link.Prefab) }, { "PrefabEntityID", templateId.ToString() } };
			}
			// What belongs to the instance: where it stands and what it is called.
			const nlohmann::json rootComponents = SceneSerializer::SerializeEntities(scene, { root })["Entities"][0]["Components"];
			if (const nlohmann::json* transform = JsonUtils::Find(rootComponents, "Transform"))
				entities[0]["Components"]["Transform"] = *transform;
			if (const nlohmann::json* name = JsonUtils::Find(rootComponents, "Name"))
				entities[0]["Components"]["Name"] = *name;

			const Entity parent = root.GetParent();
			const UUID parentId = parent ? parent.GetUUID() : UUID::Null();
			const size_t siblingIndex = scene.GetSiblingIndex(root);
			const UUID rootId = root.GetUUID();
			// The instance as it was, put back should the prefab's part not instantiate.
			const nlohmann::json previous = SceneSerializer::SerializeEntities(scene, { root });
			scene.DestroyEntities(std::vector<Entity> { root });

			EntityInstantiationOptions options;
			options.GenerateNewUUIDs = false;
			options.Parent = parent;
			options.ReportUnknownComponents = false;
			auto instantiate = [&](const nlohmann::json& data, std::string& error)
			{
				const std::vector<Entity> roots = SceneSerializer::DeserializeEntities(scene, data, options, &error);
				if (roots.size() != 1)
				{
					if (error.empty())
						error = "the prefab's part has no single root";
					if (!roots.empty())
						scene.DestroyEntities(roots);
					return false;
				}
				const Scene::EntityPlacement placement { rootId, parentId, siblingIndex };
				return scene.PlaceEntities(std::span<const Scene::EntityPlacement>(&placement, 1));
			};
			std::string error;
			if (!instantiate(part, error))
			{
				std::string restoreError;
				if (!instantiate(previous, restoreError))
					ST_CORE_ERROR("An instance whose prefab could not replace it could not be restored either: {}", restoreError);
				return fail(fmt::format("Rebuilding the instance failed: {}", error.empty() ? std::string("it could not be placed") : error));
			}
			if (outCreated)
				*outCreated = std::move(created);
			return true;
		}

		nlohmann::json ReplaceTemplate(const nlohmann::json& snapshot, UUID templateRoot, const nlohmann::json& part)
		{
			nlohmann::json result = { { "Entities", nlohmann::json::array() } };
			nlohmann::json& entities = result["Entities"];
			const nlohmann::json* source = JsonUtils::Find(snapshot, "Entities");
			const nlohmann::json* replacement = JsonUtils::Find(part, "Entities");
			bool inserted = false;
			std::unordered_set<UUID> removed;
			if (source && source->is_array())
			{
				for (const nlohmann::json& entity : *source)
				{
					const UUID id = GetId(entity);
					if (id == templateRoot || (!removed.empty() && removed.contains(GetParentId(entity))))
					{
						removed.insert(id);
						// The new part goes where the old one was.
						if (!inserted && replacement)
						{
							for (const nlohmann::json& added : *replacement)
								entities.push_back(added);
							inserted = true;
						}
						continue;
					}
					entities.push_back(entity);
				}
			}
			if (!inserted && replacement)
			{
				for (const nlohmann::json& added : *replacement)
					entities.push_back(added);
			}
			// Other keys of the snapshot stay as they were.
			for (const auto& [key, value] : snapshot.items())
			{
				if (key != "Entities")
					result[key] = value;
			}
			return result;
		}

		void Link(Scene& scene, const std::vector<Entity>& roots, AssetHandle prefab)
		{
			for (const Entity root : roots)
			{
				if (!root || root.GetScene() != &scene)
					continue;
				for (Entity entity : CollectSubtree(root))
					entity.AddOrReplaceComponent<PrefabInstanceComponent>(PrefabInstanceComponent { prefab, entity.GetUUID() });
			}
		}

		void Unpack(Scene& scene, Entity root)
		{
			if (!root || root.GetScene() != &scene)
				return;
			const AssetHandle prefab = GetPrefab(root);
			for (Entity entity : CollectSubtree(root))
			{
				const PrefabInstanceComponent* link = entity.TryGetComponent<PrefabInstanceComponent>();
				if (link && link->Prefab == prefab)
					entity.RemoveComponent<PrefabInstanceComponent>();
			}
		}

		void StripLinks(nlohmann::json& snapshot)
		{
			if (!snapshot.contains("Entities") || !snapshot["Entities"].is_array())
				return;
			for (nlohmann::json& entity : snapshot["Entities"])
			{
				if (entity.contains("Components") && entity["Components"].is_object())
					entity["Components"].erase(c_LinkComponent);
			}
		}

	}

}
