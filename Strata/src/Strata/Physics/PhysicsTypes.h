#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Scene/SceneServices.h"

#include <glm/glm.hpp>

#include <array>
#include <vector>

namespace Strata
{

	// Number of user collision layers; RigidBodyComponent::Layer ranges from 0 to c_PhysicsLayerCount - 1.
	constexpr uint32_t c_PhysicsLayerCount = 32;
	// Query layer mask that accepts every layer.
	constexpr uint32_t c_AllPhysicsLayers = 0xFFFFFFFFu;

	// Configuration of a physics world. Capacities are fixed when the world is created; the layer matrix can also be
	// changed at runtime through PhysicsWorld::SetLayersCollide.
	struct PhysicsSettings
	{
		uint32_t MaxBodies = 65536;
		uint32_t MaxBodyPairs = 65536;           // Broad phase pairs that can be processed per step
		uint32_t MaxContactConstraints = 10240;  // Contacts that can be solved per step
		uint32_t CollisionSteps = 1;             // Collision sub-steps per fixed update (raise for very fast bodies)
		uint32_t TempAllocatorSize = 10u * 1024u * 1024u; // Per-step scratch memory in bytes (falls back to the heap when exceeded)
		// Longest wait, in seconds of simulation time, at the start for the meshes of mesh colliders (still loading or being
		// cooked) before the first step; 0 does not wait. See PhysicsWorld.
		float MeshWaitTimeout = 10.0f;

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

	struct PhysicsStats
	{
		uint32_t BodyCount = 0;          // Bodies in the simulation (inactive entities' bodies are excluded)
		uint32_t StaticBodyCount = 0;
		uint32_t DynamicBodyCount = 0;
		uint32_t KinematicBodyCount = 0;
		uint32_t ActiveBodyCount = 0;    // Awake bodies (sleeping and static bodies are not simulated)
		uint32_t PendingBodyCount = 0;   // Entities waiting for a body that cannot be built yet (degenerate transform, mesh not loaded, world full)
		uint32_t ContactPairCount = 0;   // Pairs of entities currently touching (including triggers)
		// Work of the last step, which grows with the awake bodies and the signaled changes, not with sleeping bodies:
		uint32_t SyncedBodyCount = 0;    // Bodies compared with their entity before the step (awake ones and those with changes)
		uint32_t CheckedPairCount = 0;   // Touching pairs that could end during the step (those of awake or rebuilt bodies)
		uint32_t WrittenBodyCount = 0;   // Dynamic bodies whose pose was written back to their entity
		uint64_t BuildCount = 0;         // Bodies built or rebuilt since the world was created (attempts that failed included)
		uint64_t MeshCheckCount = 0;     // Mesh collider bodies checked for changed, arrived or cooked mesh data since then
		uint64_t StepCount = 0;          // Simulation steps since the world was created
		bool WaitingForMeshes = false;   // The last step was held: the simulation waits for mesh colliders' meshes to start
		uint64_t HeldStepCount = 0;      // Steps held at the start while waiting for meshes (not counted in StepCount)
		uint64_t JobCount = 0;           // Simulation jobs run since the world was created, on any thread
		uint64_t WorkerJobCount = 0;     // The part of JobCount run by JobSystem worker threads (depends on thread timing)
		float LastStepTime = 0.0f;       // Wall time of the last step in milliseconds, including transform synchronization
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
