#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Core/UUID.h"
#include "Strata/Physics/PhysicsTypes.h"
#include "Strata/Reflection/Property.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneSystem.h"
#include "Strata/Scripting/ScriptTypes.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Opaque context handed to script modules (see StrataScript/ScriptABI.h).
struct StrataScriptContext;

namespace Strata
{

	class Scene;
	class ScriptEngine;
	class ScriptModule;
	struct ScriptCallSite;
	enum class ScriptCallResult : uint8_t;

	// Runs the Script components of a playing scene through the active ScriptEngine. It is the built-in "Scripting" scene
	// system (created on Scene::OnRuntimeStart in play mode, not in simulate mode). Main thread only.
	//
	// Lifecycle: every ScriptComponent entry gets one instance of its class: constructed with its field overrides applied
	// (ScriptEntry::Fields; unknown or mistyped overrides are ignored with a warning), then OnCreate once every instance
	// that exists at that point is constructed. Entities created or given scripts while playing (prefabs, AddScript) get
	// their instances immediately, and OnCreate before their first update. Removing a script or destroying its entity calls
	// OnDestroy (while the entity is still valid) and deletes the instance; stopping calls OnDestroy for every instance,
	// descendants before ancestors.
	//
	// Order: instances update in entity hierarchy order (parents first), then in entry order on each entity. The order is
	// brought up to date at the start of a frame: instances created since are inserted at their entity's position (found by
	// binary search), those of scripted entities that moved in the hierarchy (Scene::GetHierarchyMoves) are placed again,
	// and destroyed ones stay listed but skipped until they could make up an eighth of the order. So spawning and destroying
	// scripted entities costs a few hierarchy comparisons and a move of the lists' entries, moving scripted entities (and
	// the occasional compaction) a pass over the instances, and frames that only create, destroy or move entities without
	// scripts nothing; the whole scene's hierarchy is walked only after a module reload or for changes too many to place
	// one by one. Instances created during a frame join the following phases of that frame. Each update callback is
	// dispatched only to the instances whose class implements it. Inactive entities receive no update callbacks (OnCreate
	// and OnDestroy run regardless); entities destroyed during a frame keep updating until it ends. An instance whose
	// callback throws is disabled; a crash faults the whole module (see ScriptEngine).
	//
	// Contacts: the scene's PhysicsSystem reports contact changes after each step; the scripts on both entities (those owning
	// the bodies) receive OnCollisionEnter/Exit, or OnTriggerEnter/Exit when either body is a trigger, with the other entity
	// and the contact normal pointing towards it. Contacts begin only for the scripts of active entities; each instance that got
	// an Enter gets the matching Exit, also when its entity was deactivated meanwhile (but not once the script was removed or
	// disabled, or its entity destroyed). The callbacks run while the scene updates, so the entities they destroy stay valid
	// until the frame ends; the other side of an Exit caused by destruction may already be gone.
	class ScriptSystem final : public SceneSystem
	{
	public:
		// `engine` may be null: the scene then plays without scripts (warning once if it has Script components).
		ScriptSystem(Scene& scene, Ref<ScriptEngine> engine);
		~ScriptSystem() override;

		ScriptSystem(const ScriptSystem&) = delete;
		ScriptSystem& operator=(const ScriptSystem&) = delete;

		void OnRuntimeStart() override;
		// The scripts start here (instances, then OnCreate), once every system of the scene runs: OnCreate can use physics.
		void OnRuntimeStarted() override;
		void OnRuntimeStop() override;
		void OnUpdate(Timestep timestep) override;
		void OnFixedUpdate(float fixedTimestep) override;
		void OnLateUpdate(Timestep timestep) override;
		void OnEntityDestroying(const Entity& entity) override;

		Scene& GetScene() const { return m_Scene; }
		ScriptEngine* GetEngine() const { return m_Engine.get(); }
		StrataScriptContext* GetContext() const { return m_Context; }

		// Live script instances (for the editor inspector and tests).
		size_t GetInstanceCount() const;
		bool HasInstance(Entity entity, std::string_view className) const;
		// Current value of a field of a live instance.
		std::optional<PropertyValue> GetFieldValue(Entity entity, std::string_view className, std::string_view fieldName) const;
		// Assigns a field of a live instance (the value must have the field's type).
		bool SetFieldValue(Entity entity, std::string_view className, std::string_view fieldName, const PropertyValue& value);
		// How often an entity's instances were matched against its Script component since the system was created
		// (diagnostics: the work grows with the number of changed entities, not with their square).
		uint64_t GetReconcileCount() const { return m_ReconcileCount; }
		// How often the update order changed since the system was created (diagnostics: only frames after instances were
		// created, after scripted entities moved in the hierarchy, or once destroyed instances add up, change it), and how
		// many of those changes walked the whole scene's hierarchy instead of placing the instances concerned.
		uint64_t GetUpdateOrderRebuildCount() const { return m_UpdateOrderRebuildCount; }
		uint64_t GetFullUpdateOrderBuildCount() const { return m_FullUpdateOrderBuildCount; }

