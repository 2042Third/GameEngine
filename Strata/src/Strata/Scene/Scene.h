#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/FrameProfiler.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Core/UUID.h"
#include "Strata/Scene/PhysicsLayerSettings.h"
#include "Strata/Scene/SceneHierarchy.h"
#include "Strata/Scene/SceneSystem.h"
#include "Strata/Scene/SubScene.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
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

	class AssetManagerBase;
	class Entity;
	class EntityPrototype;
	class PrototypeInstantiation;
	struct InstantiationOptions;
	struct SubSceneLoad;
	struct TransformComponent;

	// Capacities of a scene's physics world, fixed when its simulation is created. 0 sizes a capacity automatically from the
	// bodies the scene has when it starts playing (see PhysicsWorld): set them for scenes that create many bodies while they
	// play (spawners, streamed cells), which the automatic sizes cannot foresee.
	struct PhysicsCapacitySettings
	{
		uint32_t MaxBodies = 0;
		uint32_t MaxBodyPairs = 0;          // Pairs of bodies whose bounds overlap, per step
		uint32_t MaxContactConstraints = 0; // Contacts solved per step

		// The largest values a scene may ask for, whether its file or a command sets them. The physics engine reserves memory
		// for every capacity when the world is created, and does not survive an allocation that fails, so the limits bound
		// what one world reserves: about 80 bytes per body, 90 per body pair and 830 per contact constraint, so about 940 MiB
		// at the limits. A scene holds at most Scene::c_MaxEntities (2^20 - 1) entities, so 2^21 bodies have room for all of
		// them, with the automatic sizing's headroom.
		static constexpr uint32_t c_MaxBodiesLimit = 1u << 21;
		static constexpr uint32_t c_MaxBodyPairsLimit = 1u << 22;
		static constexpr uint32_t c_MaxContactConstraintsLimit = 1u << 19;

		bool operator==(const PhysicsCapacitySettings& other) const = default;
	};

	struct SceneSettings
	{
		glm::vec3 Gravity = { 0.0f, -9.81f, 0.0f };
		float FixedTimestep = 1.0f / 60.0f;
		uint32_t MaxFixedStepsPerFrame = 8; // Prevents a spiral of death after long frames
		PhysicsCapacitySettings PhysicsCapacity;
		// Entities additive loads (sub-scenes) may create per update of the scene, all of them together; each update also
		// stops creating once Scene::c_MaxInstantiationMillisecondsPerFrame went by. From 1 to c_MaxEntitiesInstantiatedLimit.
		uint32_t MaxEntitiesInstantiatedPerFrame = 5000;
		static constexpr uint32_t c_MaxEntitiesInstantiatedLimit = 1u << 20;
		PhysicsLayerSettings PhysicsLayers; // Layer names and which layers collide
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

		// Deep copy preserving entity UUIDs; used to enter play mode without touching the edited scene. The copy's entities have
		// the source's EnTT identifiers too, so the hierarchy, the UUID map and the cached world transforms are copied as they
		// are, and every component storage is copied whole (ComponentInfo::CopyStorage), the storages in parallel on the job
		// system. Copies keep the source asset and the lineage (GetSourceAsset, GetLineageID).
		static Ref<Scene> Copy(const Ref<Scene>& source);
		// Lets go of a scene that is no longer used (one a scene switch or an opened scene replaced) without spending the
		// frame on destroying it, which takes tens of milliseconds for a million entities: what must end on the main thread
		// ends here (a running scene stops: its systems, scripts, bodies and voices; sub-scene loads and their asset requests
		// are dropped), and the rest, its entities and caches, which nothing else reaches, is destroyed by a background job
		// (JobPriority::Background) unless something else still holds the scene. Main thread.
		static void ReleaseInBackground(Ref<Scene> scene);

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

		//////////////////////////////////////////////////////////////////////////
		// Instantiation of compiled prototypes (Scene/SceneInstantiation.cpp)
		//////////////////////////////////////////////////////////////////////////

		// Starts copying a prototype (EntityPrototype: prefabs, models, additive scene loads) into the scene: checks what can
		// be checked before anything exists (the parent; with InstantiationOptions::KeepIDs every prototype ID against the
		// scene's) and gives every entity its ID. Fails, creating nothing, with an error. ContinueInstantiation then creates
		// the entities; `outInstantiation` must outlive the calls (and its prototype, which it shares).
		bool BeginInstantiation(const Ref<EntityPrototype>& prototype, const InstantiationOptions& options, PrototypeInstantiation& outInstantiation,
			std::string* outError = nullptr);
		// Creates the next entities of the instantiation, at most `maxEntities`, in the prototype's hierarchy order, as
		// SceneSerializer::DeserializeEntities would: their components are copied from the prototype (on_construct), the
		// entities are linked under their parents (created before, in this call or an earlier one) or under the options'
		// parent, references among the prototype's entities point at the copies, prefab links are added, and then every
		// component of every created entity emits on_update, so systems see each entity complete. With `rootTransform`, the
		// prototype's first top-level entity gets that local transform instead of its own. An entity whose parent, created
		// by an earlier call, was destroyed since is not created, nor are its descendants (they would have gone with it;
		// PrototypeInstantiation::GetSkippedCount). Returns how many of the prototype's entities it handled (created or
		// skipped): 0, creating nothing, with an error when the scene cannot hold them (c_MaxEntities), an ID it keeps
		// collides or the options' parent went away (and 0 without an error once every entity was handled).
		size_t ContinueInstantiation(PrototypeInstantiation& instantiation, size_t maxEntities, std::string* outError = nullptr,
			const TransformComponent* rootTransform = nullptr);
		// Both in one call: the created top-level entities in order, or none, creating nothing, with the error.
		std::vector<Entity> Instantiate(const Ref<EntityPrototype>& prototype, const InstantiationOptions& options, std::string* outError = nullptr);

		//////////////////////////////////////////////////////////////////////////
		// Additive scenes (Scene/SceneSubScenes.cpp)
		//////////////////////////////////////////////////////////////////////////

		// How long the updates of a scene may spend creating the entities of sub-scenes at most (besides
		// SceneSettings::MaxEntitiesInstantiatedPerFrame): they create them in chunks and stop once this much time went by.
		static constexpr double c_MaxInstantiationMillisecondsPerFrame = 2.0;

		// Loads the scene asset `scene` into this scene, next to what it holds (a sub-scene: a level's streamed part, a room):
		// the asset and everything it uses load in the background (SceneLoader, pinned at the options' priority), its
		// prepared scene is compiled on the job system (EntityPrototype), and its entities are then created over the following
		// updates (UpdateSubScenes), at most SceneSettings::MaxEntitiesInstantiatedPerFrame per update and for about
		// c_MaxInstantiationMillisecondsPerFrame. They keep the IDs they have in the asset, so a sub-scene whose IDs the scene
		// has already (the same sub-scene twice) fails, adding nothing; its settings (gravity...) are not applied. Each of its
		// top-level entities gets a SubSceneComponent. Created entities are like any other: systems hear their components'
		// signals (scripts get OnCreate, physics adds bodies, audio starts voices, renderers add proxies). Returns the
		// sub-scene's ID, or 0 with an error when no load can start (no active asset manager, not a scene asset).
		SubSceneID LoadSubScene(AssetHandle scene, const SubSceneOptions& options = {}, std::string* outError = nullptr);
		// Adds the entities of a prepared scene the same way, without loading anything (the scene is compiled on the job
		// system: nothing may use or change it afterwards; its systems must not run). 0 with an error for a null scene.
		SubSceneID BeginAdditiveLoad(Ref<Scene> prepared, std::string* outError = nullptr);
		// Removes a sub-scene: cancels its load (releasing every pin it holds) and destroys the entities it created with their
		// descendants (DestroyEntities: running systems hear OnEntityDestroying first; deferred while the scene updates).
		// Its ID is unknown afterwards. False for an unknown ID. While UpdateSubScenes runs (scripts' OnDestroy when a failed
		// sub-scene's entities go), the unload happens once it returns.
		bool UnloadSubScene(SubSceneID id);
		// Whether every entity of the sub-scene exists.
		bool IsSubSceneLoaded(SubSceneID id) const;
		// The sub-scene's state, or null for an unknown ID (never loaded, or unloaded). Failed sub-scenes stay until they are
		// unloaded, so that their error can be read.
		const SubSceneStatus* GetSubSceneStatus(SubSceneID id) const;
		// Every sub-scene that was not unloaded, in the order of their loads.
		std::vector<SubSceneStatus> GetSubScenes() const;
		// The top-level entities the sub-scene created that still exist (with its SubSceneComponent), in order.
		std::vector<Entity> GetSubSceneRoots(SubSceneID id) const;
		// Whether a sub-scene still loads or creates entities.
		bool HasPendingSubScenes() const;
		// Advances the sub-scenes: their loads (after the asset manager's update), compilations and a slice of entity creation
		// within the budgets above. OnUpdateRuntime and OnUpdateEditor call it first (also while the scene is paused or not
		// running); owners of scenes they do not update call it once per frame themselves.
		void UpdateSubScenes();

		Entity GetEntityByUUID(UUID uuid) const; // Invalid Entity when not found
		// The first entity in hierarchy order with this name, and every entity with this tag in hierarchy order. Both use an
		// index built on the first lookup and kept current through the components' signals afterwards, so a lookup costs the
		// number of entities with that name or tag, not the size of the scene (ordering them may renumber sibling lists that
		// changed since, see CompareHierarchyOrder).
		Entity FindEntityByName(std::string_view name) const;
		std::vector<Entity> FindEntitiesByTag(std::string_view tag) const;
		// Whether a child of `parent` (a top-level entity for an invalid parent) other than `except` (none when invalid) is
		// named `name`. Looks at the parent's children or at the entities with that name (the name index), whichever are
		// fewer, so editors making names unique among siblings stay fast with many children or many entities of a name.
		bool HasChildNamed(Entity parent, std::string_view name, Entity except) const;
		size_t GetEntityCount() const { return m_EntityMap.size(); }
		// Live entities of the registry: the scene's, and any created directly through GetRegistry(). At most c_MaxEntities.
		size_t GetRegistryEntityCount() const;
		// About the system memory the scene's entities occupy: every component storage (its elements, packed entities and
		// sparse pages), the UUID map, and the heap memory of names and child lists. Costs a pass over those two components;
		// for asset accounting (a prepared scene, a prefab's prototype), not for every frame.
		uint64_t EstimateMemoryUsage() const;

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
		// The entity's cached world transform if it is current (neither the entity nor an ancestor changed since the last
		// update), else null; costs the entity's depth. Unlike GetWorldTransform it only reads the scene, so worker threads
		// may call it for many entities while no thread changes the scene (physics comparing its bodies with their entities).
		const glm::mat4* FindCurrentWorldTransform(entt::entity handle) const;
		// Sets the entity's local transform so that its world transform becomes `worldTransform`, and emits the
		// TransformComponent on_update signal like Entity::MarkModified. Returns false (leaving the entity unchanged, without
		// a signal) if the transform cannot be represented, e.g. under a parent with zero scale.
		bool SetWorldTransform(Entity entity, const glm::mat4& worldTransform);
		// Records that the entity's TransformComponent was written, without emitting on_update (for writers that must not
		// notify listeners, e.g. physics writing simulated poses back).
		void MarkTransformChanged(Entity entity);
		// MarkTransformChanged for many entities of this scene at once (invalid handles are skipped).
		void MarkTransformsChanged(std::span<const entt::entity> entities);
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
		// examined by FindEntityByName, FindEntitiesByTag, HasChildNamed and GetPrimaryCameraEntity (excluding index builds), and name or tag
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
		// resets it to unpaused. Resuming drops the steps that have not run (Step): it hands the scene to real time, and a
		// count asked for while paused must not leak into a later pause.
		void SetPaused(bool paused);
		bool IsPaused() const { return m_IsPaused; }
		// While paused, lets the next `frames` updates each advance the simulation by exactly one fixed step.
		void Step(uint32_t frames = 1) { m_StepFrames += frames; }
		// Steps requested with Step that have not run yet (dropped by resuming and by OnRuntimeStop).
		uint32_t GetStepFrames() const { return m_StepFrames; }
		// Fixed steps run since OnRuntimeStart, stepped (Step) or due on the clock: the game's own time base, which tools
		// that must be exact (play.step, simulated input) count in instead of frames.
		uint64_t GetFixedStepCount() const { return m_FixedSteps; }

		// Simulation time since OnRuntimeStart (seconds) and number of runtime updates.
		double GetTime() const { return m_Time; }
		uint64_t GetFrameIndex() const { return m_FrameIndex; }
		float GetTimeScale() const { return m_TimeScale; }
		// Time that the time scale does not slow or stop (menus and fades at time scale 0): the latest runtime update's timestep
		// before the time scale (the fixed timestep of a step while paused), and their sum since OnRuntimeStart, in seconds.
		float GetUnscaledDeltaTime() const { return m_UnscaledDeltaTime; }
		double GetUnscaledTime() const { return m_UnscaledTime; }
		void SetTimeScale(float timeScale) { m_TimeScale = std::max(0.0f, timeScale); }

		// How an update that is behind runs its fixed steps. Without time dilation (the default, so that tests and tools step
		// deterministically) it runs every step that is due, up to MaxFixedStepsPerFrame, and drops the rest of the backlog.
		// With it (what owners driven by the wall clock use: the editor's play mode, exported games), an update stops
		// running catch-up steps once the steps it ran took longer, in wall time, than the time they simulated: a simulation
		// that cannot keep up with real time slows down instead of spending ever longer frames catching up (a frame running
		// eight expensive steps makes the next one owe even more). Its first due step always runs.
		void SetTimeDilation(bool enabled) { m_TimeDilation = enabled; }
		bool IsTimeDilated() const { return m_TimeDilation; }
		// Fixed steps whose time was dropped since the scene started running: skipped by time dilation, or beyond
		// MaxFixedStepsPerFrame.
		uint64_t GetDroppedFixedStepCount() const { return m_DroppedFixedSteps; }

		// The running system of class T (registered with MakeSceneSystemDescriptor<T>), or null: not running, not created
		// in this mode, or not registered. Looks the exact class up, not its base classes.
		template<typename T>
		T* GetSystem() const
		{
			auto it = m_SystemsByType.find(entt::type_id<T>().hash());
			return it != m_SystemsByType.end() ? static_cast<T*>(it->second) : nullptr;
		}

		// Scenes between OnRuntimeStart and OnRuntimeStop in this process (the scene system registry refuses changes then).
		static uint32_t GetRunningSceneCount();

		//////////////////////////////////////////////////////////////////////////
		// Services
		//////////////////////////////////////////////////////////////////////////

		// Scene services (Scene/SceneServices.h): interfaces of the running systems that other systems use, so that they never
		// include each other's modules. A system registers each interface T it offers while it exists (in its constructor,
		// removing it in its destructor); a scene has at most one provider per interface. Registering a second one logs an
		// error and returns false; unregistering an object that does not provide T changes nothing.
		template<typename T>
		bool RegisterService(T& service)
		{
			return RegisterServiceProvider(entt::type_id<T>().hash(), entt::type_id<T>().name(), &service);
		}
		template<typename T>
		void UnregisterService(T& service)
		{
			UnregisterServiceProvider(entt::type_id<T>().hash(), &service);
		}
		// The provider of interface T, or null when no system of the scene offers it.
		template<typename T>
		T* GetService() const
		{
			auto it = m_Services.find(entt::type_id<T>().hash());
			return it != m_Services.end() ? static_cast<T*>(it->second) : nullptr;
		}

		//////////////////////////////////////////////////////////////////////////
		// Assets
		//////////////////////////////////////////////////////////////////////////

		// The asset manager the scene's assets come from, or null when there is none: for now the process's active manager
		// (AssetManager::GetActive), the same for every scene. New code that works for a scene (scene systems, code that loads
		// assets into it, the renderer that draws it) reaches the manager through the scene, with its typed accessors
		// (AssetManagerBase::GetAsset<T>, PeekLoadedAsset<T>), and uses none of AssetManager's static functions (GetActive,
		// GetAsset<T>, PeekLoadedAsset<T>, LoadAssetSync<T>, GetAssetState, GetAssetType, IsHandleValid, HasActive), which
		// all use the active manager: per-scene asset managers (wave 5's scene contexts) then change this function rather
		// than that code. Hold asset handles across frames, not the pointer.
		AssetManagerBase* GetAssetManager() const;

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
		// How far the owner got loading the scene this one asked for (owners load it while this scene goes on, and update the
		// progress every frame): in [0, 1], or nullopt while no such load is under way. Gameplay shows it (a loading bar).
		void SetSceneLoadProgress(std::optional<float> progress) { m_SceneLoadProgress = progress; }
		std::optional<float> GetSceneLoadProgress() const { return m_SceneLoadProgress; }

		// The first active entity in hierarchy order whose CameraComponent is Primary, or an invalid Entity. Examines only the
		// entities with a CameraComponent (and reads the current component values, so it is never stale).
		Entity GetPrimaryCameraEntity();

		const std::string& GetName() const { return m_Name; }
		void SetName(const std::string& name) { m_Name = name; }
		// The scene asset this scene was loaded from or saved as; null for scenes made otherwise (in code, or new ones not
		// saved yet). SceneAsset::CreateScene sets it, and so do owners that save a scene as a new asset. Copies keep it.
		AssetHandle GetSourceAsset() const { return m_SourceAsset; }
		void SetSourceAsset(AssetHandle asset) { m_SourceAsset = asset; }
		// Recognizes the scene from one play session to the next, for what systems remember about it in between (PhysicsSystem:
		// what its physics worlds lacked): its source asset, else an ID the scene got when it was constructed. Copies keep it,
		// so the copy an owner plays has the ID of the scene it was made from; other scenes never share it, whatever their
		// names.
		UUID GetLineageID() const { return m_SourceAsset.IsValid() ? m_SourceAsset : m_LocalLineageID; }
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

		// Service registry: `provider` points to the interface (already cast to it), keyed by the interface's type hash.
		bool RegisterServiceProvider(entt::id_type type, std::string_view typeName, void* provider);
		void UnregisterServiceProvider(entt::id_type type, const void* provider);
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
		// Sub-scenes (Scene/SceneSubScenes.cpp): advances one load, creating at most `budget` entities (which it lowers) while
		// time is left before `deadline`; fails one, destroying what it created, with the reason logged.
		void AdvanceSubScene(SubSceneLoad& load, size_t& budget, std::chrono::steady_clock::time_point deadline);
		void StartSubSceneInstantiation(SubSceneLoad& load);
		void FailSubScene(SubSceneLoad& load, std::string error);
		void DestroySubSceneEntities(SubSceneLoad& load);
		SubSceneLoad* FindSubScene(SubSceneID id) const;
		// Whether the entity an instantiation created for its prototype entity `index` still exists.
		bool IsInstantiatedEntityAlive(const PrototypeInstantiation& instantiation, size_t index) const;
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
		// A scene whose signal handlers are not connected yet (Copy fills it first, then connects them).
		Scene(std::string name, bool connectSignals);
		// Connects the handlers of the components whose signals keep the scene's caches (dirty transforms, activity, lookups).
		void ConnectSignals();

		void OnTransformChanged(entt::registry& registry, entt::entity handle);
		void OnInactiveAdded(entt::registry& registry, entt::entity handle);
		void OnInactiveRemoved(entt::registry& registry, entt::entity handle);
		void OnNameChanged(entt::registry& registry, entt::entity handle);
		void OnNameRemoved(entt::registry& registry, entt::entity handle);
		void OnTagChanged(entt::registry& registry, entt::entity handle);
		void OnTagRemoved(entt::registry& registry, entt::entity handle);
	private:
		std::string m_Name;
		AssetHandle m_SourceAsset = UUID::Null();
		UUID m_LocalLineageID;                     // Random; see GetLineageID
		entt::registry m_Registry;
		// UUID -> entity. A dense map: lookups probe one packed array, and copying it (Copy) copies two arrays.
		entt::dense_map<UUID, entt::entity, std::hash<UUID>> m_EntityMap;
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

		// A running system whose per-frame callback the scene calls, with its stage.
		struct SystemCall
		{
			SceneSystem* System = nullptr;
			FrameStageID Stage = c_InvalidFrameStage;
		};

		std::vector<Scope<SceneSystem>> m_Systems;
		std::vector<FrameStageID> m_SystemStages; // Per system: its FrameProfiler stage (SceneSystemDescriptor::ProfileStage)
		// In update order, the systems whose descriptors ask for OnUpdate, OnFixedUpdate, OnLateUpdate and OnPausedUpdate.
		std::vector<SystemCall> m_UpdateCalls;
		std::vector<SystemCall> m_FixedUpdateCalls;
		std::vector<SystemCall> m_LateUpdateCalls;
		std::vector<SystemCall> m_PausedUpdateCalls;
		std::unordered_map<entt::id_type, SceneSystem*> m_SystemsByType; // SceneSystemDescriptor::Type -> running system
		std::unordered_map<entt::id_type, void*> m_Services;              // Interface type -> provider (see RegisterService)
		std::vector<UUID> m_PendingDestroy;
		std::unordered_set<UUID> m_PendingDestroySet;
		SceneRuntimeMode m_RuntimeMode = SceneRuntimeMode::Play;
		bool m_IsRunning = false;
		bool m_IsUpdating = false;
		bool m_IsPaused = false;
		uint32_t m_StepFrames = 0;
		uint64_t m_FixedSteps = 0;
		float m_FixedTimeAccumulator = 0.0f;
		float m_TimeScale = 1.0f;
		bool m_TimeDilation = false;
		uint64_t m_DroppedFixedSteps = 0;
		double m_Time = 0.0;
		double m_UnscaledTime = 0.0;
		float m_UnscaledDeltaTime = 0.0f;
		uint64_t m_FrameIndex = 0;
		std::optional<int32_t> m_QuitRequest;
		std::optional<UUID> m_SceneLoadRequest;
		std::optional<float> m_SceneLoadProgress;
		// Sub-scenes in the order of their loads (Scene/SceneSubScenes.cpp).
		std::vector<Scope<SubSceneLoad>> m_SubScenes;
		SubSceneID m_NextSubSceneID = 1;
		bool m_AdvancingSubScenes = false;               // UpdateSubScenes runs: unloads wait for its end
		std::vector<SubSceneID> m_DeferredSubSceneUnloads; // Unloads requested meanwhile

		// Declared last: disconnected before anything the handlers use is destroyed.
		std::vector<entt::scoped_connection> m_Connections;

		friend class Entity;
		friend class SceneSerializer;
	};

}
