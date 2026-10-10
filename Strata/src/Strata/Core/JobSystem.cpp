#include "stpch.h"
#include "Strata/Core/JobSystem.h"

#include "Strata/Core/Platform.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <thread>

namespace Strata
{

	namespace
	{

		struct Job
		{
			std::function<void()> Function;
			Ref<Detail::JobState> State;
		};

		thread_local bool t_IsWorkerThread = false;

		void ExecuteJob(Job& job)
		{
			try
			{
				job.Function();
			}
			catch (const std::exception& exception)
			{
				ST_CORE_ERROR("JobSystem: job threw an exception: {}", exception.what());
			}
			catch (...)
			{
				ST_CORE_ERROR("JobSystem: job threw an unknown exception");
			}

			{
				std::scoped_lock<std::mutex> lock(job.State->Mutex);
				job.State->Completed.store(true, std::memory_order_release);
			}
			job.State->Condition.notify_all();
		}

		int64_t GetClockNanoseconds(std::chrono::steady_clock::time_point time)
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
		}

		// What one pool thread is doing, written by that thread and read by any (JobSystem::GetStats) without either
		// waiting: a sequence lock, odd while the thread updates the values, tells a reader whether the values it read belong
		// together.
		class ThreadActivity
		{
		public:
			struct Snapshot
			{
				int64_t BusyNanoseconds = 0;     // Jobs finished
				int64_t JobStartNanoseconds = 0; // Steady clock of the running job's start
				uint64_t CompletedJobs = 0;
				bool Running = false;
			};

			// The owning thread only.
			void BeginJob(int64_t now)
			{
				const uint64_t sequence = BeginWrite();
				m_JobStart.store(now, std::memory_order_relaxed);
				m_Running.store(true, std::memory_order_relaxed);
				EndWrite(sequence);
			}

			void EndJob(int64_t now)
			{
				const uint64_t sequence = BeginWrite();
				const int64_t start = m_JobStart.load(std::memory_order_relaxed);
				m_Busy.store(m_Busy.load(std::memory_order_relaxed) + std::max<int64_t>(now - start, 0), std::memory_order_relaxed);
				m_Completed.store(m_Completed.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
				m_Running.store(false, std::memory_order_relaxed);
				EndWrite(sequence);
			}

			// Any thread. Retries while the owner is writing, which takes a few instructions.
			Snapshot Read() const
			{
				while (true)
				{
					const uint64_t sequence = m_Sequence.load(std::memory_order_acquire);
					if ((sequence & 1u) != 0)
					{
						std::this_thread::yield();
						continue;
					}
					Snapshot snapshot;
					snapshot.BusyNanoseconds = m_Busy.load(std::memory_order_relaxed);
					snapshot.JobStartNanoseconds = m_JobStart.load(std::memory_order_relaxed);
					snapshot.CompletedJobs = m_Completed.load(std::memory_order_relaxed);
					snapshot.Running = m_Running.load(std::memory_order_relaxed);
					// The values are read before the sequence is checked again.
					std::atomic_thread_fence(std::memory_order_acquire);
					if (m_Sequence.load(std::memory_order_relaxed) == sequence)
						return snapshot;
				}
			}
		private:
			uint64_t BeginWrite()
			{
				const uint64_t sequence = m_Sequence.load(std::memory_order_relaxed);
				m_Sequence.store(sequence + 1, std::memory_order_relaxed);
				// The odd sequence becomes visible before any of the new values.
				std::atomic_thread_fence(std::memory_order_release);
				return sequence;
			}

			void EndWrite(uint64_t sequence)
			{
				m_Sequence.store(sequence + 2, std::memory_order_release);
			}
		private:
			std::atomic<uint64_t> m_Sequence = 0;
			std::atomic<int64_t> m_Busy = 0;
			std::atomic<int64_t> m_JobStart = 0;
			std::atomic<uint64_t> m_Completed = 0;
			std::atomic<bool> m_Running = false;
		};

		class JobPool
		{
		public:
			void Start(uint32_t threadCount, const std::string& namePrefix, bool workerThreads, bool lowerPriority)
			{
				m_Stopping = false;
				// Before the threads start: they write their activity from their first job on.
				m_Activity = std::make_unique<ThreadActivity[]>(threadCount);
				m_Threads.reserve(threadCount);
				for (uint32_t index = 0; index < threadCount; index++)
				{
					std::string name = fmt::format("{} {}", namePrefix, index);
					m_Threads.emplace_back([this, name = std::move(name), workerThreads, lowerPriority, index]()
					{
						Platform::SetCurrentThreadName(name);
						// A thread the system keeps at its priority still runs every job: only the frame's share of the processors
						// suffers, so this is not an error.
						if (lowerPriority && !Platform::SetCurrentThreadPriority(ThreadPriority::BelowNormal))
							ST_CORE_WARN("{} keeps the normal thread priority: the system refused to lower it", name);
						t_IsWorkerThread = workerThreads;
						ThreadMain(m_Activity[index]);
					});
				}
			}

