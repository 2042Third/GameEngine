#pragma once

#include "Strata/Asset/Asset.h"
#include "Strata/Scene/Entity.h"

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Scene;

	// How an instance differs from its prefab (PrefabInstances::Compare), by the prefab's entity ids
	// (PrefabInstanceComponent::PrefabEntityID) and the instance's.
	struct PrefabInstanceDifference
	{
		std::vector<UUID> Changed; // Instance entities whose components differ from their template entity's
		std::vector<UUID> Added;   // Instance entities without a template entity (added to the instance since)
		std::vector<UUID> Removed; // Template entities the instance lacks (their ids in the prefab)

		bool IsEmpty() const { return Changed.empty() && Added.empty() && Removed.empty(); }
	};

	// The link between prefab instances in a scene and their prefab's snapshot ({"Entities": [...]}, SceneSerializer's
	// format): which entities form an instance, the snapshot an instance would make, how it differs from the prefab, and
	// replacing it with the prefab's values. An instance is the subtree of an instance root: an entity linked to a prefab
	// (PrefabInstanceComponent) whose parent is not linked to the same prefab; a prefab with several top-level entities has
	// as many roots per instance, each standing for its own part of the prefab (its template root and what lies below it).
	// The root's transform and name belong to the instance (where it stands, what it is called), never to the prefab: they
	// are left out of comparisons, kept by Replace and replaced by the prefab's own in MakeTemplate.
	namespace PrefabInstances
	{

		// Whether the entity is an instance root (see above).
		bool IsInstanceRoot(Entity entity);
		// The root of the instance the entity belongs to (itself when it is one); an invalid entity when it is not linked.
		Entity FindInstanceRoot(Entity entity);
		// The prefab an instance root's instance comes from, and its template root's id (null for entities that are not linked).
		AssetHandle GetPrefab(Entity entity);
		// Every instance root of a prefab in the scene.
		std::vector<Entity> FindInstanceRoots(Scene& scene, AssetHandle prefab);

		// The part of a prefab snapshot below a template root: that entity and its descendants, in snapshot order (its Parent
		// left out). Empty "Entities" when the snapshot lacks the root.
		nlohmann::json ExtractTemplate(const nlohmann::json& snapshot, UUID templateRoot);

		// The template an instance root's subtree makes: its entities with the prefab's ids (their PrefabEntityIDs; entities
		// that are not linked get new ones, written to outNewIds by their UUIDs), references between them remapped, without
		// PrefabInstance components, and with `rootTransform` and `rootName` (the prefab's own, when it has the root) in place
		// of the root's.
		nlohmann::json MakeTemplate(const Scene& scene, Entity root, const nlohmann::json* rootTransform, const nlohmann::json* rootName,
			std::unordered_map<UUID, UUID>& outNewIds);

		// How an instance differs from the prefab's snapshot (the template below the root's template id), ignoring the root's
		// transform and name.
		PrefabInstanceDifference Compare(const Scene& scene, Entity root, const nlohmann::json& snapshot);

		// Replaces an instance with the prefab's values (the template below the root's template id): entities keep their UUIDs
		// by PrefabEntityID, entities the template lacks go, entities the instance lacks are made with new UUIDs, and the root
		// keeps its parent, place among its siblings, transform and name. Returns the UUIDs of the entities it made (for undo
		// tracking); false, changing nothing, when the snapshot lacks the root's template or the scene cannot hold the result.
		bool Replace(Scene& scene, Entity root, const nlohmann::json& snapshot, std::vector<UUID>* outCreated = nullptr, std::string* outError = nullptr);

		// The prefab's snapshot with one root's part replaced by `part` (MakeTemplate's), in its place, or appended when the
		// snapshot lacked it.
		nlohmann::json ReplaceTemplate(const nlohmann::json& snapshot, UUID templateRoot, const nlohmann::json& part);

		// Links entities captured into a prefab (with their UUIDs as its ids, as Prefab::CreateFromEntities captures them) to
		// it: each entity of their subtrees gets a PrefabInstanceComponent naming the prefab and its own UUID.
		void Link(Scene& scene, const std::vector<Entity>& roots, AssetHandle prefab);
		// Removes the links of an instance root's instance (its subtree's entities linked to its prefab).
		void Unpack(Scene& scene, Entity root);

		// A snapshot without PrefabInstance components: what a prefab captured from linked entities stores.
		void StripLinks(nlohmann::json& snapshot);

	}

}
