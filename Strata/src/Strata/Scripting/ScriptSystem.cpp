#include "stpch.h"
#include "Strata/Scripting/ScriptSystem.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "Strata/Scripting/ScriptHostAPI.h"
#include "Strata/Scripting/ScriptModule.h"

namespace Strata
{

	namespace
	{

		// Scripts that keep creating work for the current sync point (e.g. a prefab whose script spawns the same prefab
		// in OnCreate) must not hang the frame: what is left after this many rounds waits for the next sync point.
		constexpr uint32_t c_MaxSyncRounds = 64;
		// Distinct API problems reported per playing scene before further ones are dropped.
		constexpr size_t c_MaxReportedProblems = 1024;

		std::string DescribeEntity(const Scene& scene, UUID entityID)
		{
			if (const Entity entity = scene.GetEntityByUUID(entityID))
				return fmt::format("'{}'", entity.GetName());
			return entityID.ToString();
		}

	}

	ScriptSystem::ScriptSystem(Scene& scene, Ref<ScriptEngine> engine)
		: m_Scene(scene), m_Engine(std::move(engine))
	{
		m_Context = RegisterScriptContext(*this);
		if (m_Engine)
			m_Engine->AttachSystem(*this);
	}

	ScriptSystem::~ScriptSystem()
	{
		// Scenes stop their systems before destroying them; this only matters if that did not happen.
		if (m_Running)
		{
			m_CreationBlocked = true;
			DestroyAllInstances(false);
		}
		m_Connections.clear();
		if (m_Engine)
			m_Engine->DetachSystem(*this);
		UnregisterScriptContext(*this);
	}

	ScriptModule* ScriptSystem::GetUsableModule() const
	{
		ScriptModule* module = m_Engine ? m_Engine->GetModule() : nullptr;
		return module && !module->IsFaulted() ? module : nullptr;
	}

	ScriptCallSite ScriptSystem::MakeCallSite(const Instance& instance, const char* method) const
	{
		return ScriptCallSite { instance.Class, method, instance.Entity, &m_Scene };
	}

	////////////////////////////////////////////////////////////////////////////////
	// Runtime
	////////////////////////////////////////////////////////////////////////////////

	void ScriptSystem::OnRuntimeStart()
	{
		ST_PROFILE_FUNCTION();

		m_Running = true;
		m_CreationBlocked = false;
		ConnectSignals();
		// The physics system is created with this one (systems start after all of them exist) and stops before it.
		if (PhysicsSystem* physics = m_Scene.GetSystem<PhysicsSystem>())
			m_CollisionListener = physics->AddCollisionListener([this](const CollisionEvent& event) { OnCollision(event); });

		const std::vector<Entity> entities = m_Scene.GetEntitiesInHierarchyOrder();
		for (const Entity entity : entities)
		{
			if (entity.HasComponent<ScriptComponent>())
				MarkDirty(entity.GetUUID());
		}

		if (!m_DirtyEntities.empty() && !(m_Engine && m_Engine->IsModuleLoaded()))
			ST_CORE_WARN("Scene '{}' has Script components but no script module is loaded; its scripts do not run", m_Scene.GetName());
	}

	void ScriptSystem::OnRuntimeStarted()
	{
		ST_PROFILE_FUNCTION();

		SyncPoint();
	}

	void ScriptSystem::OnRuntimeStop()
	{
		ST_PROFILE_FUNCTION();

		if (!m_Running)
			return;

		if (m_CollisionListener != c_InvalidCollisionListener)
		{
			if (PhysicsSystem* physics = m_Scene.GetSystem<PhysicsSystem>())
				physics->RemoveCollisionListener(m_CollisionListener);
			m_CollisionListener = c_InvalidCollisionListener;
		}
		m_CreationBlocked = true;
		DestroyAllInstances(true);
		// After OnDestroy, which may still request or release assets: what the scripts kept loaded may go now.
		m_AssetPins.clear();
		m_Connections.clear();
		m_UpdateOrder.clear();
		m_PendingStart.clear();
		m_PendingReloads.clear();
		m_DirtyEntities.clear();
		m_DirtySet.clear();
		m_CreationQueue.clear();
		m_CreationQueued.clear();
		m_CreationCursor = 0;
		m_DeferredDestroys.clear();
		m_ReconcileAll = false;
		m_Running = false;
		m_CreationBlocked = false;
	}

	void ScriptSystem::OnUpdate(Timestep timestep)
	{
		ST_PROFILE_FUNCTION();

		m_DeltaTime = timestep;
		SyncPoint();
		RebuildUpdateOrder();
		RunCallbacks(ScriptCallback::OnUpdate, timestep);
		SyncPoint();
	}

	void ScriptSystem::OnFixedUpdate(float fixedTimestep)
	{
		ST_PROFILE_FUNCTION();

		RunCallbacks(ScriptCallback::OnFixedUpdate, fixedTimestep);
		SyncPoint();
	}