			void Stop()
			{
				{
					std::scoped_lock<std::mutex> lock(m_Mutex);
					m_Stopping = true;
				}
				m_Condition.notify_all();
				for (std::thread& thread : m_Threads)
					thread.join();
				m_Threads.clear();

				std::scoped_lock<std::mutex> lock(m_Mutex);
				m_Stopped = true;
			}

			void Push(Job job, JobPriority priority)
			{
				{
					std::scoped_lock<std::mutex> lock(m_Mutex);
					if (!m_Stopped)
					{
						m_Queues[static_cast<size_t>(priority)].push_back(std::move(job));
						m_Condition.notify_one();
						return;
					}
				}

				// The pool's threads are gone (late submission during shutdown): run the job right here.
				ExecuteJob(job);
			}

			bool TryPop(Job& outJob)
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				return PopLocked(outJob);
			}

			uint64_t GetPendingCount()
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				uint64_t count = 0;
				for (const std::deque<Job>& queue : m_Queues)
					count += queue.size();
				return count;
			}

			uint32_t GetThreadCount() const { return static_cast<uint32_t>(m_Threads.size()); }

			JobPoolStats GetStats(int64_t now)
			{
				JobPoolStats stats;
				stats.Threads = GetThreadCount();
				stats.QueuedJobs = GetPendingCount();
				int64_t busy = 0;
				for (uint32_t index = 0; index < stats.Threads; index++)
				{
					const ThreadActivity::Snapshot activity = m_Activity[index].Read();
					busy += activity.BusyNanoseconds;
					if (activity.Running)
					{
						busy += std::max<int64_t>(now - activity.JobStartNanoseconds, 0);
						stats.RunningJobs++;
					}
					stats.CompletedJobs += activity.CompletedJobs;
				}
				stats.BusyTime = std::chrono::nanoseconds(busy);
				return stats;
			}
		private:
			bool PopLocked(Job& outJob)
			{
				for (std::deque<Job>& queue : m_Queues)
				{
					if (!queue.empty())
					{
						outJob = std::move(queue.front());
						queue.pop_front();
						return true;
					}
				}
				return false;
			}

