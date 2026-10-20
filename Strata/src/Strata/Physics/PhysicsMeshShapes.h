#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Physics/PhysicsTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// box3d's cooked geometry (box3d/types.h); declared here so that no engine header includes box3d.
struct b3HullData;
struct b3MeshData;

namespace Strata
{

	// A shape cooked from mesh collision data: a convex hull or a triangle mesh with its bounding volume tree, owned here and
	// freed when the last reference goes. box3d's mesh shapes keep a pointer to their mesh data for their whole life, so every
	// body whose shapes use one holds a reference (see PhysicsWorld); hull shapes copy their hull into the world.
	class PhysicsCookedShape
	{
	public:
		PhysicsCookedShape(b3HullData* hull, b3MeshData* mesh);
		~PhysicsCookedShape();

		PhysicsCookedShape(const PhysicsCookedShape&) = delete;
		PhysicsCookedShape& operator=(const PhysicsCookedShape&) = delete;

		const b3HullData* GetHull() const { return m_Hull; } // Convex: the hull; else null
		const b3MeshData* GetMesh() const { return m_Mesh; } // Triangles: the mesh; else null

		// Cooked shapes alive in the process (held by the cache or by bodies), for tests.
		static uint64_t GetLiveCount();
	private:
		b3HullData* m_Hull = nullptr;
		b3MeshData* m_Mesh = nullptr;
	};

	// Shapes cooked from mesh collision data, the expensive part of building mesh colliders. They are cached process-wide per
	// data object for as long as the object lives, so that worlds created later (every Play or Simulate start) reuse them
	// instead of cooking again; when the object is destroyed (its mesh was unloaded or reloaded), the cache drops its
	// reference, and bodies that still use the shape keep it until they are rebuilt or destroyed. Cooking runs on the
	// JobSystem (inline without one) and never blocks the main thread: colliders wait for it like for mesh data that is still
	// loading.
	//
	// Convex hulls: box3d's hulls have at most 128 vertices, faces and edges each (B3_MAX_HULL_*), and a hull of V vertices
	// made of triangles has 2V - 4 faces and 3V - 6 edges, so a round mesh fails at 128 vertices. A hull is cooked with at
	// most c_PreferredHullVertices vertices and, if box3d refuses it, with c_SafeHullVertices, which any hull fits; box3d's
	// message about the refused attempt is suppressed (Box3DLogSuppression). Only a hull that fails at that count (degenerate
	// points) is unusable.
	//
	// The class is internal to the physics module (no engine header includes it). Thread-safe.
	class PhysicsMeshShapes
	{
	public:
		static constexpr int c_PreferredHullVertices = 64;
		static constexpr int c_SafeHullVertices = 44;

		enum class State : uint8_t
		{
			Cooking,
			Ready,
			Failed
		};

		struct Result
		{
			State CookState = State::Cooking;
			Ref<const PhysicsCookedShape> Shape; // Ready: the cooked shape
			std::string Error;                   // Failed: why the data cannot be used
		};

		// The shape of mesh data as a convex hull (positions only) or as a triangle mesh, cooking it if it is not cached.
		static Result Request(const Ref<const PhysicsMeshData>& data, bool convex);
		// Incremented whenever a cook finishes (successfully or not).
		static uint64_t GetCompletedCount();

		// Cooks started since the process started, and shapes held by the cache (those of destroyed data are dropped when
		// the next shape is cooked).
		static uint64_t GetCookCount();
		static size_t GetCachedCount();
	};

}
