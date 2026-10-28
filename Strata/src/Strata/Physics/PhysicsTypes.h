#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Scene/PhysicsLayerSettings.h"
#include "Strata/Scene/SceneServices.h"

#include <glm/glm.hpp>

#include <array>
#include <vector>

namespace Strata
{

	// c_PhysicsLayerCount (the user collision layers) comes with the layer settings (Scene/PhysicsLayerSettings.h).
	// Query layer mask that accepts every layer.
	constexpr uint32_t c_AllPhysicsLayers = 0xFFFFFFFFu;

	// Configuration of a physics world. Capacities are the room a world reserves when it is created; 0 sizes one
	// automatically from the bodies the scene has then (see PhysicsWorld::ResolveCapacities), and a scene's
	// SceneSettings::PhysicsCapacity overrides these. A world whose bodies outgrow its room grows (see PhysicsWorld,
	// "Capacities") up to GrowthLimit. The layer matrix is the scene's copy of the project's (Scene::GetPhysicsLayers,
	// which PhysicsSystem applies when it creates the world and when it changes); PhysicsWorld::SetLayerCollisionMasks changes it at runtime.
	struct PhysicsSettings
	{
		uint32_t MaxBodies = 0;                  // Bodies the world reserves room for
		uint32_t MaxContactConstraints = 0;      // Contacts the world reserves room for
		uint32_t CollisionSteps = 1;             // Simulation steps per fixed update (raise for very fast bodies)
		// Longest wait, in seconds of simulation time, at the start for the meshes of mesh colliders (still loading or being
		// cooked) before the first step; 0 does not wait. See PhysicsWorld.
		float MeshWaitTimeout = 10.0f;
		// The most bodies a world may grow to: a cap on the memory physics uses (see PhysicsWorld::EstimateReservedBytes);
		// beyond it bodies wait. 0: the engine's limit (PhysicsCapacitySettings::c_MaxBodiesLimit). Never below MaxBodies.
		uint32_t GrowthLimit = 0;

		// Layer collision matrix: bit j of LayerCollisionMasks[i] is set when layer i collides with layer j. Two layers
		// collide only if both of their masks allow it, so an asymmetric edit disables the pair. Everything collides by
		// default.
		std::array<uint32_t, c_PhysicsLayerCount> LayerCollisionMasks;

		PhysicsSettings()
		{
			LayerCollisionMasks.fill(c_AllPhysicsLayers);
		}

		// Enables or disables collisions between two layers (symmetric). Layers outside [0, 32) are ignored.
		void SetLayersCollide(uint32_t layerA, uint32_t layerB, bool collide)
		{
			if (layerA >= c_PhysicsLayerCount || layerB >= c_PhysicsLayerCount)
				return;

			if (collide)
			{
				LayerCollisionMasks[layerA] |= ST_BIT(layerB);
				LayerCollisionMasks[layerB] |= ST_BIT(layerA);
			}
			else
			{
				LayerCollisionMasks[layerA] &= ~ST_BIT(layerB);
				LayerCollisionMasks[layerB] &= ~ST_BIT(layerA);
			}
		}

		bool DoLayersCollide(uint32_t layerA, uint32_t layerB) const
		{
			if (layerA >= c_PhysicsLayerCount || layerB >= c_PhysicsLayerCount)
				return false;
			return (LayerCollisionMasks[layerA] & ST_BIT(layerB)) != 0 && (LayerCollisionMasks[layerB] & ST_BIT(layerA)) != 0;
		}
	};

	// CollisionEvent, the collision listener types and RaycastHit are defined with the scene services that hand them to other
	// systems (Scene/SceneServices.h).

	// What a world needed beyond the room it was created with: the bodies it grew to (and those waiting at its growth limit),
	// the contacts it held. PhysicsSystem remembers it per scene (Scene::GetLineageID), and the next world it creates for that
	// scene sizes its automatic room for at least this much ("the next play start reserves the room").
	struct PhysicsCapacityDemand
	{
		uint32_t Bodies = 0;
		uint32_t ContactConstraints = 0;