	void ScriptSystem::OnLateUpdate(Timestep timestep)
	{
		ST_PROFILE_FUNCTION();

		m_DeltaTime = timestep;
		RunCallbacks(ScriptCallback::OnLateUpdate, timestep);
		SyncPoint();
	}

	void ScriptSystem::OnEntityDestroying(const Entity& entity)
	{
		const UUID entityID = entity.GetUUID();
		auto it = m_Instances.find(entityID);
		if (it == m_Instances.end())
			return;

		// A copy: OnDestroy runs script code, which may change the instance lists.
		const std::vector<Ref<Instance>> instances = it->second;
		for (auto instance = instances.rbegin(); instance != instances.rend(); ++instance)
			DestroyInstance(**instance, true);
		RemoveDestroyedInstances(entityID);

		// Should the entity survive after all (a script moved it out of the destroyed hierarchy), it gets new instances.
		if (entity.HasComponent<ScriptComponent>())
			MarkDirty(entityID);
	}

	void ScriptSystem::RunCallbacks(ScriptCallback callback, float argument)
	{
		ScriptModule* module = GetUsableModule();
		if (!module || !m_Running)
			return;

		// Instances created during this phase are appended and wait for the next phase (indices stay valid).
		const size_t count = m_UpdateOrder.size();
		for (size_t index = 0; index < count; index++)
		{
			const Ref<Instance> instance = m_UpdateOrder[index];
			if (instance->Removed || instance->Disabled || !instance->Created || !instance->Handle || !instance->Class->Implements(callback))
				continue;

			// Entities destroyed during this frame keep updating until the frame ends, like every other system sees them.
			const Entity entity = m_Scene.GetEntityByUUID(instance->Entity);
			if (!entity || !m_Scene.IsActiveInHierarchy(entity))
				continue;

			Invoke(*instance, callback, argument);
			if (module->IsFaulted())
				return;
		}
	}

	void ScriptSystem::Invoke(Instance& instance, ScriptCallback callback, float argument)
	{
		ScriptModule* module = GetUsableModule();
		if (!module || !instance.Handle)
			return;

		const ScriptCallSite site = MakeCallSite(instance, ScriptCallbackToString(callback));
		HandleCallResult(instance, callback, module->InvokeCallback(site, instance.Handle, callback, argument));
	}

	void ScriptSystem::HandleCallResult(Instance& instance, ScriptCallback callback, ScriptCallResult result)
	{
		if (result != ScriptCallResult::Exception)
			return;

		instance.Disabled = true;
		const ScriptModule* module = m_Engine ? m_Engine->GetModule() : nullptr;
		Log::GetScriptLogger()->error("Script '{}' on entity {} threw an exception in {}: {}. The script is disabled.", instance.ClassName,
			DescribeEntity(m_Scene, instance.Entity), ScriptCallbackToString(callback), module ? module->GetLastExceptionMessage() : std::string());
	}

	////////////////////////////////////////////////////////////////////////////////
	// Contacts
	////////////////////////////////////////////////////////////////////////////////

	void ScriptSystem::OnCollision(const CollisionEvent& event)
	{
		const bool begin = event.Type == CollisionEventType::Begin;
		ScriptCallback callback = begin ? ScriptCallback::OnCollisionEnter : ScriptCallback::OnCollisionExit;
		if (event.IsTrigger)
			callback = begin ? ScriptCallback::OnTriggerEnter : ScriptCallback::OnTriggerExit;
		DeliverContact(event.AID, event.BID, begin, callback, event.Point, event.Normal);
		DeliverContact(event.BID, event.AID, begin, callback, event.Point, -event.Normal);
	}

