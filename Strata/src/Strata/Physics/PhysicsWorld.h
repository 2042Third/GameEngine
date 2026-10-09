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
	// Bodies are built from entity components:
	//  - An entity with a RigidBodyComponent owns a body. Its shape is made of the entity's own colliders and those of its
	//    descendants that have no RigidBodyComponent of their own (placed with their transform relative to the owner);
	//    descendants with their own RigidBodyComponent are separate bodies.
	//  - Collider entities without a RigidBodyComponent on themselves or any ancestor are static bodies of their own.
	//  - Several colliders form a compound shape. World scale is baked into the shapes: boxes and mesh colliders scale
	//    exactly (including non-uniform and mirrored scale), sphere radii scale by the largest absolute axis scale,
	//    capsule radii by the larger of |X| and |Z| and their half height by |Y|. Shear (non-uniform scale under a rotated
	//    parent) cannot be represented and is approximated by the decomposed scale.
	//  - Lock rotation flags refer to world axes. Collision events and query hits report the entity that owns the body.
	//
	// An entity whose body cannot be built yet keeps a pending record and is retried every step: a degenerate (e.g. zero
	// scale) world transform until it becomes valid, a mesh collider until the mesh provider returns its data, a full world
	// until bodies are freed. A body whose world transform becomes degenerate leaves the simulation until it is valid again.
	//
	// The world listens to the scene registry: component, hierarchy-activity and entity-destruction changes are applied by
	// ApplyPendingChanges (called by Simulate and by PhysicsSystem before queries). Each step:
	//  - kinematic bodies are moved towards their entity's transform; dynamic bodies whose transform was changed from
	//    outside physics are teleported (waking bodies resting on them); both are checked every step;
	//  - static bodies follow their entity's transform when a change is signaled (TransformComponent on_update, e.g.
	//    Entity::MarkModified or ComponentAccess, on the entity or an ancestor) or through Teleport; direct field writes
	//    without a signal are not noticed (move colliders that change every frame with a kinematic rigid body);
	//  - entities that are inactive in the hierarchy or pending destruction (or under such an ancestor) leave the
	//    simulation; static bodies pending destruction stay until they are destroyed, but queries skip them;
	//  - dynamic bodies write their simulated pose back to their entity, parents before children. Dynamic descendants of
	//    a moving dynamic body keep their own world pose; kinematic and static descendants follow it.
	//
	// Main thread only. Jolt runs the step on Strata's JobSystem workers when it is initialized.
	class PhysicsWorld
	{
	public:
		// Creates the bodies of every entity in the scene and starts listening to its changes.
		explicit PhysicsWorld(Scene& scene, const PhysicsSettings& settings = PhysicsSettings());
		~PhysicsWorld(); // Destroys every body without emitting collision events

		PhysicsWorld(const PhysicsWorld&) = delete;
		PhysicsWorld& operator=(const PhysicsWorld&) = delete;

		// Source of triangle data for MeshColliderComponent (process-wide, main thread). While there is no provider, or it
		// returns nullptr for a mesh (e.g. still loading), the collider is missing from its body and is retried every step.
		static void SetMeshProvider(PhysicsMeshProvider provider);
		static bool HasMeshProvider();

		Scene& GetScene() const;
		const PhysicsSettings& GetSettings() const;

		//////////////////////////////////////////////////////////////////////////
		// Bodies
		//////////////////////////////////////////////////////////////////////////

		// Applies the component, hierarchy-activity and destruction changes recorded since the last call.
		void ApplyPendingChanges();

		// True if the entity owns a body that is part of the simulation.
		bool HasBody(Entity entity) const;
		// The entity owning the body that holds this entity's colliders (the entity itself, its nearest ancestor with a
		// RigidBodyComponent, or invalid if its colliders are not part of any body).
		Entity GetBodyEntity(Entity entity) const;

		// Applies pending changes, synchronizes entity transforms to the bodies, advances the simulation by `timestep`
		// seconds, writes dynamic bodies back to their entities and updates the contact state.
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
		// Body API (entities that do not own a body in the simulation get false / zero)
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
		// Moves the body and its entity instantly (keeping velocities and the entity's world scale) and wakes the bodies
		// around its old and new place.
		bool Teleport(Entity entity, const glm::vec3& position, const glm::quat& rotation);

		//////////////////////////////////////////////////////////////////////////
		// Queries (bodies of the simulation as of the last step or change; triggers are skipped unless includeTriggers is
		// set; entities pending destruction are never reported)
		//////////////////////////////////////////////////////////////////////////

		// Closest hit of the ray. Rays starting inside a convex collider do not hit it. maxDistance is clamped to 1e5.
		std::optional<RaycastHit> Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask = c_AllPhysicsLayers, Entity ignoreEntity = {}, bool includeTriggers = false) const;
		// Every body hit by the ray (its closest hit), sorted by distance.
		std::vector<RaycastHit> RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask = c_AllPhysicsLayers, Entity ignoreEntity = {}, bool includeTriggers = false) const;
		// Entities whose bodies overlap the sphere / oriented box, in a deterministic order.
		std::vector<Entity> OverlapSphere(const glm::vec3& center, float radius, uint32_t layerMask = c_AllPhysicsLayers, bool includeTriggers = false) const;
		std::vector<Entity> OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, uint32_t layerMask = c_AllPhysicsLayers, bool includeTriggers = false) const;

		PhysicsStats GetStats() const;
	private:
		Scope<PhysicsWorldData> m_Data;
	};

}
