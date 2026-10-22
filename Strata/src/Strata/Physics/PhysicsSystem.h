#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Core/UUID.h"
#include "Strata/Physics/PhysicsTypes.h"
#include "Strata/Physics/PhysicsWorld.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SceneSystem.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class Scene;
	struct SceneWorkBudget;

	// The physics world PhysicsSystem::PrepareLoadedScene built for a loaded scene before it runs (Scene::GetPreparedState):
	// the system adopts it when the scene starts.
	struct PreparedPhysicsWorld
	{
		Scope<PhysicsWorld> World;
	};

	// Rigid body physics for a running scene (Play and Simulate modes).
	//
	// The simulation (a PhysicsWorld, see there for how components map to bodies) is created when the scene starts running
	// with physics components, or as soon as the first one is added, so scenes without physics cost nothing. The scene's
	// gravity and fixed timestep drive it: each fixed update steps the world once (the first steps are held while mesh
	// colliders wait for their meshes, see PhysicsWorld). Component edits (EnTT
	// on_construct/on_update/on_destroy; property edits must emit on_update, e.g. through Entity::MarkModified or
	// ComponentAccess), entity destruction and (de)activation are applied before the next update, body API call or query.
	// Transforms changed outside physics are applied to the bodies at the next fixed step, so queries made in between see the
	// bodies where they were after the last step (Teleport moves a body immediately). Signal transform edits too (sleeping
	// and static bodies only follow signaled changes, see PhysicsWorld).
	//
	// Other systems reach it through the scene services it registers while it exists (Scene/SceneServices.h):
	// ICollisionEventSource, IBodyMotionQuery, IBodyControl, IPhysicsQueries, ICharacterControl and IPhysicsCapacity. Collision events are collected during each
	// step and dispatched afterwards on the main thread to the registered listeners (see ICollisionEventSource, which also
	// says which contacts are tracked). Listeners may use the whole scene and physics API, including destroying entities;
	// End events caused by changes made outside a step are dispatched after the next update.
	//
	// Main thread only. Functions return false / zero / empty results while there is no simulation.
	class PhysicsSystem : public SceneSystem, public ICollisionEventSource, public IBodyMotionQuery, public IBodyControl, public IPhysicsQueries,
		public ICharacterControl, public IPhysicsCapacity
	{
	public:
		explicit PhysicsSystem(Scene& scene);
		~PhysicsSystem() override;

		PhysicsSystem(const PhysicsSystem&) = delete;
		PhysicsSystem& operator=(const PhysicsSystem&) = delete;

		// Process-wide settings (capacities, layer matrix) copied by physics systems created afterwards (tests and tools; games
		// set capacities per scene, SceneSettings::PhysicsCapacity, which win over these).
		static void SetDefaultSettings(const PhysicsSettings& settings);
		static const PhysicsSettings& GetDefaultSettings();

		// What the worlds of a scene (Scene::GetLineageID: its scene asset, or the scene and its copies) needed beyond their
		// capacities in this process (remembered when they stop): a world created for the scene later sizes its automatic
		// capacities for at least that much. Scenes that only share a name share nothing.
		static PhysicsCapacityDemand GetRememberedCapacityDemand(UUID sceneLineage);
		static void ForgetCapacityDemands();

		// The physics module's preparation of a loaded scene (SceneSystemDescriptor::PrepareLoadedScene): creates the world the
		// scene's system will run (PreparedPhysicsWorld, Sliced) and builds its bodies in slices of c_PrepareBodiesPerSlice
		// owners, in parallel, until the budget's time is up (at least one slice per call); true once every body is built.
		// Scenes without physics components need nothing. OnRuntimeStart adopts the world, so the first frame builds no body.
		static bool PrepareLoadedScene(Scene& scene, SceneWorkBudget& budget);
		static constexpr uint32_t c_PrepareBodiesPerSlice = 8192;

		void OnRuntimeStart() override;
		void OnRuntimeStop() override;
		void OnUpdate(Timestep timestep) override;
		void OnFixedUpdate(float timestep) override;

		bool IsRunning() const { return m_Running; }
		// The simulation backing this system; nullptr while not running or while the scene has no physics components.
		PhysicsWorld* GetWorld() { return m_World.get(); }
		const PhysicsWorld* GetWorld() const { return m_World.get(); }

		//////////////////////////////////////////////////////////////////////////
		// Collision events (ICollisionEventSource)
		//////////////////////////////////////////////////////////////////////////

		CollisionListenerID AddCollisionListener(CollisionCallback callback, CollisionListenerScope scope = CollisionListenerScope::AllEntities) override;
		bool RemoveCollisionListener(CollisionListenerID id) override;
		void WatchCollisions(UUID entity) override;
		void UnwatchCollisions(UUID entity) override;
		bool IsWatchingCollisions(UUID entity) const;

		//////////////////////////////////////////////////////////////////////////
		// World
		//////////////////////////////////////////////////////////////////////////

		// Sets the scene's gravity (SceneSettings::Gravity) and applies it immediately. Non-finite values are rejected.
		bool SetGravity(const glm::vec3& gravity);
		glm::vec3 GetGravity() const;
		// Whether two layers collide: both the scene's copy of the project's layer matrix (Scene::GetPhysicsLayers) and the
		// default settings' (GetDefaultSettings, all layers collide unless a program changes them) must allow it.
		// SetLayersCollide changes the scene's copy, so the change is what the scene shows (for as long as it runs: the copy is
		// not saved), and applies it to the running simulation at once; changes of the copy made elsewhere (the project's
		// table applied again, Scene::ApplyProjectLayers) apply at the next fixed step.
		void SetLayersCollide(uint32_t layerA, uint32_t layerB, bool collide);
		bool DoLayersCollide(uint32_t layerA, uint32_t layerB) const;
		PhysicsStats GetStats() const;

		//////////////////////////////////////////////////////////////////////////
		// Body API (IBodyMotionQuery, IBodyControl; see PhysicsWorld)
		//////////////////////////////////////////////////////////////////////////

		bool HasBody(Entity entity) override;
		Entity GetBodyEntity(Entity entity);
		glm::vec3 GetLinearVelocity(Entity entity) override;
		bool SetLinearVelocity(Entity entity, const glm::vec3& velocity) override;
		glm::vec3 GetAngularVelocity(Entity entity) override;
		bool SetAngularVelocity(Entity entity, const glm::vec3& velocity) override;
		std::optional<glm::vec3> GetPointVelocity(Entity entity, const glm::vec3& worldPoint) override;
		bool AddForce(Entity entity, const glm::vec3& force) override;
		bool AddForceAtPosition(Entity entity, const glm::vec3& force, const glm::vec3& worldPosition) override;
		bool AddImpulse(Entity entity, const glm::vec3& impulse) override;
		bool AddImpulseAtPosition(Entity entity, const glm::vec3& impulse, const glm::vec3& worldPosition) override;
		bool AddTorque(Entity entity, const glm::vec3& torque) override;
		bool AddAngularImpulse(Entity entity, const glm::vec3& impulse) override;
		bool SetGravityScale(Entity entity, float gravityScale) override;
		bool IsSleeping(Entity entity) override;
		Entity GetSimulatingBody(Entity entity) override;
		bool IsDynamicBody(Entity entity) override;
		bool WakeUp(Entity entity) override;
		bool Teleport(Entity entity, const glm::vec3& position, const glm::quat& rotation) override;

		//////////////////////////////////////////////////////////////////////////
		// Queries (IPhysicsQueries; see PhysicsWorld)
		//////////////////////////////////////////////////////////////////////////

		std::optional<RaycastHit> Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask = c_AllPhysicsLayers,
			Entity ignoreEntity = {}, bool includeTriggers = false) override;
		std::vector<RaycastHit> RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, uint32_t layerMask = c_AllPhysicsLayers,
			Entity ignoreEntity = {}, bool includeTriggers = false) override;
		std::vector<Entity> OverlapSphere(const glm::vec3& center, float radius, uint32_t layerMask = c_AllPhysicsLayers, bool includeTriggers = false) override;
		std::vector<Entity> OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation, uint32_t layerMask = c_AllPhysicsLayers,
			bool includeTriggers = false) override;

		//////////////////////////////////////////////////////////////////////////
		// Character controllers (ICharacterControl; see PhysicsWorld)
		//////////////////////////////////////////////////////////////////////////

		bool HasCharacterController(Entity entity) override;
		bool MoveCharacter(Entity entity, const glm::vec3& velocity) override;
		bool JumpCharacter(Entity entity, float speed) override;
		bool TeleportCharacter(Entity entity, const glm::vec3& position) override;
		bool AddCharacterMotion(Entity entity, const glm::vec3& displacement) override;
		std::optional<CharacterState> GetCharacterState(Entity entity) override;

		//////////////////////////////////////////////////////////////////////////
		// Capacity (IPhysicsCapacity; see PhysicsWorld::GetCapacityInfo and Reserve). A reservation made before the world
		// exists applies to it once it is created, and ends when the scene stops.
		//////////////////////////////////////////////////////////////////////////

		PhysicsCapacityInfo GetCapacityInfo() const override;
		void Reserve(uint32_t bodies) override;
	private:
		struct CollisionListener
		{
			CollisionListenerID ID = c_InvalidCollisionListener;
			CollisionCallback Callback;
			CollisionListenerScope Scope = CollisionListenerScope::AllEntities;
			bool Removed = false;
		};

		static bool HasPhysicsComponents(const Scene& scene);
		// The settings a world of the scene is created with: the default settings, the scene's capacities and its layer matrix.
		static PhysicsSettings MakeWorldSettings(const Scene& scene, const PhysicsSettings& defaults);
		bool SceneHasPhysicsComponents() const;
		void OnPhysicsComponentAdded(entt::registry& registry, entt::entity handle);
		void CreateWorld();
		// Makes `world` (created with `settings`) the system's world: gravity, layers and contact tracking follow.
		void AdoptWorld(Scope<PhysicsWorld> world, const PhysicsSettings& settings);
		// Creates the world once physics components exist, then applies the changes recorded since the last call.
		void ApplyPendingChanges();
		void SyncGravity();
		// The matrix the world uses: the scene's and the default settings' masks together.
		std::array<uint32_t, c_PhysicsLayerCount> GetEffectiveLayerMasks() const;
		// Applies the scene's layer matrix to the world when it changed (scene.setSettings, a script, the inspector).
		void SyncLayers();
		void DispatchCollisionEvents();
		// Tells the world which contacts to track: all of them while a listener of every entity is registered.
		void UpdateContactTracking();
	private:
		Scene& m_Scene;
		PhysicsSettings m_Settings;
		Scope<PhysicsWorld> m_World;
		bool m_Running = false;
		bool m_WorldRequested = false;
		bool m_ReportedNoWorld = false; // The scene found no free physics world (reported once)
		uint32_t m_ReservedBodies = 0; // See Reserve
		std::vector<entt::scoped_connection> m_CreationConnections; // Watch for the first physics component

		std::vector<Ref<CollisionListener>> m_CollisionListeners;
		CollisionListenerID m_NextListenerID = 1;
		std::unordered_map<UUID, uint32_t> m_CollisionWatches; // Watched entities and their watch counts (see WatchCollisions)
		glm::vec3 m_AppliedGravity = glm::vec3(0.0f); // Scene gravity last applied to the world
		std::array<uint32_t, c_PhysicsLayerCount> m_AppliedLayerMasks = {}; // Layer matrix last applied to the world
	};

}