	void ScriptSystem::DeliverContact(UUID entityID, UUID otherID, bool begin, ScriptCallback callback, const glm::vec3& point, const glm::vec3& normal)
	{
		if (!m_Running || !GetUsableModule())
			return;
		auto it = m_Instances.find(entityID);
		if (it == m_Instances.end())
			return;
		// Like the update callbacks, contacts begin for the scripts of active entities only (entities destroyed during this
		// frame included). A contact that began ends for the scripts that were told, also when their entity was deactivated
		// meanwhile (that ends its contacts): every Enter has its Exit.
		const Entity entity = m_Scene.GetEntityByUUID(entityID);
		if (!entity || (begin && !m_Scene.IsActiveInHierarchy(entity)))
			return;

		StrataScriptCollision contact = {};
		contact.StructSize = sizeof(StrataScriptCollision);
		contact.Other = static_cast<uint64_t>(otherID);
		for (int axis = 0; axis < 3; axis++)
		{
			contact.Point[axis] = point[axis];
			contact.Normal[axis] = normal[axis];
		}

		// A copy: the callbacks may add or remove scripts (removed instances are flagged and skipped).
		const std::vector<Ref<Instance>> instances = it->second;
		for (const Ref<Instance>& instance : instances)
		{
			if (instance->Removed || instance->Disabled || !instance->Created || !instance->Handle)
				continue;
			// Whether or not the script implements the callback: one that only implements the Exit still gets it.
			if (begin)
				instance->Contacts.insert(otherID);
			else if (instance->Contacts.erase(otherID) == 0)
				continue;
			if (!instance->Class->Implements(callback))
				continue;

			ScriptModule* module = GetUsableModule();
			if (!module)
				return;
			const ScriptCallSite site = MakeCallSite(*instance, ScriptCallbackToString(callback));
			HandleCallResult(*instance, callback, module->InvokeContactCallback(site, instance->Handle, callback, contact));
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Change tracking
	////////////////////////////////////////////////////////////////////////////////

	void ScriptSystem::ConnectSignals()
	{
		m_Connections.clear();
		entt::registry& registry = m_Scene.GetRegistry();
		m_Connections.emplace_back(registry.on_construct<ScriptComponent>().connect<&ScriptSystem::OnScriptComponentChanged>(*this));
		m_Connections.emplace_back(registry.on_update<ScriptComponent>().connect<&ScriptSystem::OnScriptComponentChanged>(*this));
		m_Connections.emplace_back(registry.on_destroy<ScriptComponent>().connect<&ScriptSystem::OnScriptComponentChanged>(*this));
	}

	void ScriptSystem::OnScriptComponentChanged(entt::registry& registry, entt::entity handle)
	{
		// While an entity is being destroyed its ID may already be gone; a full pass then finds the orphaned instances.
		if (const IDComponent* id = registry.try_get<IDComponent>(handle))
			MarkDirty(id->ID);
		else
			m_ReconcileAll = true;
	}

	void ScriptSystem::MarkDirty(UUID entity)
	{
		if (m_DirtySet.insert(entity).second)
			m_DirtyEntities.push_back(entity);
		if (m_CreationQueued.insert(entity).second)
			m_CreationQueue.push_back(entity);
	}

	bool ScriptSystem::HasPendingWork() const
	{
		return !m_DeferredDestroys.empty() || !m_DirtyEntities.empty() || m_ReconcileAll || !m_PendingStart.empty() || !m_PendingReloads.empty();
	}

	void ScriptSystem::SyncPoint()
	{
		for (uint32_t round = 0; m_Running && HasPendingWork(); round++)
		{
			if (round == c_MaxSyncRounds)
			{
				ReportProblem("Scripting", fmt::format("scripts keep creating scripted entities while starting; the rest is created at the next update "
					"(after {} rounds)", c_MaxSyncRounds));
				// Removals still complete now (each instance is destroyed once, so this ends); creation waits.
				DestroyRemovedInstances(m_UpdateOrder);
				return;
			}

			FlushDeferredDestroys();
			ReconcileDirtyEntities(true);
			RunPendingReloads();
			RunPendingStarts();
		}
	}

	void ScriptSystem::FlushDeferredDestroys()
	{
		const std::vector<UUID> destroys = std::move(m_DeferredDestroys);
		m_DeferredDestroys.clear();
		for (UUID entityID : destroys)
		{
			if (const Entity entity = m_Scene.GetEntityByUUID(entityID))
				m_Scene.DestroyEntity(entity);
		}
	}

	void ScriptSystem::ReconcileDirtyEntities(bool allowRemovals)
	{
		if (m_ReconcileAll)
		{
			// Every entity that has scripts or instances, in hierarchy order so creation stays deterministic.
			m_ReconcileAll = false;
			for (const Entity entity : m_Scene.GetEntitiesInHierarchyOrder())
			{
				if (entity.HasComponent<ScriptComponent>() || m_Instances.count(entity.GetUUID()))
					MarkDirty(entity.GetUUID());
			}
			std::vector<UUID> orphaned;
			for (const auto& [entityID, instances] : m_Instances)
			{
				if (!m_Scene.GetEntityByUUID(entityID))
					orphaned.push_back(entityID);
			}
			std::sort(orphaned.begin(), orphaned.end());
			for (UUID entityID : orphaned)
				MarkDirty(entityID);
		}

		if (allowRemovals)
		{
			const std::vector<UUID> dirty = std::move(m_DirtyEntities);
			m_DirtyEntities.clear();
			m_DirtySet.clear();
			// The entities queued for creation are all among them (MarkDirty queues both).
			m_CreationQueue.clear();
			m_CreationQueued.clear();
			m_CreationCursor = 0;
			for (UUID entityID : dirty)
				ReconcileEntity(entityID, true);
			return;
		}

		// Creation only (script code may be on the stack): the entities stay queued for the next full pass. Each entity
		// changed since the previous creation pass is reconciled once, so spawning many scripted entities in one update
		// stays linear. Script code run by the creations may queue more entities; this loop takes them too, and so does a
		// creation pass nested in that code (both advance the same cursor).
		while (m_CreationCursor < m_CreationQueue.size())
		{
			const UUID entityID = m_CreationQueue[m_CreationCursor++];
			m_CreationQueued.erase(entityID);
			ReconcileEntity(entityID, false);
		}
		m_CreationQueue.clear();
		m_CreationCursor = 0;
	}

	void ScriptSystem::ReconcileEntity(UUID entityID, bool allowRemovals)
	{
		m_ReconcileCount++;
		const Entity entity = m_Scene.GetEntityByUUID(entityID);

		if (allowRemovals)
		{
			auto it = m_Instances.find(entityID);
			if (it != m_Instances.end())
			{
				// Copies: destroying instances runs script code, which may change the component and the lists.
				const std::vector<Ref<Instance>> instances = it->second;
				for (auto instance = instances.rbegin(); instance != instances.rend(); ++instance)
				{
					const ScriptComponent* component = entity ? entity.TryGetComponent<ScriptComponent>() : nullptr;
					if (!(*instance)->Removed && component && component->FindScript((*instance)->ClassName))
						continue;
					DestroyInstance(**instance, entity.IsValid());
				}
				RemoveDestroyedInstances(entityID);
			}
		}

		const ScriptComponent* component = entity ? entity.TryGetComponent<ScriptComponent>() : nullptr;
		ScriptModule* module = GetUsableModule();
		if (!component || component->Scripts.empty() || m_CreationBlocked || !module)
			return;

		// Usually every entry has its instance already (the entity changed in some other way): then there is nothing to
		// create, and no reason to copy the entries.
		const bool complete = std::all_of(component->Scripts.begin(), component->Scripts.end(),
			[&](const ScriptEntry& entry) { return FindInstance(entityID, entry.ClassName) != nullptr; });
		if (!complete)
		{
			// A copy: constructors run script code, which may change the component.
			const std::vector<ScriptEntry> entries = component->Scripts;
			std::unordered_set<std::string> seen;
			for (const ScriptEntry& entry : entries)
			{
				if (!seen.insert(entry.ClassName).second)
				{
					ReportProblem("Script component", fmt::format("entity {} lists script '{}' more than once; the duplicate is ignored", DescribeEntity(m_Scene, entityID), entry.ClassName));
					continue;
				}
				if (FindInstance(entityID, entry.ClassName))
					continue;

				const ScriptClassInfo* info = module->FindClass(entry.ClassName);
				if (!info)
				{
					ReportProblem("Script component", fmt::format("entity {} uses script '{}', which the module '{}' does not contain", DescribeEntity(m_Scene, entityID),
						entry.ClassName, module->GetName()));
					continue;
				}

				CreateInstance(entity, entry, *info);
				if (!GetUsableModule() || !entity.IsValid())
					return;
			}
		}

		// Keep the instances in entry order (the update order on this entity).
		auto it = m_Instances.find(entityID);
		const ScriptComponent* current = entity.TryGetComponent<ScriptComponent>();
		if (it != m_Instances.end() && current)
		{
			auto entryIndex = [current](const Ref<Instance>& instance)
			{
				for (size_t index = 0; index < current->Scripts.size(); index++)
				{
					if (current->Scripts[index].ClassName == instance->ClassName)
						return index;
				}
				return current->Scripts.size();
			};
			std::stable_sort(it->second.begin(), it->second.end(), [&](const Ref<Instance>& a, const Ref<Instance>& b) { return entryIndex(a) < entryIndex(b); });
		}
	}

	void ScriptSystem::RunPendingReloads()
	{
		const std::vector<Ref<Instance>> reloads = std::move(m_PendingReloads);
		m_PendingReloads.clear();
		for (const Ref<Instance>& instance : reloads)
		{
			if (!GetUsableModule())
				return;
			if (!instance->Removed && instance->Handle && !instance->Disabled && instance->Class->Implements(ScriptCallback::OnReload))
				Invoke(*instance, ScriptCallback::OnReload, 0.0f);
		}
	}

	void ScriptSystem::RunPendingStarts()
	{
		const std::vector<Ref<Instance>> starts = std::move(m_PendingStart);
		m_PendingStart.clear();
		for (const Ref<Instance>& instance : starts)
		{
			if (!GetUsableModule())
				return;
			if (instance->Removed || instance->Created || !instance->Handle)
				continue;

			// Marked first: OnCreate runs once, even if it throws.
			instance->Created = true;
			if (instance->Class->Implements(ScriptCallback::OnCreate))
				Invoke(*instance, ScriptCallback::OnCreate, 0.0f);
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Instances
	////////////////////////////////////////////////////////////////////////////////

	Ref<ScriptSystem::Instance> ScriptSystem::FindInstance(UUID entity, std::string_view className) const
	{
		auto it = m_Instances.find(entity);
		if (it == m_Instances.end())
			return nullptr;
		for (const Ref<Instance>& instance : it->second)
		{
			if (!instance->Removed && instance->ClassName == className)
				return instance;
		}
		return nullptr;
	}

	Ref<ScriptSystem::Instance> ScriptSystem::CreateInstance(Entity entity, const ScriptEntry& entry, const ScriptClassInfo& info)
	{
		Ref<Instance> instance = CreateRef<Instance>();
		instance->Entity = entity.GetUUID();
		instance->ClassName = info.Name;
		instance->Class = &info;

		// Registered before construction, so script code run by the constructor sees it as existing (and never creates it
		// a second time).
		m_Instances[instance->Entity].push_back(instance);
		if (!ConstructInstance(*instance, &entry))
		{
			instance->Removed = true;
			RemoveDestroyedInstances(instance->Entity);
			return nullptr;
		}

		m_UpdateOrder.push_back(instance);
		m_PendingStart.push_back(instance);
		return instance;
	}

	bool ScriptSystem::ConstructInstance(Instance& instance, const ScriptEntry* entry)
	{
		ScriptModule* module = GetUsableModule();
		if (!module || !instance.Class)
			return false;

		StrataScriptInstance handle = nullptr;
		const ScriptCallResult result = module->CreateInstance(MakeCallSite(instance, "Create"), m_Context, &handle);
		if (result != ScriptCallResult::Ok)
		{
			if (result == ScriptCallResult::Exception)
			{
				Log::GetScriptLogger()->error("Script '{}' on entity {} threw an exception in its constructor: {}. The script is not created.", instance.ClassName,
					DescribeEntity(m_Scene, instance.Entity), module->GetLastExceptionMessage());
			}
			return false;
		}

		instance.Handle = handle;
		if (entry)
			ApplyFieldOverrides(instance, entry->Fields, true);
		return true;
	}

	void ScriptSystem::ApplyFieldOverrides(Instance& instance, const std::vector<ScriptFieldValue>& fields, bool reportProblems)
	{
		for (const ScriptFieldValue& field : fields)
		{
			ScriptModule* module = GetUsableModule();
			if (!module || !instance.Handle)
				return;

			const uint32_t index = instance.Class->FindFieldIndex(field.Name);
			if (index == UINT32_MAX)
			{
				if (reportProblems)
					ReportProblem("Script fields", fmt::format("script '{}' has no field '{}'; the stored value is ignored", instance.ClassName, field.Name));
				continue;
			}

			// Serialized enum values are integers (script fields have no enum type).
			const PropertyType storedType = field.Type == PropertyType::Enum ? PropertyType::Int : field.Type;
			const ScriptFieldInfo& info = instance.Class->Fields[index];
			if (storedType != info.Type || field.Value.index() != GetPropertyValueIndex(info.Type))
			{
				if (reportProblems)
				{
					ReportProblem("Script fields", fmt::format("field '{}.{}' is {}, but the stored value is {}; it is ignored", instance.ClassName, field.Name,
						PropertyTypeToString(info.Type), PropertyTypeToString(field.Type)));
				}
				continue;
			}

			const ScriptCallResult result = module->SetField(MakeCallSite(instance, "SetField"), instance.Handle, index, field.Value);
			if (result == ScriptCallResult::Exception)
			{
				Log::GetScriptLogger()->error("Script '{}' on entity {} threw an exception while setting field '{}': {}", instance.ClassName,
					DescribeEntity(m_Scene, instance.Entity), field.Name, module->GetLastExceptionMessage());
			}
		}
	}

	void ScriptSystem::DestroyInstance(Instance& instance, bool callOnDestroy)
	{
		if (instance.Removed && !instance.Handle)
			return;
		instance.Removed = true;
		if (!instance.Handle)
			return;

		// A faulted module is never called again: its instances are abandoned (their memory is leaked).
		if (ScriptModule* module = GetUsableModule())
		{
			if (callOnDestroy && instance.Created && !instance.Disabled && instance.Class->Implements(ScriptCallback::OnDestroy))
				Invoke(instance, ScriptCallback::OnDestroy, 0.0f);

			if (!module->IsFaulted() && instance.Handle)
			{
				const ScriptCallResult result = module->DestroyInstance(MakeCallSite(instance, "Destroy"), instance.Handle);
				if (result == ScriptCallResult::Exception)
				{
					Log::GetScriptLogger()->error("Script '{}' on entity {} threw an exception in its destructor: {}", instance.ClassName,
						DescribeEntity(m_Scene, instance.Entity), module->GetLastExceptionMessage());
				}
			}
		}
		instance.Handle = nullptr;
	}

	void ScriptSystem::DestroyAllInstances(bool callOnDestroy)
	{
		// Descendants before ancestors and the last script of an entity first: the reverse of the update order. Instances
		// of entities that left the scene without notice come first then; their entities are gone (no OnDestroy).
		std::vector<Ref<Instance>> order = CollectInstances();
		std::reverse(order.begin(), order.end());
		for (const Ref<Instance>& instance : order)
			DestroyInstance(*instance, callOnDestroy && m_Scene.GetEntityByUUID(instance->Entity).IsValid());

		m_Instances.clear();
		m_UpdateOrder.clear();
		m_PendingStart.clear();
		m_PendingReloads.clear();
	}

	std::vector<Ref<ScriptSystem::Instance>> ScriptSystem::CollectInstances()
	{
		RebuildUpdateOrder();
		std::vector<Ref<Instance>> instances = m_UpdateOrder;

		std::vector<UUID> orphaned;
		for (const auto& [entityID, entityInstances] : m_Instances)
		{
			if (!m_Scene.GetEntityByUUID(entityID))
				orphaned.push_back(entityID);
		}
		std::sort(orphaned.begin(), orphaned.end());
		for (UUID entityID : orphaned)
		{
			const std::vector<Ref<Instance>>& entityInstances = m_Instances[entityID];
			instances.insert(instances.end(), entityInstances.begin(), entityInstances.end());
		}
		return instances;
	}

	void ScriptSystem::DestroyRemovedInstances(std::vector<Ref<Instance>> order)
	{
		// OnDestroy may remove further scripts, also of instances this pass already went by: repeat until a pass finds
		// none. Every instance is destroyed at most once, so this ends.
		bool destroyedAny = true;
		while (destroyedAny)
		{
			destroyedAny = false;
			for (auto it = order.rbegin(); it != order.rend(); ++it)
			{
				Instance& instance = **it;
				if (!instance.Removed || !instance.Handle)
					continue;
				DestroyInstance(instance, m_Scene.GetEntityByUUID(instance.Entity).IsValid());
				destroyedAny = true;
			}
		}
		for (const Ref<Instance>& instance : order)
		{
			if (instance->Removed)
				RemoveDestroyedInstances(instance->Entity);
		}
	}

	void ScriptSystem::RemoveDestroyedInstances(UUID entity)
	{
		auto it = m_Instances.find(entity);
		if (it == m_Instances.end())
			return;
		std::vector<Ref<Instance>>& instances = it->second;
		instances.erase(std::remove_if(instances.begin(), instances.end(), &IsDestroyed), instances.end());
		if (instances.empty())
			m_Instances.erase(it);
	}

	void ScriptSystem::RebuildUpdateOrder()
	{
		m_UpdateOrder.clear();
		for (auto it = m_Instances.begin(); it != m_Instances.end();)
		{
			std::vector<Ref<Instance>>& instances = it->second;
			instances.erase(std::remove_if(instances.begin(), instances.end(), &IsDestroyed), instances.end());
			it = instances.empty() ? m_Instances.erase(it) : std::next(it);
		}
		if (m_Instances.empty())
			return;

		for (const Entity entity : m_Scene.GetEntitiesInHierarchyOrder())
		{
			auto it = m_Instances.find(entity.GetUUID());
			if (it != m_Instances.end())
				m_UpdateOrder.insert(m_UpdateOrder.end(), it->second.begin(), it->second.end());
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Module reloads
	////////////////////////////////////////////////////////////////////////////////

	void ScriptSystem::BeginModuleReload()
	{
		if (!m_Running)
			return;

		// Script code run by the destructors must not create instances in the module that is going away.
		m_CreationBlocked = true;
		// Scripts that were removed but not destroyed yet end in the old module (with OnDestroy); they are not carried over.
		DestroyRemovedInstances(CollectInstances());
		m_ReloadOrder = CollectInstances();
		m_UpdateOrder.clear();
		m_PendingStart.clear();

		for (const Ref<Instance>& instance : m_ReloadOrder)
		{
			instance->ReloadFields.clear();
			instance->Restore = false;

			// Removed by script code that ran during this loop (a destructor): it ends here as well.
			if (instance->Removed)
			{
				DestroyInstance(*instance, m_Scene.GetEntityByUUID(instance->Entity).IsValid());
				instance->Class = nullptr;
				continue;
			}

			// Without a usable module (it crashed) instances cannot be saved: they are abandoned and start over.
			ScriptModule* module = GetUsableModule();
			if (module && instance->Handle)
			{
				std::vector<ScriptFieldValue> snapshot;
				for (uint32_t index = 0; index < instance->Class->Fields.size(); index++)
				{
					const ScriptFieldInfo& field = instance->Class->Fields[index];
					PropertyValue value;
					if (module->GetField(MakeCallSite(*instance, "GetField"), instance->Handle, index, value) == ScriptCallResult::Ok)
						snapshot.push_back(ScriptFieldValue { field.Name, field.Type, std::move(value) });
					if (module->IsFaulted())
						break;
				}

				// Deleted without OnDestroy: the script continues in the new module.
				if (!module->IsFaulted() && module->DestroyInstance(MakeCallSite(*instance, "Destroy"), instance->Handle) != ScriptCallResult::Faulted)
				{
					instance->ReloadFields = std::move(snapshot);
					instance->Restore = true;
				}
			}
			instance->Handle = nullptr;
			instance->Class = nullptr;
		}
		m_PendingReloads.clear();
	}

	void ScriptSystem::EndModuleReload()
	{
		if (!m_Running)
			return;

		ScriptModule* module = GetUsableModule();
		const std::vector<Ref<Instance>> instances = std::move(m_ReloadOrder);
		m_ReloadOrder.clear();
		for (const Ref<Instance>& instance : instances)
		{
			if (instance->Removed)
				continue;

			const Entity entity = m_Scene.GetEntityByUUID(instance->Entity);
			const ScriptClassInfo* info = module ? module->FindClass(instance->ClassName) : nullptr;
			if (!entity || !info)
			{
				if (entity && module)
					ReportProblem("Hot reload", fmt::format("script '{}' no longer exists; it was removed from entity {}", instance->ClassName, DescribeEntity(m_Scene, instance->Entity)));
				instance->Removed = true;
				continue;
			}

			// The script may have been removed from the entity while the module was being replaced.
			const ScriptComponent* component = entity.TryGetComponent<ScriptComponent>();
			const ScriptEntry* entry = component ? component->FindScript(instance->ClassName) : nullptr;
			if (!entry)
			{
				instance->Removed = true;
				continue;
			}

			instance->Class = info;
			instance->Disabled = false;
			// The entry is copied: construction runs script code, which may change the component.
			const ScriptEntry entryCopy = *entry;
			if (!ConstructInstance(*instance, &entryCopy))
			{
				instance->Removed = true;
				if (!GetUsableModule())
					break;
				continue;
			}

			// The running values win over the stored overrides; fields that were removed or changed type keep their defaults.
			if (instance->Restore)
				ApplyFieldOverrides(*instance, instance->ReloadFields, false);
			instance->ReloadFields.clear();

			if (instance->Restore && instance->Created)
				m_PendingReloads.push_back(instance);
			else
			{
				// Never started, or its state was lost with a crashed module: it starts over (and knows of no contacts).
				instance->Created = false;
				instance->Contacts.clear();
				m_PendingStart.push_back(instance);
			}
			instance->Restore = false;
		}

		for (const Ref<Instance>& instance : instances)
		{
			if (instance->Removed)
				RemoveDestroyedInstances(instance->Entity);
		}
		RebuildUpdateOrder();
		m_CreationBlocked = false;
		// Scripts whose class was missing before may exist now, and scripts added meanwhile are still to be created.
		m_ReconcileAll = true;
	}

	void ScriptSystem::OnModuleLoaded()
	{
		if (m_Running)
			m_ReconcileAll = true;
	}

	void ScriptSystem::OnModuleUnloading()
	{
		if (!m_Running)
			return;
		m_CreationBlocked = true;
		DestroyAllInstances(true);
		m_CreationBlocked = false;
		m_ReconcileAll = true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Inspection
	////////////////////////////////////////////////////////////////////////////////

	size_t ScriptSystem::GetInstanceCount() const
	{
		size_t count = 0;
		for (const auto& [entityID, instances] : m_Instances)
		{
			for (const Ref<Instance>& instance : instances)
			{
				if (!instance->Removed && instance->Handle)
					count++;
			}
		}
		return count;
	}

	bool ScriptSystem::HasInstance(Entity entity, std::string_view className) const
	{
		if (!entity.IsValid())
			return false;
		const Ref<Instance> instance = FindInstance(entity.GetUUID(), className);
		return instance && instance->Handle;
	}

	std::optional<PropertyValue> ScriptSystem::GetFieldValue(Entity entity, std::string_view className, std::string_view fieldName) const
	{
		ScriptModule* module = GetUsableModule();
		const Ref<Instance> instance = entity.IsValid() ? FindInstance(entity.GetUUID(), className) : nullptr;
		if (!module || !instance || !instance->Handle)
			return std::nullopt;

		const uint32_t index = instance->Class->FindFieldIndex(fieldName);
		if (index == UINT32_MAX)
			return std::nullopt;

		PropertyValue value;
		if (module->GetField(MakeCallSite(*instance, "GetField"), instance->Handle, index, value) != ScriptCallResult::Ok)
			return std::nullopt;
		return value;
	}

	bool ScriptSystem::SetFieldValue(Entity entity, std::string_view className, std::string_view fieldName, const PropertyValue& value)
	{
		ScriptModule* module = GetUsableModule();
		const Ref<Instance> instance = entity.IsValid() ? FindInstance(entity.GetUUID(), className) : nullptr;
		if (!module || !instance || !instance->Handle)
			return false;

		const uint32_t index = instance->Class->FindFieldIndex(fieldName);
		if (index == UINT32_MAX || value.index() != GetPropertyValueIndex(instance->Class->Fields[index].Type))
			return false;
		return module->SetField(MakeCallSite(*instance, "SetField"), instance->Handle, index, value) == ScriptCallResult::Ok;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Script host API support
	////////////////////////////////////////////////////////////////////////////////

	void* ScriptSystem::GetInstanceHandle(Entity entity, std::string_view className) const
	{
		if (!entity.IsValid())
			return nullptr;
		const Ref<Instance> instance = FindInstance(entity.GetUUID(), className);
		return instance && !instance->Disabled ? instance->Handle : nullptr;
	}

	bool ScriptSystem::AddScript(Entity entity, std::string_view className, std::string* outError)
	{
		auto fail = [&](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};

		if (!entity.IsValid() || entity.GetScene() != &m_Scene)
			return fail("Invalid entity");
		if (!m_Running || m_CreationBlocked)
			return fail("Scripts can only be added while the scene is playing");
		ScriptModule* module = GetUsableModule();
		if (!module)
			return fail("No usable script module is loaded");
		if (!module->FindClass(className))
			return fail(fmt::format("The script module has no script '{}'", className));

		ScriptComponent& component = entity.HasComponent<ScriptComponent>() ? entity.GetComponent<ScriptComponent>() : entity.AddComponent<ScriptComponent>();
		if (!component.FindScript(className))
		{
			ScriptEntry& entry = component.Scripts.emplace_back();
			entry.ClassName = std::string(className);
			entity.MarkModified<ScriptComponent>();
		}

		CreatePendingInstances();
		if (!HasInstance(entity, className))
			return fail(fmt::format("Script '{}' could not be created", className));
		return true;
	}

	bool ScriptSystem::RemoveScript(Entity entity, std::string_view className)
	{
		ScriptComponent* component = entity.TryGetComponent<ScriptComponent>();
		if (!component)
			return false;

		auto it = std::find_if(component->Scripts.begin(), component->Scripts.end(), [&](const ScriptEntry& entry) { return entry.ClassName == className; });
		if (it == component->Scripts.end())
			return false;

		component->Scripts.erase(it);
		entity.MarkModified<ScriptComponent>();
		// No more callbacks from now on; OnDestroy and the deletion follow at the next sync point.
		if (const Ref<Instance> instance = FindInstance(entity.GetUUID(), className))
			instance->Removed = true;
		return true;
	}

	void ScriptSystem::DestroyEntity(Entity entity)
	{
		if (!entity.IsValid())
			return;

		// While the scene updates it defers destruction itself. Otherwise (script code run during a module reload) this
		// waits for the next sync point, so script code is never destroyed while it runs.
		if (m_Scene.IsUpdating())
			m_Scene.DestroyEntity(entity);
		else
			m_DeferredDestroys.push_back(entity.GetUUID());
	}

	void ScriptSystem::CreatePendingInstances()
	{
		if (m_Running && !m_CreationBlocked)
			ReconcileDirtyEntities(false);
	}

	bool ScriptSystem::RequestAsset(AssetManagerBase& manager, AssetHandle asset)
	{
		auto it = m_AssetPins.find(asset);
		if (it != m_AssetPins.end())
		{
			// Requested before: the scene holds it already. It may have been unloaded on purpose meanwhile; load it again.
			manager.RequestLoad(asset, AssetPriority::Normal);
			return true;
		}
		// Scripts request assets ahead of their use (e.g. in OnCreate): not more urgent than what is on screen.
		AssetPin pin = manager.Pin(asset, AssetPriority::Normal);
		if (!pin.IsValid())
			return false;
		m_AssetPins.emplace(asset, std::move(pin));
		return true;
	}

	bool ScriptSystem::ReleaseAsset(AssetHandle asset)
	{
		return m_AssetPins.erase(asset) > 0;
	}

	void ScriptSystem::ReportProblem(std::string_view function, const std::string& message)
	{
		std::string text;
		const ScriptCallSite* site = ScriptModule::GetCurrentCall();
		if (site && site->Class)
			text = fmt::format("{} (called by script '{}' on entity {}): {}", function, site->Class->Name, DescribeEntity(m_Scene, site->Entity), message);
		else
			text = fmt::format("{}: {}", function, message);

		if (m_ReportedProblems.size() >= c_MaxReportedProblems)
			return;
		if (!m_ReportedProblems.insert(text).second)
			return;

		Log::GetScriptLogger()->warn("{}", text);
		if (m_ReportedProblems.size() == c_MaxReportedProblems)
			Log::GetScriptLogger()->warn("Scene '{}': further script problems are not reported", m_Scene.GetName());
	}

}
