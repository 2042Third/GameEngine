#include "stpch.h"
#include "Strata/Physics/PhysicsRuntime.h"

#include "Strata/Core/Assert.h"

#include <box3d/box3d.h>

#include <atomic>
#include <mutex>

namespace Strata
{

	namespace
	{

		std::mutex s_RuntimeMutex;
		uint32_t s_ReferenceCount = 0;
		std::once_flag s_Box3DHooks;
		std::atomic<uint64_t> s_Box3DAssertions = 0;
		std::atomic<uint64_t> s_Box3DLogMessages = 0;
		thread_local uint32_t t_Box3DLogSuppression = 0;

		// box3d logs problems it recovers from (a hull it cannot build, a full mesh contact buffer); it may log from any
		// thread, which is fine because the engine loggers are thread-safe.
		void Box3DLog(const char* message)
		{
			if (t_Box3DLogSuppression > 0)
				return;
			s_Box3DLogMessages.fetch_add(1, std::memory_order_relaxed);
			ST_CORE_WARN("box3d: {}", message ? message : "");
		}

		// Routes box3d's asserts through the engine's assert handling (logging, debugger break, test handlers): the default
		// handler aborts, so box3d never continues past a failed invariant. Non-zero makes box3d break into the debugger.
		int Box3DAssertFailed(const char* condition, const char* file, int line)
		{
			s_Box3DAssertions.fetch_add(1, std::memory_order_relaxed);
			const AssertAction action = Detail::HandleAssertFailure(true, false, condition ? condition : "", file ? file : "", line, "box3d assertion");
			return action == AssertAction::Break ? 1 : 0;
		}

	}

	void PhysicsRuntime::Acquire()
	{
		std::call_once(s_Box3DHooks, []()
		{
			b3SetAssertFcn(&Box3DAssertFailed);
			b3SetLogFcn(&Box3DLog);
		});

		std::scoped_lock<std::mutex> lock(s_RuntimeMutex);
		s_ReferenceCount++;
	}

	void PhysicsRuntime::Release()
	{
		std::scoped_lock<std::mutex> lock(s_RuntimeMutex);
		ST_CORE_ASSERT(s_ReferenceCount > 0, "PhysicsRuntime::Release called without a matching Acquire");
		if (s_ReferenceCount > 0)
			s_ReferenceCount--;
	}

	bool PhysicsRuntime::IsInitialized()
	{
		std::scoped_lock<std::mutex> lock(s_RuntimeMutex);
		return s_ReferenceCount > 0;
	}

	uint32_t PhysicsRuntime::GetReferenceCount()
	{
		std::scoped_lock<std::mutex> lock(s_RuntimeMutex);
		return s_ReferenceCount;
	}

	uint64_t PhysicsRuntime::GetAssertionCount()
	{
		return s_Box3DAssertions.load(std::memory_order_relaxed);
	}

	uint64_t PhysicsRuntime::GetLogMessageCount()
	{
		return s_Box3DLogMessages.load(std::memory_order_relaxed);
	}

	Box3DLogSuppression::Box3DLogSuppression()
	{
		t_Box3DLogSuppression++;
	}

	Box3DLogSuppression::~Box3DLogSuppression()
	{
		t_Box3DLogSuppression--;
	}

}
