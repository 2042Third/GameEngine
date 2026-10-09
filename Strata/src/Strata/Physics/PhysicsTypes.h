#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/Base.h"
#include "Strata/Core/UUID.h"
#include "Strata/Scene/Entity.h"

#include <glm/glm.hpp>

#include <array>
#include <functional>
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

	enum class CollisionEventType : uint8_t
	{
		Begin = 0, // Two bodies started touching (or a body entered a trigger)
		End        // They stopped touching, or one of them was removed from the simulation while touching
	};

	// A change in the contact state of two entities. Every Begin is eventually followed by exactly one End for the same
	// pair, except when the scene stops running (the world is torn down without events).
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

	struct RaycastHit
	{
		Entity HitEntity;
		UUID EntityID = UUID::Null();
		glm::vec3 Point = glm::vec3(0.0f);  // World space hit position
		glm::vec3 Normal = glm::vec3(0.0f); // World space surface normal at the hit
		float Distance = 0.0f;              // Distance from the ray origin along the ray direction
	};

	struct PhysicsStats
	{
		uint32_t BodyCount = 0;          // Bodies in the simulation (inactive entities' bodies are excluded)
		uint32_t StaticBodyCount = 0;
		uint32_t DynamicBodyCount = 0;
		uint32_t KinematicBodyCount = 0;
		uint32_t ActiveBodyCount = 0;    // Awake bodies (sleeping and static bodies are not simulated)
		uint32_t PendingBodyCount = 0;   // Entities waiting for a body that cannot be built yet (degenerate transform, mesh not loaded, world full)
		uint32_t ContactPairCount = 0;   // Pairs of entities currently touching (including triggers)
		uint64_t StepCount = 0;          // Simulation steps since the world was created
		uint64_t WorkerJobCount = 0;     // Simulation jobs executed by JobSystem worker threads since the world was created
		float LastStepTime = 0.0f;       // Wall time of the last step in milliseconds, including transform synchronization
	};

	// Triangle data used to build MeshColliderComponent shapes.
	struct PhysicsMeshData
	{
		std::vector<glm::vec3> Positions; // Mesh space vertex positions
		std::vector<uint32_t> Indices;    // Triangle list (three indices per triangle, counter-clockwise front faces)
	};

	// Returns the collision data of a mesh asset, or nullptr if it is unknown or not loaded. Returning the same object
	// for the same data lets physics worlds reuse the shapes built from it.
	using PhysicsMeshProvider = std::function<Ref<const PhysicsMeshData>(AssetHandle mesh)>;

}