		//////////////////////////////////////////////////////////////////////////
		// Script host API support
		//////////////////////////////////////////////////////////////////////////

		// Delta time of the current frame (scaled).
		float GetDeltaTime() const { return m_DeltaTime; }
		// The module-side instance of a script on an entity (null if there is no usable one).
		void* GetInstanceHandle(Entity entity, std::string_view className) const;
		// Adds a ScriptComponent entry and creates its instance right away (OnCreate follows at the next sync point).
		bool AddScript(Entity entity, std::string_view className, std::string* outError = nullptr);
		// Removes the entry; the instance receives no more callbacks and is destroyed at the next sync point.
		bool RemoveScript(Entity entity, std::string_view className);
		// Destroys an entity on behalf of a script (deferred until the scene can destroy it safely).
		void DestroyEntity(Entity entity);
		// Creates the instances of Script components that appeared since the last sync point (e.g. after instantiating a
		// prefab), so scripts can access them immediately.
		void CreatePendingInstances();
		// Logs a problem with a script's use of the API, naming the calling script; each distinct message is logged once.
		void ReportProblem(std::string_view function, const std::string& message);
	private:
		struct Instance
		{
			UUID Entity = UUID::Null();
			// The entity's handle, so that dispatch reads its activity without looking the UUID up (checked against the UUID
			// before use: the handle of a destroyed entity may be recycled).
			entt::entity EntityHandle = entt::null;
			std::string ClassName;
			const ScriptClassInfo* Class = nullptr; // Null between the halves of a module reload
			void* Handle = nullptr;                 // Module-side instance; null until constructed and after destruction
			bool Created = false;                   // OnCreate has run (or was attempted)
			bool Disabled = false;                  // A callback threw; no further callbacks
			bool Removed = false;                   // Destroyed or about to be; no further callbacks
			bool Restore = false;                   // Reload: ReloadFields hold a snapshot to restore
			bool Placed = false;                    // In the update order at its entity's position (not just appended)
			bool Placing = false;                   // Scratch of RebuildUpdateOrder: being moved to its entity's new position
			std::vector<ScriptFieldValue> ReloadFields;
			// Entities whose contacts with this one began for this instance and have not ended yet: the Exit goes to exactly the
			// instances that got the Enter (kept across hot reloads).
			std::unordered_set<UUID> Contacts;
		};

		friend class ScriptEngine;
		// Module reload protocol (see ScriptEngine::LoadModule).
		void BeginModuleReload();
		void EndModuleReload();
		void OnModuleLoaded();
		void OnModuleUnloading();

		ScriptModule* GetUsableModule() const;
		ScriptCallSite MakeCallSite(const Instance& instance, const char* method) const;

		void ConnectSignals();
		void OnScriptComponentChanged(entt::registry& registry, entt::entity handle);
		void MarkDirty(UUID entity);

		// Applies pending changes at a safe point (no script code on the stack): deferred destruction, instance creation and
		// removal, OnReload and OnCreate. Repeats while scripts keep creating work (bounded per call).
		void SyncPoint();
		bool HasPendingWork() const;
		void FlushDeferredDestroys();
		void ReconcileDirtyEntities(bool allowRemovals);
		void ReconcileEntity(UUID entityID, bool allowRemovals);
		void RunPendingReloads();
		void RunPendingStarts();

		Ref<Instance> FindInstance(UUID entity, std::string_view className) const;
		Ref<Instance> CreateInstance(Entity entity, const ScriptEntry& entry, const ScriptClassInfo& info);
		bool ConstructInstance(Instance& instance, const ScriptEntry* entry);
		void ApplyFieldOverrides(Instance& instance, const std::vector<ScriptFieldValue>& fields, bool reportProblems);
		void DestroyInstance(Instance& instance, bool callOnDestroy);
		void DestroyAllInstances(bool callOnDestroy);
		// Destroys the instances RemoveScript flagged (OnDestroy, then deletion) in reverse `order` without creating any:
		// where the sync point that would destroy them does not come in time.
		void DestroyRemovedInstances(std::vector<Ref<Instance>> order);
		// Drops the entity's instances that are removed and deleted. Flagged instances whose script object still exists
		// stay until they are destroyed: dropping them would leak them without OnDestroy.
		void RemoveDestroyedInstances(UUID entity);
		static bool IsDestroyed(const Ref<Instance>& instance) { return instance->Removed && !instance->Handle; }
		// Brings the update order and the dispatch lists up to date unless neither the hierarchy nor the set of instances
		// changed: places new instances and those of moved scripted entities, drops the destroyed ones once they are many.
		void RebuildUpdateOrder();
		// Computes the update order from scratch by walking the scene's hierarchy order.
		void BuildUpdateOrderFromScene();
		// Merges groups of instances (or their dispatch entries), one per entity of `entities` (in hierarchy order), into
		// list[0, count), a list in hierarchy order: each goes before the first entry whose entity comes after its entity.
		template<typename Element>
		void MergeIntoOrder(std::vector<Element>& list, size_t count, const std::vector<entt::entity>& entities, std::vector<std::vector<Element>>& groups) const;
		// The first position in list[first, last) whose entity comes after `entity`, skipping the destroyed instances that stay
		// listed until the list is compacted.
		template<typename Element>
		size_t FindOrderPosition(const std::vector<Element>& list, size_t first, size_t last, Entity entity) const;
		// The instance's object exists and its entity is the one it was created for (destroyed entities' handles are reused).
		bool IsListed(const Instance& instance) const;
		// Drops the update order (and the dispatch lists, which point into it); the next RebuildUpdateOrder recomputes it.
		void ClearUpdateOrder();
		// Appends an instance to the update order (unplaced until the next RebuildUpdateOrder) and to the dispatch lists of
		// the callbacks its class implements.
		void AppendToUpdateOrder(const Ref<Instance>& instance);
		void AddToDispatchLists(Instance& instance);
		void RebuildDispatchLists();
		std::vector<Instance*>& GetDispatchList(ScriptCallback callback);
		// Every instance in update order (but the destroyed ones), followed by those of entities that left the scene without
		// notice.
		std::vector<Ref<Instance>> CollectInstances();

