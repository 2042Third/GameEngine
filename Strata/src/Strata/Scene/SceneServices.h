#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/UUID.h"
#include "Strata/Scene/Entity.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

// Scene services: the interfaces through which the systems of a running scene use each other.
//
// A system that offers a service registers it with its scene for as long as it exists (Scene::RegisterService, in its
// constructor; Scene::UnregisterService in its destructor), so it is available from the first OnRuntimeStart of any
// system to the last OnRuntimeStop. Other systems look it up when they need it (Scene::GetService<T>) and get null when
// no running system offers it: the module is not part of the build, or its system does not run in the scene's mode
// (Simulate has no audio). So the Scripting and Audio modules use physics and audio through the Scene layer and never
// include each other's headers (the layers of StrataTests/Architecture/Layers.json).
//
// The built-in providers: PhysicsSystem offers ICollisionEventSource, IBodyMotionQuery, IBodyControl and IPhysicsQueries;
// AudioSystem offers IAudioPlayback. All services are main thread only, like the scene. IAudioOutput, defined here too, is
// an engine service (Core/EngineServices.h): it belongs to the process, not to a scene.
namespace Strata
{

	//////////////////////////////////////////////////////////////////////////
	// Contacts
	//////////////////////////////////////////////////////////////////////////

	enum class CollisionEventType : uint8_t
	{
		Begin = 0, // Two bodies started touching (or a body entered a trigger)
		End        // They stopped touching, or one of them was removed from the simulation while touching
	};

	// A change in the contact state of two entities (those owning the bodies). Every Begin is eventually followed by exactly
	// one End for the same pair, except when the scene stops running (the simulation is torn down without events).
	struct CollisionEvent
	{
		CollisionEventType Type = CollisionEventType::Begin;
		bool IsTrigger = false; // At least one of the bodies is a trigger (no collision response)

		// The entities involved. A handle is invalid when its entity was destroyed before the event was dispatched
		// (typical for End events caused by destruction); AID and BID always identify the entities.
		Entity A;
		Entity B;
		UUID AID = UUID::Null();
		UUID BID = UUID::Null();

		glm::vec3 Point = glm::vec3(0.0f);  // World space contact point (the last known one for End events)
		glm::vec3 Normal = glm::vec3(0.0f); // World space contact normal pointing from A towards B

		bool Involves(UUID id) const { return AID == id || BID == id; }
		// The entity on the other side of the contact from `id` (null if `id` is not involved).
		UUID GetOther(UUID id) const { return AID == id ? BID : (BID == id ? AID : UUID::Null()); }
	};

	using CollisionCallback = std::function<void(const CollisionEvent&)>;
	using CollisionListenerID = uint64_t;
	constexpr CollisionListenerID c_InvalidCollisionListener = 0;

	// Which contacts a collision listener wants.
	enum class CollisionListenerScope : uint8_t
	{
		AllEntities = 0, // Every contact of the scene (tools, tests, gameplay that watches everything)
		WatchedEntities  // Only contacts of entities someone watches (ICollisionEventSource::WatchCollisions)
	};

	// Contact events of the running simulation (PhysicsSystem).
	//
	// Keeping track of contacts costs per touching pair, so it is done only for pairs someone wants: while a listener with
	// the AllEntities scope is registered every pair is tracked, otherwise only pairs where at least one entity is watched.
	// A pair is tracked from the first step that reports it after it became wanted (so a listener added while bodies rest
	// on each other gets Begin events for those contacts once they are awake) until it ends; a pair nobody wants any more
	// ends at the next step that would have reported it.
	class ICollisionEventSource
	{
	public:
		virtual ~ICollisionEventSource() = default;

		// Events are dispatched after each simulation step, on the main thread, to the listeners in registration order;
		// listeners of the WatchedEntities scope only get the events of pairs with a watched entity (watched by anyone).
		// A listener added while events are being dispatched receives events from the next dispatch on; a removed listener
		// is not called again, even for the rest of the current dispatch. Listeners may use the whole scene and the other
		// services, including destroying entities.
		virtual CollisionListenerID AddCollisionListener(CollisionCallback callback, CollisionListenerScope scope) = 0;
		virtual bool RemoveCollisionListener(CollisionListenerID id) = 0;

		// Asks for the contacts of an entity (the one owning a body) to be tracked, for listeners of the WatchedEntities
		// scope. Watches add up: the entity is watched until each WatchCollisions has had its UnwatchCollisions. Entities
		// are named by UUID, which is never reused; watching one that has no body yet is fine (it applies once it has one).
		virtual void WatchCollisions(UUID entity) = 0;
		virtual void UnwatchCollisions(UUID entity) = 0;
	};

	//////////////////////////////////////////////////////////////////////////
	// Rigid bodies
	//////////////////////////////////////////////////////////////////////////

	// Reading the motion of the simulated bodies (PhysicsSystem). Entities that do not own a body in the simulation get
	// false, zero or nothing.
	class IBodyMotionQuery
	{
	public:
		virtual ~IBodyMotionQuery() = default;

		// True if the entity owns a body that is part of the simulation (an active entity with a RigidBody and colliders,
		// or a collider entity without a rigid body ancestor).
		virtual bool HasBody(Entity entity) = 0;
		virtual glm::vec3 GetLinearVelocity(Entity entity) = 0;
		virtual glm::vec3 GetAngularVelocity(Entity entity) = 0;  // Radians per second
		// The velocity of the point `worldPoint` moving with the entity's body: its linear velocity plus its angular velocity
		// about the entity's origin, crossed with the point's offset from it; nothing if the entity owns no body.
		virtual std::optional<glm::vec3> GetPointVelocity(Entity entity, const glm::vec3& worldPoint) = 0;
		virtual bool IsSleeping(Entity entity) = 0; // False for static bodies and entities without a body
	};

