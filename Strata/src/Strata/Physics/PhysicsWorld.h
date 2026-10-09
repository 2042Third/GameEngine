#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Physics/PhysicsTypes.h"
#include "Strata/Scene/Entity.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <vector>

namespace Strata
{

	class Scene;
	struct PhysicsWorldData;

	// One rigid body simulation (a Jolt physics system) bound to a scene.
	//
	// Bodies are built from entity components: entities with collider components get a body whose type comes from their
	// RigidBodyComponent (static when there is none). Several colliders on one entity form a compound shape. The entity's
	// world transform places the body and its world scale is baked into the shape:
	//  - box colliders scale exactly, including non-uniform scale;
	//  - sphere radii scale by the largest absolute axis scale;
	//  - capsule radii scale by the larger of |X| and |Z|, their half height by |Y|;
	//  - mesh colliders scale exactly (non-uniform and mirrored);
	//  - collider offsets are scaled with the entity, but shear (non-uniform scale under a rotated parent) is not
	//    representable and is approximated by the decomposed scale.
	// Lock rotation flags refer to world axes.
	//
	// Simulate() keeps bodies in sync with the scene each fixed step: kinematic bodies are moved towards their entity's
	// transform, static bodies and dynamic bodies whose transform was changed from outside physics are teleported, bodies of
	// entities that become inactive in the hierarchy leave the simulation (and return when reactivated), and dynamic bodies
	// write their simulated pose back to their entities. Component edits are applied through RefreshBody (PhysicsSystem
	// calls it from EnTT signals).
	//
	// Main thread only. Jolt runs the step on Strata's JobSystem workers when it is initialized.
	class PhysicsWorld
	{
	public:
		explicit PhysicsWorld(Scene& scene, const PhysicsSettings& settings = PhysicsSettings());
		~PhysicsWorld(); // Destroys every body without emitting collision events

		PhysicsWorld(const PhysicsWorld&) = delete;
		PhysicsWorld& operator=(const PhysicsWorld&) = delete;

		// Source of triangle data for MeshColliderComponent (process-wide, main thread). Without a provider, or when it
		// returns nullptr for an asset, the mesh collider is skipped with a warning; mark the collider modified to
		// rebuild the body once the data is available.
		static void SetMeshProvider(PhysicsMeshProvider provider);
		static bool HasMeshProvider();

		Scene& GetScene() const;
		const PhysicsSettings& GetSettings() const;

		//////////////////////////////////////////////////////////////////////////
		// Bodies
		//////////////////////////////////////////////////////////////////////////

		// Creates the bodies of every entity in the scene, in hierarchy order.
		void CreateAllBodies();
		// Rebuilds the entity's body from its current components and world transform, or removes it if the entity no
		// longer needs one (destroyed, no collider, degenerate transform). Velocities and ongoing contacts carry over.
		// Returns true if the entity has a body afterwards (it may be outside the simulation while inactive).
		bool RefreshBody(entt::entity handle);
		// Adds the entity's body to, or removes it from, the simulation to match its activity in the hierarchy.
		void RefreshActivity(entt::entity handle);
		// Removes the entity's body. Contacts it had end (End events are queued).
		void DestroyBody(entt::entity handle);
		// True if the entity has a body that is part of the simulation (i.e. not deactivated).
		bool HasBody(Entity entity) const;

		// Synchronizes entity transforms and activity to the bodies, advances the simulation by `timestep` seconds, writes
		// dynamic bodies back to their entities and updates the contact state.
		void Simulate(float timestep);

		// Collision events queued since the last call, in the order they occurred. Entity handles are resolved now. Events
		// accumulate until taken (PhysicsSystem takes them after every update).
		std::vector<CollisionEvent> TakeCollisionEvents();

		//////////////////////////////////////////////////////////////////////////
		// World settings
		//////////////////////////////////////////////////////////////////////////

		// Non-finite values are rejected. Changing gravity wakes every dynamic body.
		bool SetGravity(const glm::vec3& gravity);
		glm::vec3 GetGravity() const;

		// Changes the layer collision matrix at runtime (symmetric) and wakes the bodies so that they notice.
		void SetLayersCollide(uint32_t layerA, uint32_t layerB, bool collide);
		bool DoLayersCollide(uint32_t layerA, uint32_t layerB) const;

		//////////////////////////////////////////////////////////////////////////
		// Body API (entities without a body in the simulation get false / zero)
		//////////////////////////////////////////////////////////////////////////

		glm::vec3 GetLinearVelocity(Entity entity) const;
		bool SetLinearVelocity(Entity entity, const glm::vec3& velocity);   // Dynamic bodies only
		glm::vec3 GetAngularVelocity(Entity entity) const;                  // Radians per second
		bool SetAngularVelocity(Entity entity, const glm::vec3& velocity);  // Dynamic bodies only

		// Forces and torques act during the next step only; impulses change the velocity immediately. Dynamic bodies only.
		bool AddForce(Entity entity, const glm::vec3& force);
		bool AddForceAtPosition(Entity entity, const glm::vec3& force, const glm::vec3& worldPosition);
		bool AddImpulse(Entity entity, const glm::vec3& impulse);
		bool AddImpulseAtPosition(Entity entity, const glm::vec3& impulse, const glm::vec3& worldPosition);
		bool AddTorque(Entity entity, const glm::vec3& torque);

		// Updates the body and the entity's RigidBodyComponent::GravityScale.
		bool SetGravityScale(Entity entity, float gravityScale);
		bool IsSleeping(Entity entity) const; // False for static bodies (they never simulate) and entities without a body
		bool WakeUp(Entity entity);
		// Moves the body and its entity instantly (keeping velocities and the entity's world scale).
		bool Teleport(Entity entity, const glm::vec3& position, const glm::quat& rotation);

		//////////////////////////////////////////////////////////////////////////
		// Queries (bodies of the simulation as of the last change; triggers are skipped unless includeTriggers is set)
		//////////////////////////////////////////////////////////////////////////

		// Closest hit of the ray. Rays starting inside a convex collider do not hit it. maxDistance is clamped to 1e5.
		std::optional<RaycastHit> Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask = c_AllPhysicsLayers, Entity ignoreEntity = {}, bool includeTriggers = false) const;
		// Every entity hit by the ray (its closest hit), sorted by distance.
		std::vector<RaycastHit> RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask = c_AllPhysicsLayers, Entity ignoreEntity = {}, bool includeTriggers = false) const;
		// Entities whose colliders overlap the sphere / oriented box, in a deterministic order.
		std::vector<Entity> OverlapSphere(const glm::vec3& center, float radius, uint32_t layerMask = c_AllPhysicsLayers, bool includeTriggers = false) const;
		std::vector<Entity> OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, uint32_t layerMask = c_AllPhysicsLayers, bool includeTriggers = false) const;

		PhysicsStats GetStats() const;
	private:
		Scope<PhysicsWorldData> m_Data;
	};

}