			void ThreadMain(ThreadActivity& activity)
			{
				while (true)
				{
					Job job;
					{
						std::unique_lock<std::mutex> lock(m_Mutex);
						m_Condition.wait(lock, [this]()
						{
							if (m_Stopping)
								return true;
							for (const std::deque<Job>& queue : m_Queues)
							{
								if (!queue.empty())
									return true;
							}
							return false;
						});

						// When stopping, keep draining until every queue is empty.
						if (!PopLocked(job))
							return;
					}
					activity.BeginJob(GetClockNanoseconds(std::chrono::steady_clock::now()));
					ExecuteJob(job);
					activity.EndJob(GetClockNanoseconds(std::chrono::steady_clock::now()));
				}
			}
		private:
			std::mutex m_Mutex;
			std::condition_variable m_Condition;
			std::deque<Job> m_Queues[3];
			std::vector<std::thread> m_Threads;
			std::unique_ptr<ThreadActivity[]> m_Activity; // One per thread, indexed like m_Threads
			bool m_Stopping = false;
			bool m_Stopped = false;
		};

		struct JobSystemData
		{
			JobPool Workers;
			JobPool IO;
			bool HasIOPool = false;
		};

		// Guards initialization state; job submission itself only takes the pool locks.
		std::mutex s_LifetimeMutex;
		std::atomic<JobSystemData*> s_Data = nullptr;

		JobHandle RunInline(std::function<void()>& function)
		{
			Job job { std::move(function), CreateRef<Detail::JobState>() };
			ExecuteJob(job);
			return {};
		}

	}

	void JobHandle::Wait() const
	{
		JobSystem::Wait(*this);
	}

	void JobSystem::Init(const JobSystemSpecification& specification)
	{
		std::scoped_lock<std::mutex> lock(s_LifetimeMutex);
		if (s_Data.load())
		{
			ST_CORE_WARN("JobSystem::Init called while already initialized");
			return;
		}

		uint32_t workerCount = specification.WorkerThreadCount;
		if (workerCount == 0)
		{
			const uint32_t hardwareThreads = std::thread::hardware_concurrency();
			workerCount = hardwareThreads > 1 ? hardwareThreads - 1 : 1;
		}

		auto* data = new JobSystemData();
		data->Workers.Start(workerCount, "Strata Worker", true, specification.LowerWorkerPriority);
		if (specification.IOThreadCount > 0)
		{
			data->IO.Start(specification.IOThreadCount, "Strata IO", false, false);
			data->HasIOPool = true;
		}
		s_Data.store(data);

		ST_CORE_INFO("JobSystem: {} worker threads, {} I/O threads", workerCount, specification.IOThreadCount);
	}

	void JobSystem::Shutdown()
	{
		std::scoped_lock<std::mutex> lock(s_LifetimeMutex);
		JobSystemData* data = s_Data.load();
		if (!data)
			return;

		// I/O jobs commonly hand work to the CPU pool, so drain I/O first.
		if (data->HasIOPool)
			data->IO.Stop();
		data->Workers.Stop();
		s_Data.store(nullptr);
		delete data;
	}

	bool JobSystem::IsInitialized()
	{
		return s_Data.load() != nullptr;
	}

	JobHandle JobSystem::Submit(std::function<void()> job, JobPriority priority)
	{
		JobSystemData* data = s_Data.load();
		if (!data)
			return RunInline(job);

		auto state = CreateRef<Detail::JobState>();
		data->Workers.Push(Job { std::move(job), state }, priority);
		return JobHandle(std::move(state));
	}

	JobHandle JobSystem::SubmitIO(std::function<void()> job, JobPriority priority)
	{
		JobSystemData* data = s_Data.load();
		if (!data)
			return RunInline(job);

		auto state = CreateRef<Detail::JobState>();
		JobPool& pool = data->HasIOPool ? data->IO : data->Workers;
		pool.Push(Job { std::move(job), state }, priority);
		return JobHandle(std::move(state));
	}

	void JobSystem::Wait(const JobHandle& handle)
	{
		if (handle.IsComplete())
			return;

		const Ref<Detail::JobState>& state = handle.m_State;
		while (!state->Completed.load(std::memory_order_acquire))
		{
			// Help with queued CPU work instead of idling; this also prevents deadlocks when a job waits
			// on another job while every worker is busy.
			if (JobSystemData* data = s_Data.load())
			{
				Job job;
				if (data->Workers.TryPop(job))
				{
					ExecuteJob(job);
					continue;
				}
			}

			std::unique_lock<std::mutex> lock(state->Mutex);
			state->Condition.wait_for(lock, std::chrono::microseconds(500), [&state]()
			{
				return state->Completed.load(std::memory_order_acquire);
			});
		}
	}

	void JobSystem::WaitAll(std::span<const JobHandle> handles)
	{
		for (const JobHandle& handle : handles)
			Wait(handle);
	}

	void JobSystem::ParallelFor(uint32_t count, uint32_t minBatchSize, const std::function<void(uint32_t begin, uint32_t end)>& function)
	{
		if (count == 0)
			return;

		const uint32_t batchSize = minBatchSize > 0 ? minBatchSize : 1;
		const uint32_t batchCount = (count + batchSize - 1) / batchSize;
		JobSystemData* data = s_Data.load();
		if (!data || batchCount == 1)
		{
			function(0, count);
			return;
		}

		std::atomic<uint32_t> nextBatch = 0;
		auto runBatches = [&]()
		{
			while (true)
			{
				const uint32_t batch = nextBatch.fetch_add(1, std::memory_order_relaxed);
				if (batch >= batchCount)
					return;

				const uint32_t begin = batch * batchSize;
				const uint32_t end = std::min(begin + batchSize, count);
				function(begin, end);
			}
		};

		const uint32_t helperCount = std::min(data->Workers.GetThreadCount(), batchCount - 1);
		std::vector<JobHandle> helpers;
		helpers.reserve(helperCount);
		for (uint32_t index = 0; index < helperCount; index++)
			helpers.push_back(Submit(runBatches, JobPriority::High));

		runBatches();
		WaitAll(helpers);
	}

	uint32_t JobSystem::GetWorkerThreadCount()
	{
		JobSystemData* data = s_Data.load();
		return data ? data->Workers.GetThreadCount() : 0;
	}

	uint32_t JobSystem::GetIOThreadCount()
	{
		JobSystemData* data = s_Data.load();
		if (!data)
			return 0;
		return data->HasIOPool ? data->IO.GetThreadCount() : data->Workers.GetThreadCount();
	}

	bool JobSystem::IsWorkerThread()
	{
		return t_IsWorkerThread;
	}

	uint64_t JobSystem::GetPendingJobCount()
	{
		JobSystemData* data = s_Data.load();
		if (!data)
			return 0;
		return data->Workers.GetPendingCount() + (data->HasIOPool ? data->IO.GetPendingCount() : 0);
	}

	JobSystemStats JobSystem::GetStats()
	{
		JobSystemStats stats;
		stats.SampleTime = std::chrono::steady_clock::now();
		JobSystemData* data = s_Data.load();
		if (!data)
			return stats;
		const int64_t now = GetClockNanoseconds(stats.SampleTime);
		stats.Workers = data->Workers.GetStats(now);
		if (data->HasIOPool)
			stats.IO = data->IO.GetStats(now);
		return stats;
	}

}
