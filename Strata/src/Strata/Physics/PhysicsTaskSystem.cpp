#include "stpch.h"
#include "Strata/Physics/PhysicsTaskSystem.h"

#include "Strata/Core/Assert.h"

#include <box3d/box3d.h>

#include <algorithm>

namespace Strata
{

	struct PhysicsTaskSystem::Callbacks
	{
		// Starts one box3d task. Returns the slot finishTask waits on, or null when the task ran here already (box3d then
		// does not call finishTask for it).
		static void* Enqueue(b3TaskCallback* task, void* taskContext, void* userContext, const char*)
		{
			PhysicsTaskSystem& system = *static_cast<PhysicsTaskSystem*>(userContext);
			PhysicsTaskCounters* counters = system.m_Counters;
			if (counters)
				counters->Tasks.fetch_add(1, std::memory_order_relaxed);
			Task* slot = JobSystem::IsInitialized() ? system.AcquireTask() : nullptr;
			if (!slot)
			{
				task(taskContext);
				return nullptr;
			}

			slot->Handle = JobSystem::Submit([task, taskContext, counters]()
			{
				if (counters && JobSystem::IsWorkerThread())
					counters->WorkerTasks.fetch_add(1, std::memory_order_relaxed);
				task(taskContext);
			}, JobPriority::High);
			return slot;
		}

		// Blocks until a task started by Enqueue is done: it runs on this thread if no worker has started it yet.
		static void Finish(void* userTask, void* userContext)
		{
			PhysicsTaskSystem& system = *static_cast<PhysicsTaskSystem*>(userContext);
			Task* slot = static_cast<Task*>(userTask);
			slot->Handle.Wait();
			slot->Handle = JobHandle();
			system.ReleaseTask(slot);
		}
	};

	PhysicsTaskSystem::PhysicsTaskSystem(PhysicsTaskCounters* counters)
		: m_Counters(counters)
	{
	}

	PhysicsTaskSystem::~PhysicsTaskSystem()
	{
		// Every task a step started was finished before the step returned.
		ST_CORE_ASSERT(m_FreeTasks.size() == m_Tasks.size(), "PhysicsTaskSystem destroyed with {} unfinished tasks", m_Tasks.size() - m_FreeTasks.size());
	}

	void PhysicsTaskSystem::Configure(b3WorldDef& definition)
	{
		definition.workerCount = GetWorkerCount();
		definition.enqueueTask = &Callbacks::Enqueue;
		definition.finishTask = &Callbacks::Finish;
		definition.userTaskContext = this;
	}

	uint32_t PhysicsTaskSystem::GetWorkerCount()
	{
		if (!JobSystem::IsInitialized())
			return 1;
		return std::min<uint32_t>(JobSystem::GetWorkerThreadCount() + 1, B3_MAX_WORKERS);
	}

	PhysicsTaskSystem::Task* PhysicsTaskSystem::AcquireTask()
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		if (m_FreeTasks.empty())
		{
			// box3d starts at most B3_MAX_TASKS tasks in a step; slots beyond would only come from a misbehaving caller, whose
			// tasks then run inline.
			if (m_Tasks.size() >= B3_MAX_TASKS)
				return nullptr;
			m_Tasks.push_back(CreateScope<Task>());
			return m_Tasks.back().get();
		}
		Task* task = m_FreeTasks.back();
		m_FreeTasks.pop_back();
		return task;
	}

	void PhysicsTaskSystem::ReleaseTask(Task* task)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		m_FreeTasks.push_back(task);
	}

}