	// Changing the motion of the simulated bodies (PhysicsSystem). Velocities, forces and impulses act on dynamic bodies
	// only; functions return false for entities without such a body and for non-finite values.
	class IBodyControl
	{
	public:
		virtual ~IBodyControl() = default;

		virtual bool SetLinearVelocity(Entity entity, const glm::vec3& velocity) = 0;
		virtual bool SetAngularVelocity(Entity entity, const glm::vec3& velocity) = 0;
		// Forces and torques act during the next step only; impulses (linear and angular) change the velocity immediately.
		virtual bool AddForce(Entity entity, const glm::vec3& force) = 0;
		virtual bool AddForceAtPosition(Entity entity, const glm::vec3& force, const glm::vec3& worldPosition) = 0;
		virtual bool AddImpulse(Entity entity, const glm::vec3& impulse) = 0;
		virtual bool AddImpulseAtPosition(Entity entity, const glm::vec3& impulse, const glm::vec3& worldPosition) = 0;
		virtual bool AddTorque(Entity entity, const glm::vec3& torque) = 0;
		virtual bool AddAngularImpulse(Entity entity, const glm::vec3& impulse) = 0;
		// Updates the body and the entity's RigidBodyComponent::GravityScale (any simulated body).
		virtual bool SetGravityScale(Entity entity, float gravityScale) = 0;
		virtual bool WakeUp(Entity entity) = 0;
		// Moves the body and its entity instantly (any simulated body; keeping velocities and the entity's world scale).
		// False also when the entity's parent is scaled to (nearly) zero, so the pose cannot be written.
		virtual bool Teleport(Entity entity, const glm::vec3& position, const glm::quat& rotation) = 0;
	};

	//////////////////////////////////////////////////////////////////////////
	// Queries
	//////////////////////////////////////////////////////////////////////////

	struct RaycastHit
	{
		Entity HitEntity;
		UUID EntityID = UUID::Null();
		glm::vec3 Point = glm::vec3(0.0f);  // World space hit position
		glm::vec3 Normal = glm::vec3(0.0f); // World space surface normal at the hit
		float Distance = 0.0f;              // Distance from the ray origin along the ray direction
	};

	// Spatial queries against the simulated bodies (PhysicsSystem), as of the last step or change. Bit i of `layerMask`
	// accepts collision layer i (RigidBodyComponent::Layer); triggers are skipped unless includeTriggers is set; entities
	// pending destruction are never reported. Hits report the entity owning the body.
	class IPhysicsQueries
	{
	public:
		virtual ~IPhysicsQueries() = default;

		// The closest hit of the ray (direction need not be normalized). Rays starting inside a convex collider do not hit it.
		virtual std::optional<RaycastHit> Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask,
			Entity ignoreEntity, bool includeTriggers) = 0;
		// Every body the ray hits (its closest hit), sorted by distance.
		virtual std::vector<RaycastHit> RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask,
			Entity ignoreEntity, bool includeTriggers) = 0;
		// The entities whose bodies overlap the sphere / oriented box, in a deterministic order.
		virtual std::vector<Entity> OverlapSphere(const glm::vec3& center, float radius, uint32_t layerMask, bool includeTriggers) = 0;
		virtual std::vector<Entity> OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, uint32_t layerMask,
			bool includeTriggers) = 0;
	};

	//////////////////////////////////////////////////////////////////////////
	// Audio
	//////////////////////////////////////////////////////////////////////////

	// The sounds of a playing scene (AudioSystem; Play mode only): entities with an AudioSourceComponent and one-shots.
	class IAudioPlayback
	{
	public:
		virtual ~IAudioPlayback() = default;

		// Starts or resumes the entity's source; a clip that is still loading plays once it is ready. False without an
		// active AudioSourceComponent, without a playable clip, or without audio output.
		virtual bool Play(Entity entity) = 0;
		virtual bool Pause(Entity entity) = 0;            // Keeps the position (Play resumes)
		virtual bool Stop(Entity entity) = 0;             // Rewinds
		virtual bool IsPlaying(Entity entity) = 0;        // Playing, or about to (clip still loading, scene paused)
		virtual bool Seek(Entity entity, float seconds) = 0;
		virtual float GetPlaybackPosition(Entity entity) = 0; // Seconds into the clip

		// Fire-and-forget sounds owned by the scene, without spatialization or at a world position. False for an unknown or
		// unusable clip, invalid parameters (volume >= 0 and pitch > 0, finite) or without audio output.
		virtual bool PlayOneShot(AssetHandle clip, float volume, float pitch) = 0;
		virtual bool PlayOneShotAt(AssetHandle clip, const glm::vec3& position, float volume, float pitch) = 0;
	};

	// The engine's audio output (AudioEngine), which outlives every scene: an engine service (Core/EngineServices.h) the
	// Audio module registers, not a scene service, so it works whether or not a scene plays audio.
	class IAudioOutput
	{
	public:
		virtual ~IAudioOutput() = default;

		// The master volume, like a game's sound option: 0 silences, 1 is full volume (see AudioEngine::SetMasterVolume).
		virtual void SetMasterVolume(float volume) = 0;
		virtual float GetMasterVolume() = 0;
	};

}
