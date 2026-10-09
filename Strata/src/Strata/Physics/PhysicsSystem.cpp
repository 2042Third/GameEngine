#include "stpch.h"
#include "Strata/Physics/PhysicsSystem.h"

#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

namespace Strata
{

	namespace
	{

		PhysicsSettings& GetDefaultSettingsStorage()
		{
			static PhysicsSettings s_DefaultSettings;
			return s_DefaultSettings;
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
	}

	PhysicsSystem::~PhysicsSystem()
	{
		OnRuntimeStop();
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
	}

	void PhysicsSystem::OnRuntimeStop()
	{
		m_CreationConnections.clear();
		m_WorldRequested = false;
		m_Running = false;
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
		m_World->Simulate(timestep);
		DispatchCollisionEvents();
	}

	CollisionListenerID PhysicsSystem::AddCollisionListener(CollisionCallback callback)
	{
		if (!callback)
			return c_InvalidCollisionListener;

		Ref<CollisionListener> listener = CreateRef<CollisionListener>();
		listener->ID = m_NextListenerID++;
		listener->Callback = std::move(callback);
		m_CollisionListeners.push_back(listener);
		return listener->ID;
	}

	bool PhysicsSystem::RemoveCollisionListener(CollisionListenerID id)
	{
		auto it = std::find_if(m_CollisionListeners.begin(), m_CollisionListeners.end(), [id](const Ref<CollisionListener>& listener) { return listener->ID == id; });
		if (it == m_CollisionListeners.end())
			return false;

		// A dispatch in progress holds its own references; the flag stops it from calling this listener again.
		(*it)->Removed = true;
		m_CollisionListeners.erase(it);
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
		if (m_World)
			m_World->SetLayersCollide(layerA, layerB, collide); // Validates and warns
		m_Settings.SetLayersCollide(layerA, layerB, collide);
	}

	bool PhysicsSystem::DoLayersCollide(uint32_t layerA, uint32_t layerB) const
	{
		return m_Settings.DoLayersCollide(layerA, layerB);
	}

	PhysicsStats PhysicsSystem::GetStats() const
	{
		return m_World ? m_World->GetStats() : PhysicsStats();
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

	bool PhysicsSystem::SceneHasPhysicsComponents() const
	{
		const entt::registry& registry = m_Scene.GetRegistry();
		return HasAny<RigidBodyComponent>(registry) || HasAny<BoxColliderComponent>(registry) || HasAny<SphereColliderComponent>(registry)
			|| HasAny<CapsuleColliderComponent>(registry) || HasAny<MeshColliderComponent>(registry);
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
		m_World = CreateScope<PhysicsWorld>(m_Scene, m_Settings);
		m_AppliedGravity = m_Scene.GetSettings().Gravity;
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
			for (const Ref<CollisionListener>& listener : listeners)
			{
				if (!listener->Removed)
					listener->Callback(event);
			}
		}
	}

}
