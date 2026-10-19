#include "stpch.h"
#include "Strata/Scene/PrefabInstances.h"

#include "Strata/Core/Hash.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Reflection/PropertyJson.h"
#include "Strata/Scene/ComponentAccess.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/UnknownComponents.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

		// The prefab id of the next entity MakeTemplate lists: its link's to the instance's prefab, or a new one when it has
		// none or an entity listed before it took it (`used`, the ids given so far).
		UUID TakeTemplateId(const PrefabInstanceComponent* link, AssetHandle prefab, std::unordered_set<UUID>& used)
		{
			UUID templateId = link && link->Prefab == prefab ? link->PrefabEntityID : UUID::Null();
			if (!templateId.IsValid() || !used.insert(templateId).second)
			{
				do
					templateId = UUID();
				while (!used.insert(templateId).second);
			}
			return templateId;
		}

		// The indices into a snapshot's entity list of the subtree under the entity `root`: the root first, then parents before
		// their children, siblings in the list's order. Documents may list children before their parents (they are resolved
		// by id), so the subtree is found through the parent links, whatever the order; a broken document's parent loops end
		// where an entity comes up again.
		std::vector<size_t> FindSubtree(const nlohmann::json& entities, UUID root)
		{
			std::vector<size_t> subtree;
			std::unordered_map<UUID, std::vector<size_t>> children;
			std::optional<size_t> rootIndex;
			for (size_t index = 0; index < entities.size(); index++)
			{
				const UUID id = GetId(entities[index]);
				if (id == root && !rootIndex)
					rootIndex = index;
				else
					children[GetParentId(entities[index])].push_back(index);
			}
			if (!rootIndex)
				return subtree;
			std::unordered_set<size_t> visited = { *rootIndex };
			subtree.push_back(*rootIndex);
			for (size_t next = 0; next < subtree.size(); next++)
			{
				const auto found = children.find(GetId(entities[subtree[next]]));
				if (found == children.end())
					continue;
				for (size_t child : found->second)
				{
					if (visited.insert(child).second)
						subtree.push_back(child);
				}
			}
			return subtree;
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

		// A hash of a part ({"Entities": [...]}, the root first) as it stands for an instance's values: without the root's place,
		// transform and name (the instance's own), its entities in id order (Compare matches them by id). Never 0 for a part
		// with entities; 0 for an empty one.
		uint64_t HashPart(nlohmann::json part)
		{
			nlohmann::json* entities = part.contains("Entities") ? &part["Entities"] : nullptr;
			if (!entities || !entities->is_array() || entities->empty())
				return 0;
			nlohmann::json& root = (*entities)[0];
			if (root.is_object())
			{
				root.erase("Parent");
				if (nlohmann::json* components = root.contains("Components") ? &root["Components"] : nullptr; components && components->is_object())
				{
					components->erase("Transform");
					components->erase("Name");
				}
			}
			std::vector<std::pair<std::string, const nlohmann::json*>> ordered;
			ordered.reserve(entities->size());
			for (const nlohmann::json& entity : *entities)
				ordered.emplace_back(ToText(GetId(entity)), &entity);
			std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
			uint64_t hash = Hash::FNVOffsetBasis;
			for (const auto& [id, entity] : ordered)
				hash = Hash::FNV1a(JsonUtils::Dump(*entity), hash);
			return hash != 0 ? hash : 1;
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

		std::unordered_map<AssetHandle, std::vector<UUID>> FindInstanceRootsByPrefab(Scene& scene)
		{
			std::unordered_map<AssetHandle, std::vector<UUID>> roots;
			auto view = scene.GetRegistry().view<PrefabInstanceComponent>();
			for (const entt::entity handle : view)
			{
				const Entity entity(handle, &scene);
				if (IsInstanceRoot(entity))
					roots[view.get<PrefabInstanceComponent>(handle).Prefab].push_back(entity.GetUUID());
			}
			return roots;
		}

		nlohmann::json ExtractTemplate(const nlohmann::json& snapshot, UUID templateRoot)
		{
			nlohmann::json part = nlohmann::json::array();
			const nlohmann::json* entities = JsonUtils::Find(snapshot, "Entities");
			if (entities && entities->is_array())
			{
				for (size_t index : FindSubtree(*entities, templateRoot))
					part.push_back((*entities)[index]);
				if (!part.empty())
					part[0].erase("Parent");
			}
			return nlohmann::json { { "Entities", std::move(part) } };
		}

		uint64_t GetPartVersion(const nlohmann::json& snapshot, UUID templateRoot)
		{
			return HashPart(ExtractTemplate(snapshot, templateRoot));
		}

		uint64_t GetContentHash(const Scene& scene, Entity root)
		{
			std::unordered_map<UUID, UUID> ids;
			return HashPart(MakeTemplate(scene, root, nullptr, nullptr, ids));
		}

		uint64_t GetTemplateHash(const nlohmann::json& part)
		{
			return HashPart(part);
		}

		void MarkSynced(Scene& scene, Entity root, const nlohmann::json& snapshot)
		{
			const PrefabInstanceComponent* link = root ? root.TryGetComponent<PrefabInstanceComponent>() : nullptr;
			if (!link)
				return;
			const uint64_t version = GetPartVersion(snapshot, link->PrefabEntityID);
			// The link is looked up again afterwards: MakeTemplate serializes the entities, which may grow the registry's pools
			// under the component.
			MarkSynced(root, version, GetContentHash(scene, root));
		}

		void MarkSynced(Entity root, uint64_t partVersion, uint64_t contentHash)
		{
			PrefabInstanceComponent* link = root ? root.TryGetComponent<PrefabInstanceComponent>() : nullptr;
			if (!link)
				return;
			link->PrefabVersion = partVersion;
			link->SyncedHash = contentHash;
		}

		bool IsOutdated(Entity root, const nlohmann::json& snapshot)
		{
			const PrefabInstanceComponent* link = root ? root.TryGetComponent<PrefabInstanceComponent>() : nullptr;
			if (!link || link->PrefabVersion == 0)
				return false;
			const uint64_t version = GetPartVersion(snapshot, link->PrefabEntityID);
			return version != 0 && version != link->PrefabVersion;
		}

		uint64_t PartVersionCache::Get(const nlohmann::json& snapshot, UUID templateRoot)
		{
			const auto [it, added] = m_Versions.try_emplace(templateRoot, 0);
			if (added)
				it->second = GetPartVersion(snapshot, templateRoot);
			return it->second;
		}

		bool IsOutdated(Entity root, const nlohmann::json& snapshot, PartVersionCache& versions)
		{
			const PrefabInstanceComponent* link = root ? root.TryGetComponent<PrefabInstanceComponent>() : nullptr;
			if (!link || link->PrefabVersion == 0)
				return false;
			const uint64_t version = versions.Get(snapshot, link->PrefabEntityID);
			return version != 0 && version != link->PrefabVersion;
		}

		bool IsUnchangedSinceSync(const Scene& scene, Entity root)
		{
			const PrefabInstanceComponent* link = root ? root.TryGetComponent<PrefabInstanceComponent>() : nullptr;
			return link && link->SyncedHash != 0 && GetContentHash(scene, root) == link->SyncedHash;
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
				const UUID templateId = TakeTemplateId(sceneEntity ? sceneEntity.TryGetComponent<PrefabInstanceComponent>() : nullptr, prefab, used);
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

		std::unordered_map<UUID, UUID> MapTemplateIds(Entity root)
		{
			std::unordered_map<UUID, UUID> ids;
			if (!root)
				return ids;
			const AssetHandle prefab = GetPrefab(root);
			std::unordered_set<UUID> used;
			// In the order SerializeEntities lists the subtree (parents first, siblings in order), so that of two entities with
			// one link the same one keeps its id.
			std::vector<Entity> stack = { root };
			while (!stack.empty())
			{
				const Entity entity = stack.back();
				stack.pop_back();
				ids.emplace(entity.GetUUID(), TakeTemplateId(entity.TryGetComponent<PrefabInstanceComponent>(), prefab, used));
				const std::vector<Entity> children = entity.GetChildren();
				stack.insert(stack.end(), children.rbegin(), children.rend());
			}
			return ids;
		}

		nlohmann::json MakeTemplateComponents(Entity entity, const std::unordered_map<UUID, UUID>& templateIds)
		{
			// As SceneSerializer writes an entity's components.
			nlohmann::json components = ComponentAccess::SerializeEntityComponents(entity);
			if (const UnknownComponentsComponent* unknown = entity.TryGetComponent<UnknownComponentsComponent>())
				unknown->AppendTo(components);
			components.erase(c_LinkComponent);
			// References are UUIDs as strings: those naming an entity of the instance (in the form UUIDToJson writes) are remapped.
			std::vector<nlohmann::json*> pending = { &components };
			while (!pending.empty())
			{
				nlohmann::json& current = *pending.back();
				pending.pop_back();
				if (current.is_string())
				{
					const std::string& text = current.get_ref<const std::string&>();
					const std::optional<UUID> id = UUID::FromString(text);
					const auto found = id ? templateIds.find(*id) : templateIds.end();
					if (found != templateIds.end() && ToText(*id) == text)
						current = ToText(found->second);
				}
				else if (current.is_object() || current.is_array())
				{
					for (nlohmann::json& child : current)
						pending.push_back(&child);
				}
			}
			return components;
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
			MarkSynced(scene, scene.GetEntityByUUID(rootId), snapshot);
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
			if (source && source->is_array())
			{
				const std::vector<size_t> subtree = FindSubtree(*source, templateRoot);
				const std::unordered_set<size_t> removed(subtree.begin(), subtree.end());
				for (size_t index = 0; index < source->size(); index++)
				{
					if (removed.contains(index))
					{
						// The new part goes where the old one began.
						if (!inserted && replacement)
						{
							for (const nlohmann::json& added : *replacement)
								entities.push_back(added);
							inserted = true;
						}
						continue;
					}
					entities.push_back((*source)[index]);
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
