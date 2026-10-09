#include "stpch.h"
#include "Strata/Physics/PhysicsRuntime.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <cstdio>

namespace Strata
{

	namespace
	{

		std::mutex s_RuntimeMutex;
		uint32_t s_ReferenceCount = 0;

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
		// Routes Jolt's internal asserts through the engine's assert handling (logging, debugger break, test handlers).
		bool JoltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line)
		{
			const AssertAction action = Detail::HandleAssertFailure(true, false, expression, file, static_cast<int>(line), message ? std::string(message) : std::string());
			return action == AssertAction::Break;
		}
#endif

	}

	void PhysicsRuntime::Acquire()
	{
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

}
