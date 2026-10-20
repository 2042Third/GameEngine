#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/JobSystem.h"

#include <atomic>
#include <mutex>
#include <vector>

// box3d's world definition (box3d/types.h); declared here so that no engine header includes box3d.
struct b3WorldDef;

namespace Strata
{

	// Tasks run by a physics task system. Updated from any thread.
	struct PhysicsTaskCounters
	{
		std::atomic<uint64_t> Tasks = 0;       // Every task the physics library started (each runs exactly once)
		std::atomic<uint64_t> WorkerTasks = 0; // Tasks that ran on a JobSystem worker thread (depends on thread timing)
	};

	// Runs box3d's step tasks on Strata's JobSystem.
	//
	// b3World_Step splits its stages into one task per worker (b3WorldDef::workerCount) and blocks the stepping thread on
	// each through the finish callback until it is done. Running them on the engine's worker pool, rather than box3d's own
	// scheduler threads, keeps the process at one worker thread per core. Workers are only an opportunity: the finish
	// callback waits through JobHandle::Wait, which runs a task that has not started on the waiting thread itself, so a
	// step completes even while every worker is busy with other work. Without an initialized JobSystem (tests, tools) the
	// tasks run inline when they are enqueued. box3d gives the same results for any number of workers.
	//
	// One per world. The callbacks are called by the thread that steps the world (box3d's tasks never enqueue tasks), but the
	// task slots are guarded anyway, so that nothing depends on that.
	class PhysicsTaskSystem
	{
	public:
		explicit PhysicsTaskSystem(PhysicsTaskCounters* counters = nullptr);
		~PhysicsTaskSystem();

		PhysicsTaskSystem(const PhysicsTaskSystem&) = delete;
		PhysicsTaskSystem& operator=(const PhysicsTaskSystem&) = delete;

		// Points a world definition at this task system (the task callbacks and their context) with the worker count of the
		// job system as it is now. The task system must outlive the worlds created from the definition.
		void Configure(b3WorldDef& definition);

		// The workers box3d splits a step's stages for: the JobSystem's workers and the stepping thread, at most box3d's
		// B3_MAX_WORKERS; 1 without a JobSystem.
		static uint32_t GetWorkerCount();
	private:
		struct Task
		{
			JobHandle Handle;
		};
		struct Callbacks;
		friend struct Callbacks;

		Task* AcquireTask();
		void ReleaseTask(Task* task);

		PhysicsTaskCounters* m_Counters = nullptr;
		std::mutex m_Mutex;
		std::vector<Scope<Task>> m_Tasks; // Every task slot made so far (at most the tasks box3d starts in one step)
		std::vector<Task*> m_FreeTasks;
	};

}
