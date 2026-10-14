#include "stpch.h"
#include "Strata/Physics/PhysicsSystem.h"

#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace Strata
{

	namespace
	{

		PhysicsSettings& GetDefaultSettingsStorage()
		{
			static PhysicsSettings s_DefaultSettings;
			return s_DefaultSettings;
		}

		// What the worlds of each scene (by Scene::GetLineageID) needed beyond their capacities, in this process.
		std::unordered_map<UUID, PhysicsCapacityDemand>& GetCapacityDemandStorage()
		{
			static std::unordered_map<UUID, PhysicsCapacityDemand> s_Demands;
			return s_Demands;
		}

		// Bitwise comparison, so that a non-finite gravity is not re-applied (and re-rejected) every step.
		bool IsSameVector(const glm::vec3& a, const glm::vec3& b)
		{
			return std::memcmp(&a, &b, sizeof(glm::vec3)) == 0;
		}

		template<typename Component>
		bool HasAny(const entt::registry& registry)
		{
			const auto* storage = registry.storage<Component>();
			return storage && !storage->empty();
		}

	}

	PhysicsSystem::PhysicsSystem(Scene& scene)
		: m_Scene(scene), m_Settings(GetDefaultSettings())
	{
		m_Scene.RegisterService<ICollisionEventSource>(*this);
		m_Scene.RegisterService<IBodyMotionQuery>(*this);
		m_Scene.RegisterService<IBodyControl>(*this);
		m_Scene.RegisterService<IPhysicsQueries>(*this);
		m_Scene.RegisterService<ICharacterControl>(*this);
		m_Scene.RegisterService<IPhysicsCapacity>(*this);
	}

	PhysicsSystem::~PhysicsSystem()
	{
		OnRuntimeStop();
		m_Scene.UnregisterService<IPhysicsCapacity>(*this);
		m_Scene.UnregisterService<ICharacterControl>(*this);
		m_Scene.UnregisterService<IPhysicsQueries>(*this);
		m_Scene.UnregisterService<IBodyControl>(*this);
		m_Scene.UnregisterService<IBodyMotionQuery>(*this);
		m_Scene.UnregisterService<ICollisionEventSource>(*this);
	}

	void PhysicsSystem::SetDefaultSettings(const PhysicsSettings& settings)
	{
		GetDefaultSettingsStorage() = settings;
	}

	const PhysicsSettings& PhysicsSystem::GetDefaultSettings()
	{
		return GetDefaultSettingsStorage();
	}

	void PhysicsSystem::OnRuntimeStart()
	{
		ST_PROFILE_FUNCTION();

		if (m_Running)
			return;

		m_Running = true;
		if (SceneHasPhysicsComponents())
		{
			CreateWorld();
			return;
		}

		// No physics yet: create the world (and allocate Jolt's per-world buffers) only once a physics component appears.
		entt::registry& registry = m_Scene.GetRegistry();
		m_CreationConnections.emplace_back(registry.on_construct<RigidBodyComponent>().connect<&PhysicsSystem::OnPhysicsComponentAdded>(*this));
		m_CreationConnections.emplace_back(registry.on_construct<BoxColliderComponent>().connect<&PhysicsSystem::OnPhysicsComponentAdded>(*this));
		m_CreationConnections.emplace_back(registry.on_construct<SphereColliderComponent>().connect<&PhysicsSystem::OnPhysicsComponentAdded>(*this));
		m_CreationConnections.emplace_back(registry.on_construct<CapsuleColliderComponent>().connect<&PhysicsSystem::OnPhysicsComponentAdded>(*this));
		m_CreationConnections.emplace_back(registry.on_construct<MeshColliderComponent>().connect<&PhysicsSystem::OnPhysicsComponentAdded>(*this));
		m_CreationConnections.emplace_back(registry.on_construct<CharacterControllerComponent>().connect<&PhysicsSystem::OnPhysicsComponentAdded>(*this));
	}

	PhysicsCapacityDemand PhysicsSystem::GetRememberedCapacityDemand(UUID sceneLineage)
	{
		const auto& demands = GetCapacityDemandStorage();
		auto it = demands.find(sceneLineage);
		return it != demands.end() ? it->second : PhysicsCapacityDemand();
	}

	void PhysicsSystem::ForgetCapacityDemands()
	{
		GetCapacityDemandStorage().clear();
	}

	void PhysicsSystem::OnRuntimeStop()
	{
		m_CreationConnections.clear();
		m_WorldRequested = false;
		m_Running = false;
		m_ReservedBodies = 0;
		if (m_World)
		{
			// What this world lacked, the scene's next world gets.
			const PhysicsCapacityDemand demand = m_World->GetCapacityDemand();
			if (!demand.IsEmpty())
			{
				PhysicsCapacityDemand& remembered = GetCapacityDemandStorage()[m_Scene.GetLineageID()];
				remembered.Bodies = std::max(remembered.Bodies, demand.Bodies);
				remembered.BodyPairs = std::max(remembered.BodyPairs, demand.BodyPairs);
				remembered.ContactConstraints = std::max(remembered.ContactConstraints, demand.ContactConstraints);
			}
		}
		m_World.reset(); // Bodies are destroyed without collision events: the whole world goes away
	}

	void PhysicsSystem::OnUpdate(Timestep)
	{
		ApplyPendingChanges();
		DispatchCollisionEvents();
	}

	void PhysicsSystem::OnFixedUpdate(float timestep)
	{
		ST_PROFILE_FUNCTION();

		ApplyPendingChanges();
		if (!m_World)
			return;

		SyncGravity();
		SyncLayers();
		m_World->Simulate(timestep);
		DispatchCollisionEvents();
	}

	PhysicsCapacityInfo PhysicsSystem::GetCapacityInfo() const
	{
		return m_World ? m_World->GetCapacityInfo() : PhysicsCapacityInfo();
	}

	void PhysicsSystem::Reserve(uint32_t bodies)
	{
		m_ReservedBodies = bodies;
		if (m_World)
			m_World->Reserve(bodies);
	}

	CollisionListenerID PhysicsSystem::AddCollisionListener(CollisionCallback callback, CollisionListenerScope scope)
	{
		if (!callback)
			return c_InvalidCollisionListener;

		Ref<CollisionListener> listener = CreateRef<CollisionListener>();
		listener->ID = m_NextListenerID++;
		listener->Callback = std::move(callback);
		listener->Scope = scope;
		m_CollisionListeners.push_back(listener);
		UpdateContactTracking();
		return listener->ID;
	}

	void PhysicsSystem::WatchCollisions(UUID entity)
	{
		if (m_CollisionWatches[entity]++ == 0 && m_World)
			m_World->SetContactInterest(entity, true);
	}

	void PhysicsSystem::UnwatchCollisions(UUID entity)
	{
		auto it = m_CollisionWatches.find(entity);
		if (it == m_CollisionWatches.end())
			return;
		if (--it->second > 0)
			return;
		m_CollisionWatches.erase(it);
		if (m_World)
			m_World->SetContactInterest(entity, false);
	}

	void PhysicsSystem::UpdateContactTracking()
	{
		if (!m_World)
			return;
		const bool trackAll = std::any_of(m_CollisionListeners.begin(), m_CollisionListeners.end(),
			[](const Ref<CollisionListener>& listener) { return listener->Scope == CollisionListenerScope::AllEntities; });
		m_World->SetTrackAllContacts(trackAll);
	}

	bool PhysicsSystem::IsWatchingCollisions(UUID entity) const
	{
		return m_CollisionWatches.find(entity) != m_CollisionWatches.end();
	}

	bool PhysicsSystem::RemoveCollisionListener(CollisionListenerID id)
	{
		auto it = std::find_if(m_CollisionListeners.begin(), m_CollisionListeners.end(), [id](const Ref<CollisionListener>& listener) { return listener->ID == id; });
		if (it == m_CollisionListeners.end())
			return false;

		// A dispatch in progress holds its own references; the flag stops it from calling this listener again.
		(*it)->Removed = true;
		m_CollisionListeners.erase(it);
		UpdateContactTracking();
		return true;
	}

	bool PhysicsSystem::SetGravity(const glm::vec3& gravity)
	{
		if (!std::isfinite(gravity.x) || !std::isfinite(gravity.y) || !std::isfinite(gravity.z))
			return false;

		m_Scene.GetSettings().Gravity = gravity;
		m_AppliedGravity = gravity;
		if (m_World)
			m_World->SetGravity(gravity);
		return true;
	}

	glm::vec3 PhysicsSystem::GetGravity() const
	{
		return m_World ? m_World->GetGravity() : m_Scene.GetSettings().Gravity;
	}

	void PhysicsSystem::SetLayersCollide(uint32_t layerA, uint32_t layerB, bool collide)
	{
		if (layerA >= c_PhysicsLayerCount || layerB >= c_PhysicsLayerCount)
		{
			ST_CORE_WARN("Physics: cannot change the collision of layers {} and {}: layers range from 0 to {}", layerA, layerB, c_PhysicsLayerCount - 1);
			return;
		}
		m_Scene.GetSettings().PhysicsLayers.SetCollide(layerA, layerB, collide);
		if (m_World)
			SyncLayers();
	}

	bool PhysicsSystem::DoLayersCollide(uint32_t layerA, uint32_t layerB) const
	{
		return m_Settings.DoLayersCollide(layerA, layerB) && m_Scene.GetSettings().PhysicsLayers.DoCollide(layerA, layerB);
	}

	std::array<uint32_t, c_PhysicsLayerCount> PhysicsSystem::GetEffectiveLayerMasks() const
	{
		std::array<uint32_t, c_PhysicsLayerCount> masks;
		const PhysicsLayerSettings& layers = m_Scene.GetSettings().PhysicsLayers;
		for (uint32_t layer = 0; layer < c_PhysicsLayerCount; layer++)
			masks[layer] = m_Settings.LayerCollisionMasks[layer] & layers.CollisionMasks[layer];
		return masks;
	}

	void PhysicsSystem::SyncLayers()
	{
		const std::array<uint32_t, c_PhysicsLayerCount> masks = GetEffectiveLayerMasks();
		if (masks == m_AppliedLayerMasks)
			return;
		m_AppliedLayerMasks = masks;
		m_World->SetLayerCollisionMasks(masks);
	}

	PhysicsStats PhysicsSystem::GetStats() const
	{
		PhysicsStats stats = m_World ? m_World->GetStats() : PhysicsStats();
		stats.DroppedSteps = m_Scene.GetDroppedFixedStepCount();
		return stats;
	}

	bool PhysicsSystem::HasBody(Entity entity)
	{
		ApplyPendingChanges();
		return m_World && m_World->HasBody(entity);
	}

	Entity PhysicsSystem::GetBodyEntity(Entity entity)
	{
		ApplyPendingChanges();
		return m_World ? m_World->GetBodyEntity(entity) : Entity();
	}

	glm::vec3 PhysicsSystem::GetLinearVelocity(Entity entity)
	{
		ApplyPendingChanges();
		return m_World ? m_World->GetLinearVelocity(entity) : glm::vec3(0.0f);
	}

	bool PhysicsSystem::SetLinearVelocity(Entity entity, const glm::vec3& velocity)
	{
		ApplyPendingChanges();
		return m_World && m_World->SetLinearVelocity(entity, velocity);
	}

	glm::vec3 PhysicsSystem::GetAngularVelocity(Entity entity)
	{
		ApplyPendingChanges();
		return m_World ? m_World->GetAngularVelocity(entity) : glm::vec3(0.0f);
	}

	bool PhysicsSystem::SetAngularVelocity(Entity entity, const glm::vec3& velocity)
	{
		ApplyPendingChanges();
		return m_World && m_World->SetAngularVelocity(entity, velocity);
	}

	std::optional<glm::vec3> PhysicsSystem::GetPointVelocity(Entity entity, const glm::vec3& worldPoint)
	{
		ApplyPendingChanges();
		if (!m_World || !m_World->HasBody(entity))
			return std::nullopt;
		const glm::vec3 origin = glm::vec3(m_Scene.GetWorldTransform(entity)[3]);
		return m_World->GetLinearVelocity(entity) + glm::cross(m_World->GetAngularVelocity(entity), worldPoint - origin);
	}

	bool PhysicsSystem::AddForce(Entity entity, const glm::vec3& force)
	{
		ApplyPendingChanges();
		return m_World && m_World->AddForce(entity, force);
	}

	bool PhysicsSystem::AddForceAtPosition(Entity entity, const glm::vec3& force, const glm::vec3& worldPosition)
	{
		ApplyPendingChanges();
		return m_World && m_World->AddForceAtPosition(entity, force, worldPosition);
	}

	bool PhysicsSystem::AddImpulse(Entity entity, const glm::vec3& impulse)
	{
		ApplyPendingChanges();
		return m_World && m_World->AddImpulse(entity, impulse);
	}

	bool PhysicsSystem::AddImpulseAtPosition(Entity entity, const glm::vec3& impulse, const glm::vec3& worldPosition)
	{
		ApplyPendingChanges();
		return m_World && m_World->AddImpulseAtPosition(entity, impulse, worldPosition);
	}

	bool PhysicsSystem::AddTorque(Entity entity, const glm::vec3& torque)
	{
		ApplyPendingChanges();
		return m_World && m_World->AddTorque(entity, torque);
	}

	bool PhysicsSystem::AddAngularImpulse(Entity entity, const glm::vec3& impulse)
	{
		ApplyPendingChanges();
		return m_World && m_World->AddAngularImpulse(entity, impulse);
	}

	bool PhysicsSystem::SetGravityScale(Entity entity, float gravityScale)
	{
		ApplyPendingChanges();
		return m_World && m_World->SetGravityScale(entity, gravityScale);
	}

	bool PhysicsSystem::IsSleeping(Entity entity)
	{
		ApplyPendingChanges();
		return m_World && m_World->IsSleeping(entity);
	}

	bool PhysicsSystem::WakeUp(Entity entity)
	{
		ApplyPendingChanges();
		return m_World && m_World->WakeUp(entity);
	}

	bool PhysicsSystem::Teleport(Entity entity, const glm::vec3& position, const glm::quat& rotation)
	{
		ApplyPendingChanges();
		return m_World && m_World->Teleport(entity, position, rotation);
	}

	std::optional<RaycastHit> PhysicsSystem::Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask, Entity ignoreEntity, bool includeTriggers)
	{
		ApplyPendingChanges();
		if (!m_World)
			return std::nullopt;
		return m_World->Raycast(origin, direction, maxDistance, layerMask, ignoreEntity, includeTriggers);
	}

	std::vector<RaycastHit> PhysicsSystem::RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask, Entity ignoreEntity, bool includeTriggers)
	{
		ApplyPendingChanges();
		if (!m_World)
			return {};
		return m_World->RaycastAll(origin, direction, maxDistance, layerMask, ignoreEntity, includeTriggers);
	}

	std::vector<Entity> PhysicsSystem::OverlapSphere(const glm::vec3& center, float radius, uint32_t layerMask, bool includeTriggers)
	{
		ApplyPendingChanges();
		if (!m_World)
			return {};
		return m_World->OverlapSphere(center, radius, layerMask, includeTriggers);
	}

	std::vector<Entity> PhysicsSystem::OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, uint32_t layerMask, bool includeTriggers)
	{
		ApplyPendingChanges();
		if (!m_World)
			return {};
		return m_World->OverlapBox(center, halfExtents, rotation, layerMask, includeTriggers);
	}

	bool PhysicsSystem::HasCharacterController(Entity entity)
	{
		ApplyPendingChanges();
		return m_World && m_World->HasCharacterController(entity);
	}

	bool PhysicsSystem::MoveCharacter(Entity entity, const glm::vec3& velocity)
	{
		ApplyPendingChanges();
		return m_World && m_World->MoveCharacter(entity, velocity);
	}

	bool PhysicsSystem::JumpCharacter(Entity entity, float speed)
	{
		ApplyPendingChanges();
		return m_World && m_World->JumpCharacter(entity, speed);
	}

	bool PhysicsSystem::TeleportCharacter(Entity entity, const glm::vec3& position)
	{
		ApplyPendingChanges();
		return m_World && m_World->TeleportCharacter(entity, position);
	}

	bool PhysicsSystem::AddCharacterMotion(Entity entity, const glm::vec3& displacement)
	{
		ApplyPendingChanges();
		return m_World && m_World->AddCharacterMotion(entity, displacement);
	}

	std::optional<CharacterState> PhysicsSystem::GetCharacterState(Entity entity)
	{
		ApplyPendingChanges();
		return m_World ? m_World->GetCharacterState(entity) : std::nullopt;
	}

	bool PhysicsSystem::SceneHasPhysicsComponents() const
	{
		const entt::registry& registry = m_Scene.GetRegistry();
		return HasAny<RigidBodyComponent>(registry) || HasAny<BoxColliderComponent>(registry) || HasAny<SphereColliderComponent>(registry)
			|| HasAny<CapsuleColliderComponent>(registry) || HasAny<MeshColliderComponent>(registry) || HasAny<CharacterControllerComponent>(registry);
	}

	void PhysicsSystem::OnPhysicsComponentAdded(entt::registry&, entt::entity)
	{
		// Created later: the component is usually filled in right after it is added.
		m_WorldRequested = true;
	}

	void PhysicsSystem::CreateWorld()
	{
		m_CreationConnections.clear();
		m_WorldRequested = false;
		// The scene's capacities win over the defaults; those left automatic are sized by the world, for at least what an
		// earlier world of this scene needed.
		PhysicsSettings settings = m_Settings;
		const PhysicsCapacitySettings& capacity = m_Scene.GetSettings().PhysicsCapacity;
		if (capacity.MaxBodies != 0)
			settings.MaxBodies = capacity.MaxBodies;
		if (capacity.MaxBodyPairs != 0)
			settings.MaxBodyPairs = capacity.MaxBodyPairs;
		if (capacity.MaxContactConstraints != 0)
			settings.MaxContactConstraints = capacity.MaxContactConstraints;
		// The scene's layer matrix, as the scene's settings say when the world starts.
		settings.LayerCollisionMasks = GetEffectiveLayerMasks();
		m_World = CreateScope<PhysicsWorld>(m_Scene, settings, GetRememberedCapacityDemand(m_Scene.GetLineageID()));
		m_AppliedGravity = m_Scene.GetSettings().Gravity;
		m_AppliedLayerMasks = settings.LayerCollisionMasks;
		if (m_ReservedBodies > 0)
			m_World->Reserve(m_ReservedBodies);
		// Contacts are tracked for the listeners and watches registered so far.
		UpdateContactTracking();
		for (const auto& [entity, count] : m_CollisionWatches)
			m_World->SetContactInterest(entity, true);
	}

	void PhysicsSystem::ApplyPendingChanges()
	{
		if (!m_Running)
			return;

		if (!m_World)
		{
			// The components may have been removed again before the world was needed.
			if (m_WorldRequested && SceneHasPhysicsComponents())
				CreateWorld();
			return;
		}
		m_World->ApplyPendingChanges();
	}

	void PhysicsSystem::SyncGravity()
	{
		const glm::vec3 gravity = m_Scene.GetSettings().Gravity;
		if (IsSameVector(gravity, m_AppliedGravity))
			return;

		m_AppliedGravity = gravity;
		if (!m_World->SetGravity(gravity))
			ST_CORE_WARN("Physics: scene '{}' has a non-finite gravity {}; the previous gravity is kept", m_Scene.GetName(), gravity);
	}

	void PhysicsSystem::DispatchCollisionEvents()
	{
		if (!m_World)
			return;

		const std::vector<CollisionEvent> events = m_World->TakeCollisionEvents();
		if (events.empty() || m_CollisionListeners.empty())
			return;

		ST_PROFILE_FUNCTION();

		// Listeners may add or remove listeners while being called; iterate over a snapshot.
		const std::vector<Ref<CollisionListener>> listeners = m_CollisionListeners;
		for (const CollisionEvent& event : events)
		{
			// Watches may change during the dispatch too; each event is judged when its turn comes.
			const bool watched = IsWatchingCollisions(event.AID) || IsWatchingCollisions(event.BID);
			for (const Ref<CollisionListener>& listener : listeners)
			{
				if (listener->Removed || (listener->Scope == CollisionListenerScope::WatchedEntities && !watched))
					continue;
				listener->Callback(event);
			}
		}
	}

}
