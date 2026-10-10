#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Core/UUID.h"
#include "Strata/Scene/SceneHierarchy.h"
#include "Strata/Scene/SceneSystem.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Strata
{

	class Entity;

	struct SceneSettings
	{
		glm::vec3 Gravity = { 0.0f, -9.81f, 0.0f };
		float FixedTimestep = 1.0f / 60.0f;
		uint32_t MaxFixedStepsPerFrame = 8; // Prevents a spiral of death after long frames
	};

	// A world of entities. Owns the EnTT registry, the hierarchy, and (while playing) the runtime systems.
	// All Scene functions must be called on the main thread.
	//
	// Per-frame cost follows what changed, not the size of the scene: the hierarchy is mirrored as EnTT handle links
	// (HierarchyComponent), world transforms are recomputed only below entities whose transform changed, activity in the
	// hierarchy is kept exact as it changes, and lookups (hierarchy order, names, tags, primary camera) are cached or
	// indexed. The transform contract: code that writes TransformComponent fields directly must signal the change, through
	// Entity::MarkModified<TransformComponent>() or registry.patch (ComponentAccess, Scene::SetWorldTransform and the script
	// API do), or through MarkTransformChanged where an on_update signal is unwanted (physics write-back). The same holds
	// for NameComponent and TagComponent, whose values the name and tag lookups index. Entities created since the last
	// UpdateWorldTransforms are recomputed anyway. Debug builds validate the caches after every runtime and editor update.
	class Scene
	{
	public:
		explicit Scene(std::string name = "Untitled");
		~Scene();

		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;

		// Deep copy preserving entity UUIDs; used to enter play mode without touching the edited scene.
		static Ref<Scene> Copy(const Ref<Scene>& source);

		//////////////////////////////////////////////////////////////////////////
		// Entities
		//////////////////////////////////////////////////////////////////////////

		// EnTT identifiers have a 20-bit index, so a registry holds at most this many live entities.
		static constexpr size_t c_MaxEntities = static_cast<size_t>(entt::entt_traits<entt::entity>::entity_mask);

		// When the registry holds c_MaxEntities live entities, creating one more logs an error and returns an invalid Entity
		// (deserialization reports an error instead).
		Entity CreateEntity(const std::string& name = std::string());
		Entity CreateEntityWithUUID(UUID uuid, const std::string& name = std::string());
		Entity CreateChildEntity(Entity parent, const std::string& name = std::string());

		// Destroys the entity and all its descendants. While the scene is updating (scripts, physics callbacks)
		// destruction is deferred to the end of the frame, so handles stay valid for the rest of the frame.
		void DestroyEntity(Entity entity);
		// Destroys several entities (each with its descendants) like DestroyEntity, in one pass: running systems are told
		// about every subtree first, every sibling list is compacted once and the hierarchy version changes once, so removing
		// many entities from a long sibling list stays linear. Invalid entities and entities of other scenes are ignored.
		void DestroyEntities(std::span<const Entity> entities);
		bool IsPendingDestroy(Entity entity) const;
		// Entities whose destruction was deferred to the end of the current update (each with its descendants).
		const std::vector<UUID>& GetPendingDestroys() const { return m_PendingDestroy; }

		// Deep copy of an entity and its descendants with fresh UUIDs, inserted after the original.
		// References between entities inside the copied hierarchy are remapped to the copies.
		Entity DuplicateEntity(Entity entity);

		Entity GetEntityByUUID(UUID uuid) const; // Invalid Entity when not found
		// The first entity in hierarchy order with this name, and every entity with this tag in hierarchy order. Both use an
		// index built on the first lookup and kept current through the components' signals afterwards, so a lookup costs the
		// number of entities with that name or tag, not the size of the scene (ordering them may renumber sibling lists that
		// changed since, see CompareHierarchyOrder).
		Entity FindEntityByName(std::string_view name) const;
		std::vector<Entity> FindEntitiesByTag(std::string_view tag) const;
		size_t GetEntityCount() const { return m_EntityMap.size(); }

		// Root entities in hierarchy order.
		const std::vector<UUID>& GetRootEntities() const { return m_RootEntities; }
		// Changes whenever the hierarchy order may have changed (entities created or destroyed, reparented or reordered among
		// their siblings), so that results depending on it can be cached.
		uint64_t GetHierarchyVersion() const { return m_HierarchyVersion; }
		// Appends the entities that moved in the hierarchy after hierarchy version `sinceVersion`: reparented (SetParent,
		// PlaceEntities, CreateChildEntity) or moved among their siblings (SetSiblingIndex, PlaceEntities). Creating and
		// destroying entities never changes the order of the other entities relative to each other, so a cache of the
		// hierarchy order of some entities stays valid for every entity outside the subtrees of these. An entity may appear
		// more than once; some may have been destroyed since. Returns false, appending nothing, if the scene no longer
		// remembers that far back (it keeps the latest c_MaxHierarchyMoves moves): then any entity may have moved.
		bool GetHierarchyMoves(uint64_t sinceVersion, std::vector<entt::entity>& outEntities) const;
		static constexpr size_t c_MaxHierarchyMoves = 4096;
		// Every entity in depth-first hierarchy order (parents before children). Computed once per hierarchy version.
		std::vector<Entity> GetEntitiesInHierarchyOrder() const;

		//////////////////////////////////////////////////////////////////////////
		// Hierarchy and transforms
		//////////////////////////////////////////////////////////////////////////

		// Re-parents child under parent (an invalid parent makes it a root). Returns false if this would create a
		// cycle. With keepWorldTransform the child's world transform is preserved (through SetWorldTransform, which signals
		// the new local transform). A change of parent emits the child's RelationshipComponent on_update signal (the old and
		// new parents' Children lists change silently).
		bool SetParent(Entity child, Entity parent, bool keepWorldTransform = true);
		// Moves an entity to position `index` among its siblings (or among the roots). Only the order changes, so no
		// signal is emitted.
		bool SetSiblingIndex(Entity entity, size_t index);
		// Position of the entity among its siblings (or among the roots); 0 for invalid entities. Constant while the sibling
		// list is unchanged and after appending to it; the first query after any other change of the list (an insertion,
		// a removal that is not the last entry, a reorder) renumbers the whole list, linear in its length. So queries that
		// alternate with such changes of one list (deleting entities one at a time) each cost the list's length.
		size_t GetSiblingIndex(Entity entity) const;

		// Where PlaceEntities puts an entity: under Parent (the null UUID: among the roots) at position SiblingIndex.
		struct EntityPlacement
		{
			UUID Target = UUID::Null();
			UUID Parent = UUID::Null();
			size_t SiblingIndex = 0;
		};
		// Moves many entities at once, as if each were detached first and then attached under its parent in ascending order of
		// SiblingIndex (ties keep the given order), each landing at its position (clamped to the list's end). World
		// transforms are not preserved (local transforms stay). A placement whose parent does not exist or would create a
		// cycle leaves its entity at the top level, and one whose target does not exist (or is listed twice) is skipped; both
		// make the call return false. Each sibling list involved is rebuilt once, so restoring many positions in a long list
		// stays linear. Entities whose parent changed emit their RelationshipComponent on_update signal, like SetParent.
		bool PlaceEntities(std::span<const EntityPlacement> placements);
		bool IsDescendantOf(Entity entity, Entity ancestor) const;
		// Negative if a comes before b in hierarchy order, positive if after, 0 if they are the same entity. Costs the depth of
		// the two entities while the sibling list where their ancestors meet has current positions; after a change of that
		// list it renumbers it once, like GetSiblingIndex. Both must be valid entities of this scene.
		int CompareHierarchyOrder(Entity a, Entity b) const;
		// Depth in the hierarchy (roots: 0); 0 for invalid entities.
		uint32_t GetDepth(Entity entity) const;

		// Brings the cached WorldTransformComponent matrices up to date (runs every frame). Only subtrees below changed
		// entities are recomputed, each from its parent's cached matrix; without changes this returns at once. Large updates
		// run on the job system.
		void UpdateWorldTransforms();
		// The entity's world transform right now: the cached one when neither it nor an ancestor changed since the last
		// update, otherwise computed from the hierarchy (costs the entity's depth).
		glm::mat4 GetWorldTransform(Entity entity) const;
		// Sets the entity's local transform so that its world transform becomes `worldTransform`, and emits the
		// TransformComponent on_update signal like Entity::MarkModified. Returns false (leaving the entity unchanged, without
		// a signal) if the transform cannot be represented, e.g. under a parent with zero scale.
		bool SetWorldTransform(Entity entity, const glm::mat4& worldTransform);
		// Records that the entity's TransformComponent was written, without emitting on_update (for writers that must not
		// notify listeners, e.g. physics writing simulated poses back).
		void MarkTransformChanged(Entity entity);
		// Marks every world transform stale: the next UpdateWorldTransforms recomputes the whole scene.
		void InvalidateAllTransforms();
		// A parent whose world transform's determinant is not above this (in magnitude) cannot be inverted, so the world
		// transforms of its children cannot be set.
		static constexpr float c_MinInvertibleDeterminant = 1.0e-12f;
		// Constant time: WorldTransformComponent::ActiveInHierarchy is updated for the whole affected subtree whenever
		// InactiveComponent is added or removed or an entity is reparented, so it is never stale.
		bool IsActiveInHierarchy(Entity entity) const;

		// Changes whenever a cached WorldTransformComponent changes (matrices in UpdateWorldTransforms, activity at once).
		uint64_t GetTransformsVersion() const { return m_TransformsVersion; }
		// Appends the entities whose WorldTransformComponent changed after transforms version `sinceVersion` (an entity may
		// appear more than once; some may have been destroyed since), so that caches of world transforms update only what
		// changed. Returns false, appending nothing, if the scene no longer remembers that far back (it keeps the latest
		// c_MaxTransformChanges changes): then any entity may have changed.
		bool GetWorldTransformChanges(uint64_t sinceVersion, std::vector<entt::entity>& outEntities) const;
		static constexpr size_t c_MaxTransformChanges = 16384;

		// Compares every cached world transform that is not known to be stale (and every ActiveInHierarchy flag) with a full
		// recomputation from the hierarchy (within a relative 1e-5). Returns false with a description of the first mismatch.
		bool ValidateWorldTransforms(std::string* outError = nullptr) const;
		// Checks that the hierarchy links, depths, child counts, cached sibling positions, root list and the name and tag
		// indices agree with the RelationshipComponents, the UUID map and the components. Returns false with the first
		// inconsistency.
		bool ValidateHierarchy(std::string* outError = nullptr) const;

		// Diagnostics (cumulative since the scene was created): world transforms recomputed by UpdateWorldTransforms (and the
		// number of its calls that recomputed some of them on the job system), full hierarchy order computations, entities
		// examined by FindEntityByName, FindEntitiesByTag and GetPrimaryCameraEntity (excluding index builds), and name or tag
		// index builds.
		uint64_t GetTransformUpdateCount() const { return m_TransformUpdateCount; }
		uint64_t GetParallelTransformUpdateCount() const { return m_ParallelTransformUpdateCount; }
		uint64_t GetHierarchyOrderBuildCount() const { return m_HierarchyOrderBuildCount; }
		uint64_t GetLookupVisitCount() const { return m_LookupVisitCount; }
		uint64_t GetLookupIndexBuildCount() const { return m_LookupIndexBuildCount; }

		//////////////////////////////////////////////////////////////////////////
		// Simulation
		//////////////////////////////////////////////////////////////////////////

		void OnRuntimeStart(SceneRuntimeMode mode = SceneRuntimeMode::Play);
		void OnRuntimeStop();
		void OnUpdateRuntime(Timestep timestep);
		void OnUpdateEditor(Timestep timestep);

		bool IsRunning() const { return m_IsRunning; }
		SceneRuntimeMode GetRuntimeMode() const { return m_RuntimeMode; }
		bool IsUpdating() const { return m_IsUpdating; }
		// Pausing stops the updates of the running systems and tells them (SceneSystem::OnPausedChanged). Starting the scene
		// resets it to unpaused.
		void SetPaused(bool paused);
		bool IsPaused() const { return m_IsPaused; }
		// While paused, lets the next `frames` updates each advance the simulation by exactly one fixed step.
		void Step(uint32_t frames = 1) { m_StepFrames += frames; }
		// Steps requested with Step that have not run yet.
		uint32_t GetStepFrames() const { return m_StepFrames; }

		// Simulation time since OnRuntimeStart (seconds) and number of runtime updates.
		double GetTime() const { return m_Time; }
		uint64_t GetFrameIndex() const { return m_FrameIndex; }
		float GetTimeScale() const { return m_TimeScale; }
		void SetTimeScale(float timeScale) { m_TimeScale = std::max(0.0f, timeScale); }

		template<typename T>
		T* GetSystem() const
		{
			for (const Scope<SceneSystem>& system : m_Systems)
			{
				if (T* typed = dynamic_cast<T*>(system.get()))
					return typed;
			}
			return nullptr;
		}

		//////////////////////////////////////////////////////////////////////////
		// Requests to the scene's owner
		//////////////////////////////////////////////////////////////////////////

		// Gameplay code (scripts) asks whoever runs the scene to end the game or to switch scenes; the owner (GameRuntime,
		// the editor's play mode) honors the requests after the update in which they were made. Quitting wins over a scene
		// load. Later requests replace earlier ones; starting the scene clears both.
		void RequestQuit(int32_t exitCode) { m_QuitRequest = exitCode; }
		std::optional<int32_t> GetQuitRequest() const { return m_QuitRequest; }
		// `sceneAsset` is the handle of a scene asset; the null handle asks to restart the running scene.
		void RequestSceneLoad(UUID sceneAsset) { m_SceneLoadRequest = sceneAsset; }
		std::optional<UUID> GetSceneLoadRequest() const { return m_SceneLoadRequest; }
		// The pending scene load, which the call clears (so that an owner that cannot honor it does not retry every frame).
		std::optional<UUID> TakeSceneLoadRequest();

		// The first active entity in hierarchy order whose CameraComponent is Primary, or an invalid Entity. Examines only the
		// entities with a CameraComponent (and reads the current component values, so it is never stale).
		Entity GetPrimaryCameraEntity();

		const std::string& GetName() const { return m_Name; }
		void SetName(const std::string& name) { m_Name = name; }
		SceneSettings& GetSettings() { return m_Settings; }
		const SceneSettings& GetSettings() const { return m_Settings; }

		entt::registry& GetRegistry() { return m_Registry; }
		const entt::registry& GetRegistry() const { return m_Registry; }

		template<typename... Components>
		auto GetAllEntitiesWith()
		{
			return m_Registry.view<Components...>();
		}
	private:
		// Where an entity is listed in a lookup index (by its EnTT index, which outlives the destruction order of components).
		struct LookupSlot
		{
			uint64_t Key = 0;      // Hash of the indexed value
			uint32_t Position = 0; // In the bucket
			bool Indexed = false;
		};
		// Entities of a lookup index (FindEntityByName, FindEntitiesByTag) by the hash of their name or tag. Lookups compare
		// the actual strings, so different strings sharing a hash only share a bucket. Removal swaps with the bucket's last
		// entry, so it costs the same for a name a million entities share.
		struct LookupIndex
		{
			bool Built = false;
			std::unordered_map<uint64_t, std::vector<entt::entity>> Buckets;
			std::vector<LookupSlot> Slots;
		};
		// An entry of the bounded change logs (world transforms, hierarchy moves).
		struct EntityChange
		{
			uint64_t Version = 0; // Transforms or hierarchy version the change produced
			entt::entity Entity = entt::null;
		};

		// Live entities of the registry: the scene's, and any created directly through GetRegistry().
		size_t GetRegistryEntityCount() const;
		// Drops the entities that lie below another listed one (they go with it) and repeated ones, keeping the order.
		void RemoveNestedRoots(std::vector<entt::entity>& roots) const;
		// Destroys the subtrees (already announced to the systems) and compacts the sibling lists they leave.
		void DestroySubtrees(std::span<const entt::entity> roots);
		// Lets running systems react to the subtree's destruction (SceneSystem::OnEntityDestroying).
		void NotifyEntitiesDestroying(entt::entity root);
		// The entity and its descendants in depth-first hierarchy order (parents before children).
		std::vector<entt::entity> CollectSubtree(entt::entity root) const;
		// Destroys the subtrees of `roots` (nested and repeated ones are dropped): running systems are told about all of them
		// first, then each sibling list they leave is compacted once.
		void DestroyRoots(std::vector<entt::entity> roots);
		void FlushPendingDestroys();
		// Every entity in hierarchy order (walks the links).
		void CollectHierarchyOrder(std::vector<entt::entity>& outEntities) const;

		// Hierarchy links. These keep the links, child counts, sibling-position validity and the UUID lists
		// (RelationshipComponent::Children, m_RootEntities) in step; Depth, activity and transforms are refreshed separately.
		HierarchyComponent& GetHierarchy(entt::entity handle) { return m_Registry.get<HierarchyComponent>(handle); }
		const HierarchyComponent& GetHierarchy(entt::entity handle) const { return m_Registry.get<HierarchyComponent>(handle); }
		void LinkLast(entt::entity child, entt::entity parent);
		// Inserts the child at `position` among the parent's children (clamped to the end).
		void LinkAt(entt::entity child, entt::entity parent, size_t position);
		// Detaches the entity from its parent or the root list. With removeId false the parent's UUID list keeps the entity's
		// id (a batch compacts it afterwards).
		void Unlink(entt::entity child, bool removeId = true);
		// Rewrites the child list of `parent` (entt::null: the roots) to exactly `children`, all of which are already detached
		// or children of `parent`.
		void RebuildChildList(entt::entity parent, std::span<const entt::entity> children);
		std::vector<UUID>& GetChildIds(entt::entity parent);
		entt::entity GetFirstChild(entt::entity parent) const;
		// Recomputes the sibling positions of the parent's children (entt::null: the roots) unless they are current.
		void RefreshSiblingIndices(entt::entity parent) const;
		// Depth and ActiveInHierarchy of the subtree from its parent's; with `prune`, subtrees whose value did not change are
		// skipped (only valid when they were consistent before).
		void RefreshSubtreeDepth(entt::entity root, bool prune);
		void RefreshSubtreeActivity(entt::entity root, bool prune, bool rootLosesInactive = false);
		// Depth, activity and stale transforms after linking a batch of new subtrees (deserialization).
		void FinishLinking(std::span<const entt::entity> roots);
		// Logs entities that moved in the hierarchy at the current hierarchy version (see GetHierarchyMoves).
		void RecordHierarchyMoves(std::span<const entt::entity> entities);

		// World transforms
		void MarkTransformDirty(entt::entity handle);
		bool HasDirtyAncestor(entt::entity parent);
		void RecordTransformChanges(uint64_t version, std::span<const entt::entity> entities, bool overflowed);
		// Debug builds: asserts that ValidateWorldTransforms and ValidateHierarchy pass.
		void AssertCachesValid() const;

		// Lookup indices (caches, hence const)
		void BuildLookupIndex(LookupIndex& index, bool names) const;
		void IndexLookupValue(LookupIndex& index, entt::entity handle, std::string_view value) const;
		void RemoveLookupValue(LookupIndex& index, entt::entity handle) const;

		// Signal handlers
		void OnTransformChanged(entt::registry& registry, entt::entity handle);
		void OnInactiveAdded(entt::registry& registry, entt::entity handle);
		void OnInactiveRemoved(entt::registry& registry, entt::entity handle);
		void OnNameChanged(entt::registry& registry, entt::entity handle);
		void OnNameRemoved(entt::registry& registry, entt::entity handle);
		void OnTagChanged(entt::registry& registry, entt::entity handle);
		void OnTagRemoved(entt::registry& registry, entt::entity handle);
	private:
		std::string m_Name;
		entt::registry m_Registry;
		std::unordered_map<UUID, entt::entity> m_EntityMap;
		std::vector<UUID> m_RootEntities;
		uint64_t m_HierarchyVersion = 0;
		std::deque<EntityChange> m_HierarchyMoves; // The latest moves, oldest first
		uint64_t m_HierarchyMovesFloor = 0;        // Moves up to this version may be missing from m_HierarchyMoves
		SceneSettings m_Settings;

		// Hierarchy links of the root list (the roots' HierarchyComponents link the rest).
		entt::entity m_FirstRoot = entt::null;
		entt::entity m_LastRoot = entt::null;
		mutable bool m_RootIndicesValid = true;
		// Set while subtrees are torn down: the signals their components emit must not walk links into destroyed entities.
		bool m_DestroyingEntities = false;

		// World transforms: entities whose HierarchyComponent::TransformDirty is set, in the order they were marked.
		std::vector<entt::entity> m_DirtyTransforms;
		uint32_t m_TransformPass = 0;
		uint64_t m_TransformsVersion = 0;
		uint64_t m_TransformUpdateCount = 0;
		uint64_t m_ParallelTransformUpdateCount = 0;
		std::deque<EntityChange> m_TransformChanges; // The latest changes, oldest first
		uint64_t m_TransformChangesFloor = 0;        // Changes up to this version may be missing from m_TransformChanges
		std::vector<entt::entity> m_ChangeScratch;
		mutable std::vector<entt::entity> m_PathScratch;

		// Queries (caches, hence mutable)
		mutable std::vector<Entity> m_HierarchyOrder;
		mutable uint64_t m_HierarchyOrderVersion = 0;
		mutable bool m_HierarchyOrderValid = false;
		mutable LookupIndex m_NameIndex;
		mutable LookupIndex m_TagIndex;
		mutable uint64_t m_HierarchyOrderBuildCount = 0;
		mutable uint64_t m_LookupVisitCount = 0;
		mutable uint64_t m_LookupIndexBuildCount = 0;

		std::vector<Scope<SceneSystem>> m_Systems;
		std::vector<UUID> m_PendingDestroy;
		std::unordered_set<UUID> m_PendingDestroySet;
		SceneRuntimeMode m_RuntimeMode = SceneRuntimeMode::Play;
		bool m_IsRunning = false;
		bool m_IsUpdating = false;
		bool m_IsPaused = false;
		uint32_t m_StepFrames = 0;
		float m_FixedTimeAccumulator = 0.0f;
		float m_TimeScale = 1.0f;
		double m_Time = 0.0;
		uint64_t m_FrameIndex = 0;
		std::optional<int32_t> m_QuitRequest;
		std::optional<UUID> m_SceneLoadRequest;

		// Declared last: disconnected before anything the handlers use is destroyed.
		std::vector<entt::scoped_connection> m_Connections;

		friend class Entity;
		friend class SceneSerializer;
	};

}