		bool IsEmpty() const { return Bodies == 0 && ContactConstraints == 0; }
		bool operator==(const PhysicsCapacityDemand& other) const = default;
	};

	// The physics library's own stage times of a fixed step, in milliseconds: box3d's b3World_GetProfile read after each of its
	// steps and summed over the fixed step's box3d steps. Two levels, as box3d measures them: the stages, whose sum is the
	// library step (up to box3d's own small overhead), and the parts of Solve, which are measured inside it and never added to
	// it. SplitIslands runs in a task beside the solve and is added to nothing.
	struct PhysicsLibraryProfile
	{
		// The stages
		float Pairs = 0.0f;        // Finding new pairs in the broad phase
		float Collide = 0.0f;      // The narrow phase: contact manifolds (contacts recycled skip it)
		float Solve = 0.0f;        // Solving, integrating, islands, transforms and the broad phase's refit
		float Sensors = 0.0f;      // Sensor overlaps
		// Parts of Solve
		float Constraints = 0.0f;  // The constraint solver's stages
		float Transforms = 0.0f;   // Finalizing the bodies' transforms
		float Refit = 0.0f;        // Refitting the broad phase's trees
		float Continuous = 0.0f;   // Continuous collision of fast bodies (box3d's bullets)
		float SleepIslands = 0.0f; // Putting islands to sleep
		// Beside the solve
		float SplitIslands = 0.0f;
	};

	// box3d's counters after a step (b3World_GetCounters): its islands, its contacts (touching or not), the awake ones (as of
	// after the step) and the contacts its collide pass recycled. box3d resets its recycled count only in a collide pass, which
	// runs only while some contact is awake, so after a step that left no contact awake it still holds the last pass's count:
	// that step counts 0 recycled (exact for that case; it misses only the contacts recycled in the step in which the last
	// island fell asleep).
	struct PhysicsLibraryCounters
	{
		uint32_t Islands = 0;
		uint32_t Contacts = 0;
		uint32_t AwakeContacts = 0;
		uint32_t RecycledContacts = 0;
	};

