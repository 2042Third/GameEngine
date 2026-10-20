#pragma once

#include <cstdint>

namespace Strata
{

	// Process-wide use of the physics library, box3d: counted while physics worlds (and tools such as mesh cooking) use it.
	// box3d's assert and log hooks are installed by the first Acquire of the process and stay: box3d reads them as plain
	// globals, also from cook jobs, and its cooked hulls and meshes outlive every world. Its allocation stays box3d's own
	// (aligned allocation, thread-safe), so whatever it made is freed by the allocator that made it. Thread-safe.
	class PhysicsRuntime
	{
	public:
		static void Acquire();
		static void Release();
		static bool IsInitialized();
		static uint32_t GetReferenceCount();

		// box3d's failed assertions (routed through Strata's assert handling, Detail::HandleAssertFailure: logged, test
		// handlers, a debugger break; the default handler aborts) and its log messages (warnings in Strata's log, prefixed
		// "box3d: "), since the process started. Asserts are compiled out of Dist builds.
		static uint64_t GetAssertionCount();
		static uint64_t GetLogMessageCount();
	};

	// While one lives on a thread, box3d's log messages from that thread are dropped: for calls whose failure the caller
	// expects and handles, such as building a convex hull with a vertex count box3d may refuse (see PhysicsMeshShapes).
	class Box3DLogSuppression
	{
	public:
		Box3DLogSuppression();
		~Box3DLogSuppression();

		Box3DLogSuppression(const Box3DLogSuppression&) = delete;
		Box3DLogSuppression& operator=(const Box3DLogSuppression&) = delete;
	};

	// Holds a PhysicsRuntime reference for its lifetime.
	class PhysicsRuntimeReference
	{
	public:
		PhysicsRuntimeReference() { PhysicsRuntime::Acquire(); }
		~PhysicsRuntimeReference() { PhysicsRuntime::Release(); }

		PhysicsRuntimeReference(const PhysicsRuntimeReference&) = delete;
		PhysicsRuntimeReference& operator=(const PhysicsRuntimeReference&) = delete;
	};

}
