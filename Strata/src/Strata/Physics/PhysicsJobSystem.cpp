#include "stpch.h"
#include "Strata/Physics/PhysicsJobSystem.h"

#include "Strata/Core/JobSystem.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/FixedSizeFreeList.h>
#include <Jolt/Core/JobSystemWithBarrier.h>
#include <Jolt/Physics/PhysicsSettings.h>

#include <deque>

namespace Strata
{

	namespace
	{

		// Runs Jolt's jobs on Strata's JobSystem.
		//
		// Why this design: PhysicsSystem::Update builds a graph of short jobs and then blocks the calling thread in
		// WaitForJobs until the graph is done. Running those jobs on the engine's worker pool (instead of a second pool such
		// as JPH::JobSystemThreadPool) keeps the process at one worker thread per core, so physics, transform propagation
		// and asset jobs never oversubscribe the CPU. Deriving from JPH::JobSystemWithBarrier reuses Jolt's barrier, whose
		// waiting thread executes any job of the barrier that has not started yet. Workers are therefore only an
		// opportunity for parallelism, never a dependency: if every worker is busy with unrelated work, the stepping thread
		// runs the jobs itself and the step still completes.
		//
		// Ready jobs go into a queue owned by this object, drained by at most one Strata task per worker thread, rather
		// than one Strata task per Jolt job: queued tasks keep their job alive, and with one task per job the stepping
		// thread can run the jobs itself faster than busy workers retire the leftover tasks, so finished jobs pile up until
		// the job pool is exhausted. The queue is flushed after every barrier wait instead, which bounds the live jobs to
		// one step's worth.
		//
		// Without an initialized JobSystem (tests, tools) jobs execute inline as soon as they become ready, exactly like
		// JPH::JobSystemSingleThreaded. Jolt produces bit-identical results for any degree of parallelism.
		class StrataJoltJobSystem final : public JPH::JobSystemWithBarrier
		{
		public:
			StrataJoltJobSystem(JPH::uint maxJobs, JPH::uint maxBarriers, PhysicsJobCounters* counters)
				: JPH::JobSystemWithBarrier(maxBarriers), m_Counters(counters)
			{
				m_Jobs.Init(maxJobs, maxJobs);
			}

			~StrataJoltJobSystem() override
			{
				// Drain tasks reference this object; let them finish (waiting helps execute queued tasks, so this cannot
				// stall on a busy pool), then release whatever is left in the queue before the job storage goes away.
				std::vector<::Strata::JobHandle> drainTasks;
				{
					std::scoped_lock<std::mutex> lock(m_QueueMutex);
					drainTasks = std::move(m_DrainTasks);
					m_DrainTasks.clear();
				}
				::Strata::JobSystem::WaitAll(drainTasks);
				FlushQueue();
			}

			int GetMaxConcurrency() const override
			{
				// The thread waiting on the barrier executes jobs as well.
				if (!::Strata::JobSystem::IsInitialized())
					return 1;
				return static_cast<int>(::Strata::JobSystem::GetWorkerThreadCount()) + 1;
			}

			JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& function, JPH::uint32 dependencyCount) override
			{
				if (!m_Counters)
					return CreateStoredJob(name, color, function, dependencyCount);

				m_Counters->Jobs.fetch_add(1, std::memory_order_relaxed);
				// Without workers every job runs on the stepping thread, so there is nothing more to count.
				if (!::Strata::JobSystem::IsInitialized())
					return CreateStoredJob(name, color, function, dependencyCount);

				std::atomic<uint64_t>* workerJobs = &m_Counters->WorkerJobs;
				const JobFunction counted = [workerJobs, function]()
				{
					if (::Strata::JobSystem::IsWorkerThread())
						workerJobs->fetch_add(1, std::memory_order_relaxed);
					function();
				};
				return CreateStoredJob(name, color, counted, dependencyCount);
			}

