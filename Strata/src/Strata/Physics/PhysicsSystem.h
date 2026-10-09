#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Physics/PhysicsTypes.h"
#include "Strata/Physics/PhysicsWorld.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneSystem.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <unordered_set>
#include <vector>

namespace Strata
{

	class Scene;

	// Rigid body physics for a running scene (Play and Simulate modes).
	//
	// On runtime start every active entity with colliders gets a body (see PhysicsWorld for how components map to bodies),
	// and the scene's gravity and fixed timestep drive the simulation: each fixed update steps the world once. Adding,
	// removing or modifying RigidBody/collider components (EnTT on_construct/on_update/on_destroy; property edits must
	// emit on_update, e.g. through Entity::MarkModified or ComponentAccess), destroying entities and (de)activating them
	// are picked up automatically before the next update, body API call or query. Transforms changed outside physics are
	// applied to the bodies at the next fixed step, so queries made in between see the bodies where they were after the last
	// step (Teleport moves a body immediately).
	//
	// Collision events are collected during each step and dispatched afterwards on the main thread to the registered
	// listeners. Listeners may use the whole scene and physics API, including destroying entities; End events caused by
	// changes made outside a step are dispatched after the next update.
	//
	// Main thread only. Functions return false / zero / empty results while the system is not running.
	class PhysicsSystem : public SceneSystem
	{
	public:
		explicit PhysicsSystem(Scene& scene);
		~PhysicsSystem() override;

		PhysicsSystem(const PhysicsSystem&) = delete;
		PhysicsSystem& operator=(const PhysicsSystem&) = delete;

		// Project-wide settings (capacities, layer matrix) used by physics systems started afterwards.
		static void SetDefaultSettings(const PhysicsSettings& settings);
		static const PhysicsSettings& GetDefaultSettings();

		void OnRuntimeStart() override;
		void OnRuntimeStop() override;
		void OnUpdate(Timestep timestep) override;
		void OnFixedUpdate(float timestep) override;

		bool IsRunning() const { return m_World != nullptr; }
		// The simulation backing this system; nullptr while not running.
		PhysicsWorld* GetWorld() { return m_World.get(); }
		const PhysicsWorld* GetWorld() const { return m_World.get(); }

		//////////////////////////////////////////////////////////////////////////
		// Collision events
		//////////////////////////////////////////////////////////////////////////

		// Listeners are called in registration order. A listener added while events are being dispatched receives events
		// from the next dispatch on; a removed listener is not called again, even for the rest of the current dispatch.
		CollisionListenerID AddCollisionListener(CollisionCallback callback);
		bool RemoveCollisionListener(CollisionListenerID id);

		//////////////////////////////////////////////////////////////////////////
		// World
		//////////////////////////////////////////////////////////////////////////

		// Sets the scene's gravity (SceneSettings::Gravity) and applies it immediately. Non-finite values are rejected.
		bool SetGravity(const glm::vec3& gravity);
		glm::vec3 GetGravity() const;
		void SetLayersCollide(uint32_t layerA, uint32_t layerB, bool collide);
		bool DoLayersCollide(uint32_t layerA, uint32_t layerB) const;
		PhysicsStats GetStats() const;

		//////////////////////////////////////////////////////////////////////////
		// Body API (see PhysicsWorld)
		//////////////////////////////////////////////////////////////////////////

		bool HasBody(Entity entity);
		glm::vec3 GetLinearVelocity(Entity entity);
		bool SetLinearVelocity(Entity entity, const glm::vec3& velocity);
		glm::vec3 GetAngularVelocity(Entity entity);
		bool SetAngularVelocity(Entity entity, const glm::vec3& velocity);
		bool AddForce(Entity entity, const glm::vec3& force);
		bool AddForceAtPosition(Entity entity, const glm::vec3& force, const glm::vec3& worldPosition);
		bool AddImpulse(Entity entity, const glm::vec3& impulse);
		bool AddImpulseAtPosition(Entity entity, const glm::vec3& impulse, const glm::vec3& worldPosition);
		bool AddTorque(Entity entity, const glm::vec3& torque);
		bool SetGravityScale(Entity entity, float gravityScale);
		bool IsSleeping(Entity entity);
		bool WakeUp(Entity entity);
		bool Teleport(Entity entity, const glm::vec3& position, const glm::quat& rotation);

		//////////////////////////////////////////////////////////////////////////
		// Queries (see PhysicsWorld)
		//////////////////////////////////////////////////////////////////////////

		std::optional<RaycastHit> Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask = c_AllPhysicsLayers, Entity ignoreEntity = {}, bool includeTriggers = false);
		std::vector<RaycastHit> RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask = c_AllPhysicsLayers, Entity ignoreEntity = {}, bool includeTriggers = false);
		std::vector<Entity> OverlapSphere(const glm::vec3& center, float radius, uint32_t layerMask = c_AllPhysicsLayers, bool includeTriggers = false);
		std::vector<Entity> OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, uint32_t layerMask = c_AllPhysicsLayers, bool includeTriggers = false);
	private:
		struct CollisionListener
		{
			CollisionListenerID ID = c_InvalidCollisionListener;
			CollisionCallback Callback;
			bool Removed = false;
		};

		void ConnectSignals();
		void OnBodyComponentChanged(entt::registry& registry, entt::entity handle);
		void OnMeshRendererChanged(entt::registry& registry, entt::entity handle);
		void OnActivityChanged(entt::registry& registry, entt::entity handle);
		void MarkActivityChanged(entt::registry& registry, entt::entity handle);

		// Applies component, destruction and activity changes recorded by the signals since the last call.
		void ApplyPendingChanges();
		void SyncGravity();
		void DispatchCollisionEvents();
	private:
		Scene& m_Scene;
		Scope<PhysicsWorld> m_World;
		std::vector<entt::scoped_connection> m_Connections;

		std::vector<entt::entity> m_PendingRebuilds; // In signal order, so that changes are applied deterministically
		std::unordered_set<entt::entity> m_PendingRebuildSet;
		std::vector<entt::entity> m_PendingActivity;
		std::unordered_set<entt::entity> m_PendingActivitySet;

		std::vector<Ref<CollisionListener>> m_CollisionListeners;
		CollisionListenerID m_NextListenerID = 1;
		glm::vec3 m_AppliedGravity = glm::vec3(0.0f); // Scene gravity last applied to the world
	};

}
