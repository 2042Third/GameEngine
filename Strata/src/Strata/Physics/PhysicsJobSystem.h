#pragma once

#include "Strata/Core/Base.h"

#include <atomic>

namespace JPH
{
	class JobSystem;
}

namespace Strata
{

	// Jobs run by a physics job system. Updated from any thread.
	struct PhysicsJobCounters
	{
		std::atomic<uint64_t> Jobs = 0;       // Every job created (each one runs exactly once)
		std::atomic<uint64_t> WorkerJobs = 0; // Jobs that ran on a JobSystem worker thread (depends on thread timing)
	};

	// Creates the job system Jolt runs its simulation jobs on. Jobs are executed by Strata's JobSystem worker pool when
	// it is initialized and inline on the stepping thread otherwise (see PhysicsJobSystem.cpp for the rationale).
	// counters, if given, counts the jobs; it must outlive the job system.
	// The returned object is only used by PhysicsWorld; it is declared here so that no engine header includes Jolt.
	Scope<JPH::JobSystem> CreatePhysicsJobSystem(PhysicsJobCounters* counters = nullptr);

}
