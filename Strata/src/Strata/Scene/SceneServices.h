#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/UUID.h"
#include "Strata/Math/AABB.h"
#include "Strata/Scene/Entity.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
// The built-in providers: PhysicsSystem offers ICollisionEventSource, IBodyMotionQuery, IBodyControl, IPhysicsQueries,
// ICharacterControl and IPhysicsCapacity; AudioSystem offers IAudioPlayback; the World module's system offers
// IWorldStreaming; the Animation module's system offers ISkinningPoses, IAnimatorControl and IAnimationEventSource (the
// editor's pose preview provides ISkinningPoses for edited scenes, where no system runs); the GameUI module's system offers
// IUIEvents and IUIControl. All services are main thread only, like the scene. IAudioOutput, defined here too, is an
// engine service (Core/EngineServices.h): it belongs to the process, not to a scene.
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
		// The entity whose body moves this one: itself or its nearest ancestor with a RigidBody component; an invalid entity
		// when none has one (what moves it is not simulated).
		virtual Entity GetSimulatingBody(Entity entity) = 0;
		// Whether the entity's RigidBody is dynamic: only dynamic bodies have velocities, forces and impulses applied to them
		// (false without a RigidBody: colliders alone are static bodies).
		virtual bool IsDynamicBody(Entity entity) = 0;
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
	// Character controllers
	//////////////////////////////////////////////////////////////////////////

	// Where a character controller stands, as of its last fixed step.
	enum class CharacterGroundState : uint8_t
	{
		OnGround = 0,  // On walkable ground (not steeper than its MaxSlopeDegrees)
		OnSteepGround, // Touching ground too steep to walk on: it slides down
		NotSupported,  // Touching something, but nothing below holds it (e.g. pressed against a wall)
		InAir          // Touching nothing
	};

	struct CharacterState
	{
		CharacterGroundState Ground = CharacterGroundState::InAir;
		glm::vec3 GroundNormal = glm::vec3(0.0f);   // The surface normal of what it stands on (zero in the air)
		glm::vec3 GroundVelocity = glm::vec3(0.0f); // The velocity of what it stands on (a moving platform)
		glm::vec3 Velocity = glm::vec3(0.0f);       // How fast it moved in its last fixed step, after collisions
		Entity GroundEntity;                        // The entity owning what it stands on (invalid in the air)
	};

	// The character controllers of the running simulation (PhysicsSystem): entities with an active, enabled
	// CharacterControllerComponent and no RigidBodyComponent. Functions fail (false, nothing) for other entities and for
	// non-finite values. What they set takes effect at the next fixed step.
	class ICharacterControl
	{
	public:
		virtual ~ICharacterControl() = default;

		virtual bool HasCharacterController(Entity entity) = 0;
		// The velocity (meters per second, world space) the character walks with from now on, until the next call: the
		// horizontal part moves it along the ground; the vertical part is used only while its GravityScale is 0 (flying,
		// swimming), otherwise gravity, jumps and the ground decide how it moves up and down.
		virtual bool MoveCharacter(Entity entity, const glm::vec3& velocity) = 0;
		// Makes it move up at `speed` (meters per second) at its next fixed step, on the ground or not.
		virtual bool JumpCharacter(Entity entity, float speed) = 0;
		// Moves it (and its entity) to a world position at once, keeping how it walks and dropping its vertical velocity.
		virtual bool TeleportCharacter(Entity entity, const glm::vec3& position) = 0;
		// A displacement (world space) added to its next fixed step's movement, colliding like walking: how animation root
		// motion moves a character. Displacements add up until that step.
		virtual bool AddCharacterMotion(Entity entity, const glm::vec3& displacement) = 0;
		virtual std::optional<CharacterState> GetCharacterState(Entity entity) = 0;
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

		// Whether the entity has an AudioSourceComponent (whether or not it is active).
		virtual bool HasSource(Entity entity) = 0;
	};

	//////////////////////////////////////////////////////////////////////////
	// Animation
	//////////////////////////////////////////////////////////////////////////

	// The pose of an animator's skeleton: per joint its transform relative to the animator's entity (model space).
	struct SkeletonPose
	{
		std::span<const glm::mat4> Joints; // Valid until the provider changes the pose (the next animation update)
		uint64_t Version = 0;              // Changes whenever the joints or the reach change
		// For an animator that does not compute its pose every update (WhenVisible): where what it plays takes the joints'
		// origins (model space, like the joints), and the largest scale of a joint's transform there. Renderers bound its
		// meshes with it as well as with the pose, so they find it wherever its clips take it, also while it shows an older
		// pose. Invalid (empty) for poses that are computed every update.
		AABB Reach;
		float ReachScale = 0.0f;
	};

	// An entity drawn at another world transform than the scene has (ISkinningPoses::GetRenderTransforms).
	struct RenderTransform
	{
		entt::entity Entity = entt::null;
		glm::mat4 World = glm::mat4(1.0f);
	};

	// The poses animators publish (the Animation module's system in Play mode; the editor's pose preview), which renderers
	// skin meshes with (SkinComponent): the renderer never includes the Animation module. Renderers read them in
	// RenderScene::Sync and report which animators' meshes they drew, so animators that only animate while seen
	// (AnimatorComponent::CullingMode WhenVisible) know.
	class ISkinningPoses
	{
	public:
		virtual ~ISkinningPoses() = default;

		// The current pose of the animator on `animator` (an entity with an AnimatorComponent); false while it has none (no
		// skeleton loaded yet, not evaluated yet, inactive).
		virtual bool GetPose(entt::entity animator, SkeletonPose& outPose) const = 0;
		// Changes whenever any pose changes: a renderer that saw this version has seen every pose.
		virtual uint64_t GetPosesVersion() const = 0;
		// A renderer drew (in its view or its shadows) a mesh this animator poses, or would have, had it been posed. Returns
		// whether the pose it drew is older than the animator's time (a WhenVisible animator that skipped its poses while
		// unseen): the provider poses it for the next frame, also while the scene is paused, so a renderer that must show the
		// pose of the moment (a capture) renders again then.
		virtual bool ReportVisible(entt::entity animator) = 0;

		// Entities renderers draw at other world transforms than the scene's: a provider that may not change the scene (the
		// editor's pose preview of a scene that does not play) places attachments (AttachToJointComponent), and the meshes
		// under them, at their joints this way. Valid until the provider changes them; the version changes whenever they do.
		// None by default: the animation system of a playing scene moves attachments in the scene.
		virtual std::span<const RenderTransform> GetRenderTransforms() const { return {}; }
		virtual uint64_t GetRenderTransformsVersion() const { return 0; }
	};

	// Commands and queries of the animators of a playing scene (the Animation module's system), for code that may not
	// include the module (the script host). Every function takes the entity with the AnimatorComponent and fails (false,
	// nothing, 0) for entities without one, unknown states or parameters, and wrong values; the system logs nothing, the
	// callers report misuse their way.
	class IAnimatorControl
	{
	public:
		virtual ~IAnimatorControl() = default;

		// Jumps to a state of the animator's controller (or, PlayClip, plays a clip without the controller's transitions until
		// a state is played again); CrossFade blends to the state over `seconds`.
		virtual bool Play(Entity animator, std::string_view state) = 0;
		virtual bool PlayClip(Entity animator, AssetHandle clip) = 0;
		virtual bool CrossFade(Entity animator, std::string_view state, float seconds) = 0;
		// Parameters of the controller by name and type; a trigger stays set until a transition consumes it or it is reset.
		virtual bool SetFloat(Entity animator, std::string_view parameter, float value) = 0;
		virtual bool SetBool(Entity animator, std::string_view parameter, bool value) = 0;
		virtual bool SetTrigger(Entity animator, std::string_view parameter) = 0;
		virtual bool ResetTrigger(Entity animator, std::string_view parameter) = 0;
		// The animator's speed (AnimatorComponent::Speed, which the call writes).
		virtual bool SetSpeed(Entity animator, float speed) = 0;

		// The state the animator is in: the target of the latest transition or command, also one waiting for the controller to
		// load (an empty name while it plays a clip directly, or before it plays); nothing for entities without an animator.
		virtual std::optional<std::string> GetCurrentState(Entity animator) = 0;
		// How far the state is: its time over its clip's duration, counting loops.
		virtual float GetNormalizedTime(Entity animator) = 0;
		virtual bool IsInTransition(Entity animator) = 0;
		// A joint's world transform in the animator's current pose (attach things to a hand); nothing without a pose or for a
		// joint the skeleton does not have.
		virtual std::optional<glm::mat4> GetJointTransform(Entity animator, std::string_view joint) = 0;

		// Whether the entity has an AnimatorComponent.
		virtual bool HasAnimator(Entity entity) = 0;
	};

	// An event of an animation controller's state that the playhead passed (AnimationEvent): dispatched on the main thread
	// after the animators were evaluated, animator by animator, each animator's in the order they happened.
	struct AnimationEventInfo
	{
		Entity Animator;
		UUID AnimatorID = UUID::Null();
		std::string_view Name;  // Valid during the call
		std::string_view State;
		float Time = 0.0f;      // The event's time in its clip
	};

	using AnimationEventCallback = std::function<void(const AnimationEventInfo&)>;
	using AnimationEventListenerID = uint64_t;

	// The animation events of a playing scene (the Animation module's system). Listeners added while events are dispatched
	// receive the next update's; a removed one is not called again. Listeners may use the whole scene and the other services.
	class IAnimationEventSource
	{
	public:
		virtual ~IAnimationEventSource() = default;

		virtual AnimationEventListenerID AddAnimationEventListener(AnimationEventCallback callback) = 0;
		virtual bool RemoveAnimationEventListener(AnimationEventListenerID id) = 0;
	};

	//////////////////////////////////////////////////////////////////////////
	// Physics capacity
	//////////////////////////////////////////////////////////////////////////

	// What a running scene's physics world holds and has room for (IPhysicsCapacity). Bodies count the body slots in use:
	// every body of the world, also those of inactive entities, and character controllers' presence bodies.
	struct PhysicsCapacityInfo
	{
		uint32_t Bodies = 0;
		uint32_t MaxBodies = 0;          // The capacity now (it grows, see PhysicsWorld); 0 while the scene has no physics world
		uint32_t WaitingForSlot = 0;     // Entities waiting for a body slot: a world at its growth limit, or before its growth
		uint32_t PendingOther = 0;       // Entities waiting for a body for other reasons (a mesh loading, a degenerate transform)
		uint32_t AddedLastUpdate = 0;    // Bodies built (or rebuilt) during the scene's last update
		uint32_t RemovedLastUpdate = 0;  // Bodies destroyed during it
		uint32_t GrowthCount = 0;        // Growths of the world since it was created
		float LastGrowthMs = 0.0f;
		uint64_t ReservedBytes = 0;      // What the world's capacities reserve (PhysicsWorld::EstimateReservedBytes)
	};

	// The capacity of a running scene's physics world (PhysicsSystem), for code that may not include the Physics module: the
	// world streamer reserves room for the cells it is about to load, the editor shows it and games log it. Cheap: no work
	// grows with the bodies.
	class IPhysicsCapacity
	{
	public:
		virtual ~IPhysicsCapacity() = default;

		virtual PhysicsCapacityInfo GetCapacityInfo() const = 0;
		// Asks for room for `bodies` bodies in all: a world with less grows before its next step (see PhysicsWorld::Reserve).
		// The latest call counts; 0 reserves nothing.
		virtual void Reserve(uint32_t bodies) = 0;
	};

	//////////////////////////////////////////////////////////////////////////
	// World streaming
	//////////////////////////////////////////////////////////////////////////

	// What a world's streaming does (WorldStreamer::GetStats, IWorldStreaming): cells by state, the loads and unloads it
	// started, and the entities the cells' slices created and destroyed in the scene's last update.
	struct WorldStreamingStats
	{
		uint32_t CellsWanted = 0;    // Cells within a source's radius (or pinned)
		uint32_t CellsLoading = 0;   // Cells whose scene loads or whose entities are being created
		uint32_t CellsLoaded = 0;
		uint32_t CellsUnloading = 0;
		uint32_t CellsPinned = 0;
		uint32_t CellsFailed = 0;    // Cells whose load failed, not retried until their scene changes
		uint64_t LoadsStarted = 0;   // Since streaming started
		uint64_t UnloadsStarted = 0;
		uint32_t EntitiesCreatedLastUpdate = 0;
		uint32_t EntitiesDestroyedLastUpdate = 0;
		uint32_t EvaluatedCells = 0; // Cells the last update looked at: 0 while nothing moved or changed
		uint64_t StateVersion = 0;   // Changes whenever a cell's state does (a cell map rebuilds when it changed)
	};

	// The streaming of the world a running scene is the persistent scene of (the World module's system), for code that may
	// not include the World module: scripts prefetch cells before a teleport, games toggle layers, the editor and the perf
	// log show the streaming. Main thread.
	class IWorldStreaming
	{
	public:
		virtual ~IWorldStreaming() = default;

		// Streams the cells within `radius` of `position` on every enabled layer, at priority 100 (before the sources'), until
		// released or the scene stops: a request ID, 0 when the position or radius is no finite number (radius >= 0).
		virtual uint64_t RequestCellsAround(const glm::vec3& position, float radius) = 0;
		// False for an unknown or released request.
		virtual bool ReleaseCells(uint64_t request) = 0;
		// Whether every existing cell of every enabled layer within `radius` of `position` is loaded.
		virtual bool IsLoadedAround(const glm::vec3& position, float radius) const = 0;
		// Layers by name (WorldLayer::Name), starting from their EnabledByDefault: a disabled layer's cells unload. False, and
		// nullopt, for a name the world does not have.
		virtual bool SetLayerEnabled(std::string_view layer, bool enabled) = 0;
		virtual std::optional<bool> IsLayerEnabled(std::string_view layer) const = 0;
		virtual WorldStreamingStats GetStats() const = 0;
		// Whether the scene streams a world: its first active World component names a World asset. The service exists in every
		// running scene of an engine with the World module; the calls above fail in one that streams none.
		virtual bool IsStreaming() const = 0;
	};

	//////////////////////////////////////////////////////////////////////////
	// Game UI
	//////////////////////////////////////////////////////////////////////////

	enum class UIEventType : uint8_t
	{
		Click = 0,    // A button was clicked (released over it after a press on it, or Submit while it has the focus)
		ValueChanged, // A slider's value or a toggle's state changed through input (not through code)
		FocusChanged, // A selectable gained or lost the focus
		Submit        // An input field's text was submitted (Enter, or a gamepad's Submit, while it was being edited)
	};

	// An event of a widget of the game UI (the GameUI module's system).
	struct UIEventInfo
	{
		UIEventType Type = UIEventType::Click;
		Entity Widget;
		UUID WidgetID = UUID::Null();
		Entity Target;        // The widget's EventTarget: invalid without one, or when it is the widget itself
		float Value = 0.0f;   // ValueChanged: the slider's value, or a toggle's 0 or 1
		bool Focused = false; // FocusChanged: whether the widget gained the focus
	};

	using UIEventCallback = std::function<void(const UIEventInfo&)>;
	using UIEventListenerID = uint64_t;

	// The events of the game UI's widgets while the scene plays (the GameUI module's "UI" system, which runs before the
	// scripts). Events are dispatched on the main thread during the system's update, so they arrive before the scripts'
	// OnUpdate of that frame, in the order their inputs happened: pointer, then navigation, then submit. Listeners added
	// while events are dispatched receive the next ones; a removed one is not called again. Listeners may use the whole
	// scene and the other services.
	class IUIEvents
	{
	public:
		virtual ~IUIEvents() = default;

		virtual UIEventListenerID AddUIEventListener(UIEventCallback callback) = 0;
		virtual bool RemoveUIEventListener(UIEventListenerID id) = 0;
	};

	// The game UI's elements for gameplay code (the script host), offered by the GameUI module's system while the scene plays.
	// Element functions fail (false, with the reason in outError when given) for an invalid entity or one without the
	// component they need: text needs a UIInputField (its text, cut to its character limit) or a UIText, the image a UIImage, the color either (the image's tint first), interactable
	// a UISelectable, the value a UISlider (its value, clamped and snapped like input) or a UIToggle (0 or 1; switching one on
	// switches the others of its group off). Values set here raise no UI events: events report the player's input.
	class IUIControl
	{
	public:
		virtual ~IUIControl() = default;

		virtual bool GetText(Entity element, std::string& outText, std::string* outError = nullptr) = 0;
		virtual bool SetText(Entity element, std::string_view text, std::string* outError = nullptr) = 0;
		virtual bool GetImage(Entity element, AssetHandle& outTexture, std::string* outError = nullptr) = 0;
		// A null handle draws the image's color alone; other handles must be textures.
		virtual bool SetImage(Entity element, AssetHandle texture, std::string* outError = nullptr) = 0;
		virtual bool GetColor(Entity element, glm::vec4& outColor, std::string* outError = nullptr) = 0;
		virtual bool SetColor(Entity element, const glm::vec4& color, std::string* outError = nullptr) = 0;
		virtual bool IsInteractable(Entity element, bool& outInteractable, std::string* outError = nullptr) = 0;
		virtual bool SetInteractable(Entity element, bool interactable, std::string* outError = nullptr) = 0;
		virtual bool GetValue(Entity element, float& outValue, std::string* outError = nullptr) = 0;
		virtual bool SetValue(Entity element, float value, std::string* outError = nullptr) = 0;

		// The selectable with the keyboard and gamepad focus, or an invalid entity.
		virtual Entity GetFocus() = 0;
		// Focuses an interactable selectable shown on a canvas that receives input (FocusChanged events follow), or clears the
		// focus with an invalid entity.
		virtual bool SetFocus(Entity selectable, std::string* outError = nullptr) = 0;
		// Whether the pointer is over a raycast target of a canvas that receives input (as of the UI's latest update).
		virtual bool IsPointerOverUI() = 0;
		// The UI actions (Input/UIActions.h, by index: Submit, Cancel, Up, Down, Left, Right) as of the UI's latest update;
		// false for other indices.
		virtual bool IsActionPressed(uint32_t action) = 0;
		virtual bool IsActionDown(uint32_t action) = 0;
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
