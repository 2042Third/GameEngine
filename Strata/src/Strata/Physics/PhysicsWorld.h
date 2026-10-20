#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/UUID.h"
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

	// How a world builds the bodies of the scene it is created for.
	enum class PhysicsBuildMode : uint8_t
	{
		Immediate = 0, // In the constructor (play starts, tools)
		Sliced         // Over calls of BuildSlice: a loaded scene prepared before it runs (PhysicsSystem::PrepareLoadedScene)
	};

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
	// An entity whose body cannot be built yet keeps a pending record and is retried once that can succeed: a degenerate
	// (e.g. zero scale) world transform when a transform change is signaled (see below), a mesh collider when its mesh data
	// is available and its shape cooked (see SetMeshProvider), a full world when bodies are freed. A body whose world
	// transform becomes degenerate leaves the simulation until a signaled change (or reactivation) makes it valid again.
	// Dynamic bodies write their pose back relative to their parent, so for them a parent scaled to (nearly) zero, which
	// cannot be inverted, counts as a degenerate transform too.
	//
	// The world listens to the scene registry: component, reparenting (Scene::SetParent), activity and entity-destruction
	// changes are applied by ApplyPendingChanges (called by Simulate and by PhysicsSystem before queries); reparenting
	// moves colliders between bodies. Each step:
	//  - bodies follow transform changes of their entity: kinematic bodies move towards it, dynamic bodies are teleported
	//    (waking bodies resting on them) and static bodies are repositioned. Changes are noticed when they are signaled
	//    (TransformComponent on_update, e.g. Entity::MarkModified, ComponentAccess or Scene::SetWorldTransform, on the
	//    entity or an ancestor) and, for awake bodies, also without an on_update signal (writes that only told the scene,
	//    through Scene::MarkTransformChanged: every direct write must at least do that, see Scene); the bodies below an
	//    awake body found moved that way follow it in the same step. Sleeping and static bodies are never polled
	//    otherwise, so writes without on_update go unnoticed for them (a kinematic body moved every step stays awake);
	//  - entities that are inactive in the hierarchy or pending destruction (or under such an ancestor) leave the
	//    simulation; static bodies pending destruction stay until they are destroyed, but queries skip them;
	//  - dynamic bodies write their simulated pose back to their entity, parents before children, changing only its
	//    translation and rotation (the authored scale stays, mirrored axes included, unless a sheared parent makes that
	//    impossible). Dynamic descendants of a moving dynamic body keep their own world pose; kinematic and static
	//    descendants follow it.
	// The work of a step grows with the awake bodies, their contacts and the signaled changes, not with sleeping or static
	// bodies or the world's capacity (see PhysicsStats::SyncedBodyCount, CheckedPairCount, WrittenBodyCount and
	// OrderedSlotCount).
	//
	// Character controllers (CharacterControllerComponent, see CharacterControllerSet) move in each step before Jolt's: they
	// walk, fall and push dynamic bodies, write their entities' translations, and their contacts join the bodies' (the
	// entity pairs above), so contact events and the scripts' callbacks report them like bodies' contacts.
	//
	// The simulation waits for mesh colliders at the start: while a mesh collider that exists before the first step waits for
	// mesh data the provider is loading or for its shape to be cooked, Simulate holds the step (nothing moves and no
	// collision events occur, but changes are still applied and queries work), so that bodies do not fall through mesh
	// floors that are not there yet. PhysicsStats::WaitingForMeshes and HeldStepCount report it. The wait ends after at
	// most PhysicsSettings::MeshWaitTimeout seconds of simulation time, with a warning naming the colliders that are still
	// missing; mesh colliders added later, unknown meshes and meshes that failed to load are not waited for.
	//
	// Capacities: Jolt allocates the world for fixed numbers of bodies, body pairs and contact constraints. Capacities that are
	// 0 in the settings are sized when the world is created from the bodies the scene has then (ResolveCapacities), so a scene
	// gets the room it needs without settings. A body that finds the world full waits for a slot, queued in the order it
	// failed (no step looks at it; slots that free go to the first waiters), and the world grows before its next step (also
	// when Reserve asked for more room than it has): it moves into a new Jolt system for the next power of two of at least
	// twice its bodies and what it needs, up to PhysicsSettings::GrowthLimit. Every body keeps its JPH::BodyID, pose,
	// velocity, sleep state and contacts, and character controllers keep their state; automatic pair and contact capacities
	// grow with it, given ones stay, and a given MaxBodies is exceeded with a warning (correctness comes first). The bodies
	// are moved in fixed chunks on the job threads, and the old system is destroyed by a background job. At its growth limit
	// a world keeps bodies waiting (one aggregated error names them) until bodies are removed. Steps that overflow the pair
	// or contact buffers drop contacts (a warning once per world). Both are recorded (PhysicsStats::BodyOverflowCount,
	// ContactOverflowSteps, Demand, GrowthCount), so the next world of the scene starts large enough.
	//
	// Main thread only. Jolt runs the step on Strata's JobSystem workers when it is initialized.
	class PhysicsWorld
	{
	public:
		// Creates the bodies of every entity in the scene and starts listening to its changes. `demand` is what an earlier
		// world of the scene needed beyond its capacities: automatic capacities are sized for at least that much.
		// A Sliced world only starts listening and builds its bodies over BuildSlice calls.
		explicit PhysicsWorld(Scene& scene, const PhysicsSettings& settings = PhysicsSettings(), const PhysicsCapacityDemand& demand = {},
			PhysicsBuildMode mode = PhysicsBuildMode::Immediate);
		~PhysicsWorld(); // Destroys every body without emitting collision events

		// The settings a world is created with for `bodyCount` bodies: capacities given in `settings` stay (clamped to the
		// engine's limits), automatic ones (0) become
		//  - MaxBodies: the next power of two of at least twice the bodies, at least 65,536;
		//  - MaxBodyPairs: four times MaxBodies;
		//  - MaxContactConstraints: twice the bodies, at least 10,240;
		// each at least what `demand` asks for, and an automatic TempAllocatorSize holds a step's scratch data for those
		// capacities (at least 10 MiB).
		static PhysicsSettings ResolveCapacities(const PhysicsSettings& settings, uint32_t bodyCount, const PhysicsCapacityDemand& demand = {});
		// What a world of these resolved capacities (see ResolveCapacities) reserves when it is created, in bytes: Jolt's
		// buffers (about 48 bytes per body, 90 per body pair and some 300 per contact constraint) and the step's scratch
		// memory. Fitted to the commit charge the reference machine measures (Perf.Physics.MostBodies.ReservedMemory and
		// LargestCapacities.ReservedMemory, within 15%); systems that count only touched pages report less.
		static uint64_t EstimateReservedBytes(const PhysicsSettings& settings);
		// The entities of a scene that may own a body when a world is created for it: those with a RigidBody, collider entities
		// without one (colliders merged into an ancestor's body count too, so this is an upper bound) and character
		// controllers (their presence bodies). Linear, without allocating. Automatic capacities and the world cook count by it.
		static uint32_t CountBodyCandidates(const Scene& scene);

		PhysicsWorld(const PhysicsWorld&) = delete;
		PhysicsWorld& operator=(const PhysicsWorld&) = delete;

		// Source of the triangle data of MeshColliderComponent meshes (process-wide, main thread). By default meshes come from
		// the active asset manager (AssetMeshProvider); a null provider restores that default. While a mesh's data is not
		// available (still loading) or its shape is being cooked (on the JobSystem), the collider is missing from its body
		// (a body without other colliders is pending); bodies are rebuilt when the provider reports changed mesh data.
		static void SetMeshProvider(Ref<PhysicsMeshProvider> provider);
		static PhysicsMeshProvider& GetMeshProvider();

		Scene& GetScene() const;
		// The settings in effect, capacities resolved (see ResolveCapacities).
		const PhysicsSettings& GetSettings() const;
		// What the world needed beyond its capacities so far (see PhysicsCapacityDemand); empty while they suffice.
		PhysicsCapacityDemand GetCapacityDemand() const;

		//////////////////////////////////////////////////////////////////////////
		// Bodies
		//////////////////////////////////////////////////////////////////////////

		// Applies the component, hierarchy-activity and destruction changes recorded since the last call.
		void ApplyPendingChanges();

		// Builds the bodies of up to `maxOwners` more entities of a Sliced world, in hierarchy order (in parallel, see
		// "Parallel creation"), with the changes recorded meanwhile; true once every body is built (at once for an Immediate
		// world). The last call rebuilds the broad phase's trees, as the constructor of an Immediate world does.
		bool BuildSlice(uint32_t maxOwners);
		bool IsBuilt() const;

		// What the world holds and has room for, without work that grows with the bodies (see IPhysicsCapacity). The last
		// update's counters (also PhysicsStats::BodiesAddedLastUpdate, BodiesRemovedLastUpdate, ChangeTime) are those of the
		// scene's update before its current frame index (Scene::GetFrameIndex): the update that just ended when read between
		// updates, the previous one when read during an update.
		PhysicsCapacityInfo GetCapacityInfo() const;

		// Asks for room for `bodies` bodies in all (character controllers' presence bodies included): a world with less grows
		// before its next step (see "Capacities"). The world streamer reserves for the cells it is about to load, so their
		// bodies never find the world full. The latest call counts; 0 reserves nothing.
		void Reserve(uint32_t bodies);
		uint32_t GetReservedBodies() const;
		// Applies the pending changes and grows the world now if bodies wait for a slot or more were reserved than it has room
		// for, as the next step would. Returns whether it grew. Never call it during a step, a query or a collision dispatch.
		bool Grow();

		// True if the entity owns a body that is part of the simulation.
		bool HasBody(Entity entity) const;
		// The physics library's ID of the body the entity owns (its slot and the slot's generation; the world it belongs to
		// is left out, so that two worlds built alike give equal IDs), also out of the simulation, or c_NoBodyID: for
		// diagnostics and tests (which bodies a build gave which IDs).
		static constexpr uint64_t c_NoBodyID = UINT64_MAX;
		uint64_t GetBodyID(Entity entity) const;
		// The entity owning the body that holds this entity's colliders (the entity itself, its nearest ancestor with a
		// RigidBodyComponent, or invalid if its colliders are not part of any body).
		Entity GetBodyEntity(Entity entity) const;

		// Applies pending changes, synchronizes entity transforms to the bodies, advances the simulation by `timestep`
		// seconds, writes dynamic bodies back to their entities and updates the contact state. At the start, steps are held
		// while mesh colliders wait for their meshes (see above).
		void Simulate(float timestep);

		// Collision events queued since the last call, in the order they occurred. Entity handles are resolved now. Events
		// accumulate until taken (PhysicsSystem takes them after every update).
		std::vector<CollisionEvent> TakeCollisionEvents();

		// Which contacts become events (see ICollisionEventSource): while tracking all contacts (the default) every touching
		// pair, otherwise only pairs with an entity of interest (one owning a body). Contacts of pairs nobody wants are not
		// recorded during the step, so they cost nothing beyond Jolt's own work. A pair is tracked from the first step that
		// reports it while it is wanted; one that stops being wanted ends at the next step that checks it.
		void SetTrackAllContacts(bool trackAll);
		bool IsTrackingAllContacts() const;
		void SetContactInterest(UUID entity, bool interested);

		//////////////////////////////////////////////////////////////////////////
		// World settings
		//////////////////////////////////////////////////////////////////////////

		// Non-finite values are rejected. Changing gravity wakes every dynamic body.
		bool SetGravity(const glm::vec3& gravity);
		glm::vec3 GetGravity() const;

		// The layer collision matrix the world uses (PhysicsSettings::LayerCollisionMasks). PhysicsSystem replaces it with the
		// scene's when that changes; SetLayerCollisionMasks wakes the bodies so that they notice.
		bool DoLayersCollide(uint32_t layerA, uint32_t layerB) const;
		void SetLayerCollisionMasks(const std::array<uint32_t, c_PhysicsLayerCount>& masks);

		//////////////////////////////////////////////////////////////////////////
		// Body API (entities that do not own a body in the simulation get false / zero)
		//////////////////////////////////////////////////////////////////////////

		glm::vec3 GetLinearVelocity(Entity entity) const;
		bool SetLinearVelocity(Entity entity, const glm::vec3& velocity);   // Dynamic bodies only
		glm::vec3 GetAngularVelocity(Entity entity) const;                  // Radians per second
		bool SetAngularVelocity(Entity entity, const glm::vec3& velocity);  // Dynamic bodies only

		// Forces and torques act during the next step only; impulses (linear and angular) change the velocity immediately.
		// Dynamic bodies only.
		bool AddForce(Entity entity, const glm::vec3& force);
		bool AddForceAtPosition(Entity entity, const glm::vec3& force, const glm::vec3& worldPosition);
		bool AddImpulse(Entity entity, const glm::vec3& impulse);
		bool AddImpulseAtPosition(Entity entity, const glm::vec3& impulse, const glm::vec3& worldPosition);
		bool AddTorque(Entity entity, const glm::vec3& torque);
		bool AddAngularImpulse(Entity entity, const glm::vec3& impulse);

		// Updates the body and the entity's RigidBodyComponent::GravityScale.
		bool SetGravityScale(Entity entity, float gravityScale);
		bool IsSleeping(Entity entity) const; // False for static bodies (they never simulate) and entities without a body
		bool WakeUp(Entity entity);
		// Moves the body and its entity instantly (keeping velocities and the entity's world scale) and wakes the bodies
		// around its old and new place.
		bool Teleport(Entity entity, const glm::vec3& position, const glm::quat& rotation);

		//////////////////////////////////////////////////////////////////////////
		// Character controllers (see CharacterControllerSet and ICharacterControl; entities without a controller in the
		// simulation get false or nothing)
		//////////////////////////////////////////////////////////////////////////

		bool HasCharacterController(Entity entity) const;
		bool MoveCharacter(Entity entity, const glm::vec3& velocity);
		bool JumpCharacter(Entity entity, float speed);
		// Moves the controller and its entity at once (bodies of the entity and below it follow at the next step).
		bool TeleportCharacter(Entity entity, const glm::vec3& position);
		bool AddCharacterMotion(Entity entity, const glm::vec3& displacement);
		std::optional<CharacterState> GetCharacterState(Entity entity) const;

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