			void WaitForJobs(Barrier* barrier) override
			{
				JPH::JobSystemWithBarrier::WaitForJobs(barrier);

				// Every job of the barrier has run, possibly on this thread, so queue entries left behind are finished jobs
				// that only hold a reference.
				FlushQueue();
			}
		protected:
			void QueueJob(Job* job) override
			{
				if (!::Strata::JobSystem::IsInitialized())
				{
					job->Execute();
					return;
				}

				job->AddRef(); // Released once the job has been taken from the queue and executed
				bool startDrainTask = false;
				{
					std::scoped_lock<std::mutex> lock(m_QueueMutex);
					m_ReadyJobs.push_back(job);
					if (m_ActiveDrainTasks < ::Strata::JobSystem::GetWorkerThreadCount())
					{
						m_ActiveDrainTasks++;
						startDrainTask = true;
					}
				}

				if (startDrainTask)
				{
					::Strata::JobHandle task = ::Strata::JobSystem::Submit([this]() { DrainQueue(); }, JobPriority::High);
					std::scoped_lock<std::mutex> lock(m_QueueMutex);
					m_DrainTasks.erase(std::remove_if(m_DrainTasks.begin(), m_DrainTasks.end(), [](const ::Strata::JobHandle& handle) { return handle.IsComplete(); }), m_DrainTasks.end());
					m_DrainTasks.push_back(std::move(task));
				}
			}

			void QueueJobs(Job** jobs, JPH::uint jobCount) override
			{
				for (JPH::uint index = 0; index < jobCount; index++)
					QueueJob(jobs[index]);
			}

			void FreeJob(Job* job) override
			{
				m_Jobs.DestructObject(job);
			}
		private:
			JobHandle CreateStoredJob(const char* name, JPH::ColorArg color, const JobFunction& function, JPH::uint32 dependencyCount)
			{
				JPH::uint32 index = m_Jobs.ConstructObject(name, color, this, function, dependencyCount);
				if (index == JobStorage::cInvalidObjectIndex)
				{
					// Every job slot is in use: wait for running jobs to finish and free theirs. Jolt never needs more
					// than JPH::cMaxPhysicsJobs jobs at once, so this only happens with a misconfigured world.
					ST_CORE_ERROR("Physics: the job pool is exhausted; waiting for running jobs");
					while ((index = m_Jobs.ConstructObject(name, color, this, function, dependencyCount)) == JobStorage::cInvalidObjectIndex)
						std::this_thread::yield();
				}

				Job* job = &m_Jobs.Get(index);
				JobHandle handle(job); // Keeps the job alive while it is queued and possibly completes right away
				if (dependencyCount == 0)
					QueueJob(job);
				return handle;
			}

			// Runs on a Strata worker: executes queued jobs until the queue is empty. The task count is updated under the
			// queue lock, so a job queued after the last task saw an empty queue always starts a new task.
			void DrainQueue()
			{
				while (true)
				{
					Job* job = nullptr;
					{
						std::scoped_lock<std::mutex> lock(m_QueueMutex);
						if (m_ReadyJobs.empty())
						{
							m_ActiveDrainTasks--;
							return;
						}
						job = m_ReadyJobs.front();
						m_ReadyJobs.pop_front();
					}

					job->Execute(); // No-op if the barrier's thread already ran it
					job->Release();
				}
			}

			void FlushQueue()
			{
				std::deque<Job*> remaining;
				{
					std::scoped_lock<std::mutex> lock(m_QueueMutex);
					remaining.swap(m_ReadyJobs);
				}
				for (Job* job : remaining)
				{
					job->Execute();
					job->Release();
				}
			}
		private:
			using JobStorage = JPH::FixedSizeFreeList<Job>;

			JobStorage m_Jobs;
			std::mutex m_QueueMutex;
			std::deque<Job*> m_ReadyJobs;                  // Each entry holds a reference to its job
			uint32_t m_ActiveDrainTasks = 0;
			std::vector<::Strata::JobHandle> m_DrainTasks; // Submitted drain tasks (completed ones are pruned)
			PhysicsJobCounters* m_Counters = nullptr;
		};

	}

	Scope<JPH::JobSystem> CreatePhysicsJobSystem(PhysicsJobCounters* counters)
	{
		return CreateScope<StrataJoltJobSystem>(static_cast<JPH::uint>(JPH::cMaxPhysicsJobs), static_cast<JPH::uint>(JPH::cMaxPhysicsBarriers), counters);
	}

}
