#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Physics/PhysicsTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Strata
{

	// Shapes cooked from mesh collision data: the bounding volume trees of triangle meshes and convex hulls, the expensive
	// part of building mesh colliders. They are cached process-wide per data object for as long as the object lives, so
	// that worlds created later (every Play or Simulate start) reuse them instead of cooking again; when the object is
	// destroyed (its mesh was unloaded or reloaded), its shapes are dropped. The cache holds the shapes in Jolt's binary
	// form, plain memory that does not depend on the lifetime of the Jolt runtime, and worlds restore them, which only
	// copies the data. Cooking runs on the JobSystem (inline without one) and never blocks the main thread: colliders wait
	// for it like for mesh data that is still loading.
	//
	// The class is internal to the physics module (no engine header includes it). Thread-safe.
	class PhysicsMeshShapes
	{
	public:
		enum class State : uint8_t
		{
			Cooking,
			Ready,
			Failed
		};

		struct Result
		{
			State CookState = State::Cooking;
			Ref<const std::vector<uint8_t>> Shape; // Ready: the shape in Jolt's binary state format
			std::string Error;                     // Failed: why the data cannot be used
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