	struct PhysicsStats
	{
		uint32_t BodyCount = 0;          // Bodies in the simulation (inactive entities' bodies are excluded)
		uint32_t StaticBodyCount = 0;
		uint32_t DynamicBodyCount = 0;
		uint32_t KinematicBodyCount = 0;
		uint32_t ActiveBodyCount = 0;    // Awake bodies (sleeping and static bodies are not simulated), character controllers' presence bodies included
		uint32_t PendingBodyCount = 0;   // Entities waiting for a body that cannot be built yet (degenerate transform, mesh not loaded, growth limit)
		uint32_t CharacterControllerCount = 0; // Character controllers in the simulation (their presence bodies are not in BodyCount)
		uint32_t ContactPairCount = 0;   // Touching pairs of entities that are tracked (those someone listens to, see ICollisionEventSource)
		uint64_t PairIndexOperations = 0; // Entries of the per-entity touching pair lists written since the world was created
		// The room of the world (bodies: as it grew since it was created; contacts: as reserved), its growth limit, and what
		// it needed beyond the room it was created with.
		uint32_t MaxBodies = 0;
		uint32_t MaxContactConstraints = 0;
		uint32_t GrowthLimit = 0;
		uint32_t BodyOverflowCount = 0;  // Entities waiting for a body at the growth limit (part of PendingBodyCount)
		PhysicsCapacityDemand Demand;    // What the world needed beyond its room so far (empty while it sufficed)
		// Work of the last step, which grows with the awake bodies and the signaled changes, not with sleeping bodies or the
		// world's capacity:
		uint32_t SyncedBodyCount = 0;    // Bodies compared with their entity before the step (awake ones and those with changes)
		uint32_t CheckedPairCount = 0;   // Touching pairs of awake bodies, whose points the step refreshed
		uint32_t WrittenBodyCount = 0;   // Dynamic bodies whose pose was written back to their entity
		// Body slots read to put the moved bodies in body order for the write-back: the groups of 64 that hold a moved body.
		uint32_t OrderedSlotCount = 0;
		uint64_t BuildCount = 0;         // Bodies built or rebuilt since the world was created (attempts that failed included)
		uint64_t AddBatchCount = 0;      // Rounds of changes that created bodies since then, one per kind (static, moving) they created
		uint32_t SharedShapeCount = 0;   // Box hulls the world made, each shared by the bodies of its size and placement
		uint64_t MeshCheckCount = 0;     // Mesh collider bodies checked for changed, arrived or cooked mesh data since then
		uint64_t StepCount = 0;          // Simulation steps since the world was created
		uint64_t DroppedSteps = 0;       // Fixed steps the scene dropped since it started (Scene::GetDroppedFixedStepCount)
		bool WaitingForMeshes = false;   // The last step was held: the simulation waits for mesh colliders' meshes to start
		uint64_t HeldStepCount = 0;      // Steps held at the start while waiting for meshes (not counted in StepCount)
		uint64_t JobCount = 0;           // Simulation jobs run since the world was created, on any thread
		uint64_t WorkerJobCount = 0;     // The part of JobCount run by JobSystem worker threads (depends on thread timing)
		float LastStepTime = 0.0f;       // Wall time of the last step in milliseconds, including transform synchronization
		// The parts of LastStepTime, in milliseconds: the physics library's own step (wall time of its step calls), and
		// Strata's work around it (bringing the bodies in line with their entities, writing simulated poses back, turning
		// contacts into events).
		float LibraryStepTime = 0.0f;
		float SyncTime = 0.0f;           // Includes CharacterTime
		float WriteBackTime = 0.0f;
		float ContactTime = 0.0f;
		float CharacterTime = 0.0f;      // Moving the character controllers (see CharacterControllerSet)
		// Growth (see PhysicsWorld, "Capacities"): how often the world outgrew its room since it was created, and its last
		// growth (wall time in milliseconds of the round of changes that grew it, the room before and after).
		uint32_t GrowthCount = 0;
		float LastGrowthMs = 0.0f;
		uint32_t LastGrowthFrom = 0;
		uint32_t LastGrowthTo = 0;
		uint64_t ParallelBuildCount = 0; // Rounds of changes since the world was created that built their bodies on the job threads
		// The scene's last update (see PhysicsWorld::GetCapacityInfo): bodies built or rebuilt, bodies destroyed, and the
		// milliseconds spent applying changes (building and destroying bodies, a growth).
		uint32_t BodiesAddedLastUpdate = 0;
		uint32_t BodiesRemovedLastUpdate = 0;
		float ChangeTime = 0.0f;
		// Tree queries that woke the bodies around moved static bodies since the world was created (box3d wakes the bodies
		// touching a body it removes, destroys or that is teleported through its contacts).
		uint64_t WakeQueryCount = 0;
		// The slices a loaded scene's world was built in while its load prepared it, before the scene ran
		// (PhysicsSystem::PrepareLoadedScene); 0 for a world built when its scene started.
		uint32_t PreparedSlices = 0;

