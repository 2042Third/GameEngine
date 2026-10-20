#include "stpch.h"
#include "Strata/Physics/PhysicsRuntime.h"

#include "Strata/Core/Assert.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/RegisterTypes.h>

#include <box3d/box3d.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string_view>

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

		// Jolt reports problems it recovers from (e.g. full contact caches) through Trace; it may be called from Jolt's
		// worker threads, which is fine because the engine loggers are thread-safe.
		void JoltTrace(const char* format, ...)
		{
			char message[1024];
			va_list arguments;
			va_start(arguments, format);
			std::vsnprintf(message, sizeof(message), format, arguments);
			va_end(arguments);
			ST_CORE_WARN("Jolt: {}", message);
		}

#ifdef JPH_ENABLE_ASSERTS
		// Jolt asserts at the end of a step that the step had no errors, but those errors (full body pair and contact buffers)
		// are returned to the caller too, which handles them: PhysicsWorld counts them, warns once and sizes the scene's next
		// world for them. Escalating the assertion would turn a recoverable overflow into an abort in builds with asserts.
		bool IsHandledUpdateErrorAssertion(const char* expression, const char* file)
		{
			return expression && file && std::string_view(expression) == "errors == EPhysicsUpdateError::None" && std::string_view(file).ends_with("PhysicsSystem.cpp");
		}

		// Routes Jolt's internal asserts through the engine's assert handling (logging, debugger break, test handlers).
		bool JoltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line)
		{
			if (IsHandledUpdateErrorAssertion(expression, file))
				return false;
			const AssertAction action = Detail::HandleAssertFailure(true, false, expression, file, static_cast<int>(line), message ? std::string(message) : std::string());
			return action == AssertAction::Break;
		}
#endif

	}

	void PhysicsRuntime::Acquire()
	{
		std::call_once(s_Box3DHooks, []()
		{
			b3SetAssertFcn(&Box3DAssertFailed);
			b3SetLogFcn(&Box3DLog);
		});

		std::scoped_lock<std::mutex> lock(s_RuntimeMutex);
		if (s_ReferenceCount++ > 0)
			return;

		JPH::RegisterDefaultAllocator();
		JPH::Trace = &JoltTrace;
		JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = &JoltAssertFailed;)
		JPH::Factory::sInstance = new JPH::Factory();
		JPH::RegisterTypes();
	}

	void PhysicsRuntime::Release()
	{
		std::scoped_lock<std::mutex> lock(s_RuntimeMutex);
		ST_CORE_ASSERT(s_ReferenceCount > 0, "PhysicsRuntime::Release called without a matching Acquire");
		if (s_ReferenceCount == 0 || --s_ReferenceCount > 0)
			return;

		JPH::UnregisterTypes();
		delete JPH::Factory::sInstance;
		JPH::Factory::sInstance = nullptr;
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