		void RunCallbacks(ScriptCallback callback, float argument);
		void Invoke(Instance& instance, ScriptCallback callback, float argument);
		// Disables an instance whose callback threw.
		void HandleCallResult(Instance& instance, ScriptCallback callback, ScriptCallResult result);

		void OnCollision(const CollisionEvent& event);
		// Calls a contact callback on the scripts of an entity, for its contact with "other" (the normal points towards it).
		void DeliverContact(UUID entityID, UUID otherID, bool begin, ScriptCallback callback, const glm::vec3& point, const glm::vec3& normal);
	private:
		Scene& m_Scene;
		Ref<ScriptEngine> m_Engine;
		StrataScriptContext* m_Context = nullptr;

		std::unordered_map<UUID, std::vector<Ref<Instance>>> m_Instances; // Per entity, in entry order
		// Owns what the dispatch lists point to. The first m_PlacedCount instances are in hierarchy order as of
		// m_OrderHierarchyVersion; instances created since follow in creation order. Destroyed instances stay listed (the
		// dispatch skips them) until compacting the lists pays off.
		std::vector<Ref<Instance>> m_UpdateOrder;
		size_t m_PlacedCount = 0;
		// The instances of m_UpdateOrder (in that order) whose class implements OnUpdate, OnFixedUpdate or OnLateUpdate.
		std::vector<Instance*> m_UpdateDispatch;
		std::vector<Instance*> m_FixedUpdateDispatch;
		std::vector<Instance*> m_LateUpdateDispatch;
		// The update order is current while both versions match: the scene's hierarchy version and m_InstanceSetVersion,
		// which changes whenever instances are created, destroyed or moved to another module.
		uint64_t m_InstanceSetVersion = 1;
		uint64_t m_OrderInstanceSetVersion = 0;
		uint64_t m_OrderHierarchyVersion = 0;
		// Instances destroyed since the lists were last compacted: at most that many destroyed instances are listed.
		size_t m_RemovalsSinceCompaction = 0;
		bool m_OrderNeedsFullBuild = false; // Instances exist that the order does not list (a module reload recreated them)
		uint64_t m_UpdateOrderRebuildCount = 0;
		uint64_t m_FullUpdateOrderBuildCount = 0;
		bool m_Dispatching = false; // While a dispatch list is walked, the order must not be rebuilt
		std::vector<Ref<Instance>> m_PendingStart;   // Constructed, waiting for OnCreate (creation order)
		std::vector<Ref<Instance>> m_PendingReloads; // Recreated by a reload, waiting for OnReload
		std::vector<Ref<Instance>> m_ReloadOrder;    // Instances between the halves of a module reload

		std::vector<UUID> m_DirtyEntities; // Script components changed since the last sync point (in change order)
		std::unordered_set<UUID> m_DirtySet;
		// Changed entities the next creation-only pass (CreatePendingInstances) takes; the cursor is shared by nested passes.
		std::vector<UUID> m_CreationQueue;
		std::unordered_set<UUID> m_CreationQueued;
		size_t m_CreationCursor = 0;
		uint64_t m_ReconcileCount = 0;
		bool m_ReconcileAll = false;
		std::vector<UUID> m_DeferredDestroys;

		std::unordered_set<std::string> m_ReportedProblems;
		CollisionListenerID m_CollisionListener = c_InvalidCollisionListener; // Registered with the scene's PhysicsSystem while running
		std::vector<entt::scoped_connection> m_Connections;
		float m_DeltaTime = 0.0f;
		bool m_Running = false;
		// Set while instances are being torn down or moved to a new module: no instances are created meanwhile (pending
		// entries are created at the next sync point).
		bool m_CreationBlocked = false;
	};

}