		// The instruments (DeveloperGuide "Physics instruments"), kept as the world steps, so that IPhysicsDiagnostics reads
		// them in constant time. Of the last fixed step unless stated.
		// Bodies: the moving bodies in the simulation (dynamic and kinematic bodies of entities in the simulation, and character
		// controllers' presence bodies, as ActiveBodyCount counts them), and those of them asleep.
		uint32_t MovingBodyCount = 0;
		uint32_t SleepingBodyCount = 0;
		// box3d's counters (see PhysicsLibraryCounters; RecycledContactCount summed over the fixed step's box3d steps), read
		// after every box3d step only while DetailedStats is on (PhysicsWorld::SetDetailedStats: box3d counts them together with
		// its memory, which walks every island); 0 otherwise.
		bool DetailedStats = false;
		uint32_t IslandCount = 0;
		uint32_t ContactCount = 0;
		uint32_t AwakeContactCount = 0;
		uint32_t RecycledContactCount = 0;
		PhysicsLibraryProfile LibraryProfile; // box3d's stage times
		uint32_t LibraryWorkers = 0;          // The workers box3d splits its stages for (b3World_GetWorkerCount)
		// The most contacts box3d held at the start of a step so far (b3World_GetMaxCapacity: a peak, not its storage), the
		// steps after which that peak passed the contact room box3d reserved or its next doubling (box3d's contact storage grew
		// while playing), and the library time of the last such step.
		uint32_t LibraryContactPeak = 0;
		uint32_t ContactRoomGrowths = 0;
		float LastContactGrowthStepMs = 0.0f;
		// How the dynamic bodies the step wrote back settle: the fastest one's speed (m/s), those faster than their sleep
		// threshold, and the RMS of the step's displacement of the others, in millimeters. Speeds are displacements over the
		// fixed step, a proxy: box3d's sleep test uses its own sleep velocity (linear speed plus the angular arc over the body's
		// extent), so a body counted settling may still fall asleep.
		float MaxBodySpeed = 0.0f;
		uint32_t SettlingBodyCount = 0;
		float RestingJitter = 0.0f;
		// Parts of Strata's work, in milliseconds: ApplyTime (changes and growth at the step's start) and CheckedPairTime are
		// parts of SyncTime, EventTime (reading box3d's events) is a part of ContactTime, and InstrumentTime is what reading
		// box3d's profile and counters cost the step (also part of LastStepTime, in none of the other parts).
		float ApplyTime = 0.0f;
		float CheckedPairTime = 0.0f;
		float EventTime = 0.0f;
		float InstrumentTime = 0.0f;
		// Since the world was created: the steps' wall time and the library's, in milliseconds (tools difference two reads).
		double TotalStepMs = 0.0;
		double TotalLibraryMs = 0.0;
	};

	// Triangle data used to build MeshColliderComponent shapes.
	struct PhysicsMeshData
	{
		std::vector<glm::vec3> Positions; // Mesh space vertex positions
		std::vector<uint32_t> Indices;    // Triangle list (three indices per triangle, counter-clockwise front faces)
	};

	// Supplies the triangle data of the meshes that MeshColliderComponents reference. Called on the main thread. The default
	// provider reads the meshes of the active asset manager (see AssetMeshProvider).
	class PhysicsMeshProvider
	{
	public:
		virtual ~PhysicsMeshProvider() = default;

		// The collision data of a mesh asset, or nullptr while it is not available (unknown, or still loading: this must
		// not block). Return the same object for as long as the mesh does not change: the shapes cooked from mesh data are
		// cached process-wide per data object (for as long as the object lives, so that worlds created later reuse them),
		// and colliders are rebuilt when a mesh's data object changes (hot reload).
		virtual Ref<const PhysicsMeshData> GetMeshData(AssetHandle mesh) = 0;
		// Like GetMeshData, but never starts loading a mesh: physics uses it to check whether the meshes of built colliders
		// changed, so that a mesh unloaded on purpose is not loaded again (the colliders keep their shape).
		virtual Ref<const PhysicsMeshData> PeekMeshData(AssetHandle mesh) { return GetMeshData(mesh); }
		// Whether the data of a mesh that GetMeshData does not return yet is on its way (loading), as opposed to unknown or
		// unusable: before its first step, the simulation waits for such meshes (see PhysicsSettings::MeshWaitTimeout).
		virtual bool IsMeshLoading([[maybe_unused]] AssetHandle mesh) { return false; }
		// Changes whenever GetMeshData may return a different result than before for some mesh (data that finished
		// loading, was reloaded or was dropped). Physics asks for mesh data again only when it changes.
		virtual uint64_t GetVersion() = 0;
		// Appends the meshes for which GetMeshData may return a different result than when GetVersion returned `version`
		// (other handles may be included), so that physics checks only the colliders using them. Returns false if the
		// provider cannot tell; then physics checks every mesh collider.
		virtual bool GetChangedMeshes([[maybe_unused]] uint64_t version, [[maybe_unused]] std::vector<AssetHandle>& outMeshes) { return false; }
	};

}
