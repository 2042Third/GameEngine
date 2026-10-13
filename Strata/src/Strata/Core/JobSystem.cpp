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

		using JobRef = Ref<Detail::JobState>;

		enum class PoolKind : uint8_t
		{
			Workers = 0,
			IO,
			Background
		};

		// The pool of the thread, for IsWorkerThread, IsBackgroundThread and the work background threads start.
		thread_local PoolKind t_ThreadPool = PoolKind::IO;
		thread_local bool t_IsPoolThread = false;
		// Background jobs another thread runs while it waits for them (JobSystem::Wait): what they start stays background.
		thread_local uint32_t t_BackgroundJobDepth = 0;

		void ExecuteJob(Detail::JobState& job)
		{
			if (job.Background)
				t_BackgroundJobDepth++;
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
			if (job.Background)
				t_BackgroundJobDepth--;
			// What the job captured is released before anyone hears that it completed.
			job.Function = nullptr;

			{
				std::scoped_lock<std::mutex> lock(job.Mutex);
				job.Completed.store(true, std::memory_order_release);
			}
			job.Condition.notify_all();
		}

		// Runs the job unless another thread claimed it first. Returns whether it ran here.
		bool TryRunJob(Detail::JobState& job)
		{
			if (job.Claimed.exchange(true, std::memory_order_acq_rel))
				return false;
			ExecuteJob(job);
			return true;
		}

		// Completes a job that will never run (a ParallelFor helper that is no longer needed), unless a thread claimed it.
		void DropJob(Detail::JobState& job)
		{
			if (job.Claimed.exchange(true, std::memory_order_acq_rel))
				return;
			job.Function = nullptr;
			{
				std::scoped_lock<std::mutex> lock(job.Mutex);
				job.Completed.store(true, std::memory_order_release);
			}
			job.Condition.notify_all();
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
			void Start(uint32_t threadCount, const std::string& namePrefix, PoolKind kind)
			{
				m_Stopping = false;
				// Before the threads start: they write their activity from their first job on.
				m_Activity = std::make_unique<ThreadActivity[]>(threadCount);
				m_Threads.reserve(threadCount);
				for (uint32_t index = 0; index < threadCount; index++)
				{
					std::string name = fmt::format("{} {}", namePrefix, index);
					m_Threads.emplace_back([this, name = std::move(name), kind, index]()
					{
						Platform::SetCurrentThreadName(name);
						// A background thread the system keeps at the normal priority still runs every job: only the frame's share of
						// the processors suffers while it does, so this is not an error.
						if (kind == PoolKind::Background && !Platform::SetCurrentThreadPriority(ThreadPriority::BelowNormal))
							ST_CORE_WARN("{} keeps the normal thread priority: the system refused to lower it", name);
						t_ThreadPool = kind;
						t_IsPoolThread = true;
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

			void Push(const JobRef& job, JobPriority priority)
			{
				{
					std::scoped_lock<std::mutex> lock(m_Mutex);
					if (!m_Stopped)
					{
						m_Queues[GetQueueIndex(priority)].push_back(job);
						m_Condition.notify_one();
						return;
					}
				}

				// The pool's threads are gone (late submission during shutdown): run the job right here.
				TryRunJob(*job);
			}

			uint64_t GetPendingCount()
			{
				std::scoped_lock<std::mutex> lock(m_Mutex);
				uint64_t count = 0;
				for (const std::deque<JobRef>& queue : m_Queues)
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
			// The queues run in priority order; Background is the last (only I/O pools see it as a priority).
			static size_t GetQueueIndex(JobPriority priority)
			{
				return std::min<size_t>(static_cast<size_t>(priority), static_cast<size_t>(JobPriority::Low));
			}

			bool PopLocked(JobRef& outJob)
			{
				for (std::deque<JobRef>& queue : m_Queues)
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
					JobRef job;
					{
						std::unique_lock<std::mutex> lock(m_Mutex);
						m_Condition.wait(lock, [this]()
						{
							if (m_Stopping)
								return true;
							for (const std::deque<JobRef>& queue : m_Queues)
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
					// A job that a waiting thread ran, or a dropped helper, is only taken off the queue.
					if (job->Claimed.exchange(true, std::memory_order_acq_rel))
						continue;
					activity.BeginJob(GetClockNanoseconds(std::chrono::steady_clock::now()));
					ExecuteJob(*job);
					activity.EndJob(GetClockNanoseconds(std::chrono::steady_clock::now()));
				}
			}
		private:
			std::mutex m_Mutex;
			std::condition_variable m_Condition;
			std::deque<JobRef> m_Queues[3];
			std::vector<std::thread> m_Threads;
			std::unique_ptr<ThreadActivity[]> m_Activity; // One per thread, indexed like m_Threads
			bool m_Stopping = false;
			bool m_Stopped = false;
		};

		struct JobSystemData
		{
			JobPool Workers;
			JobPool IO;
			JobPool Background;
			bool HasIOPool = false;
		};

		// Guards initialization state; job submission itself only takes the pool locks.
		std::mutex s_LifetimeMutex;
		std::atomic<JobSystemData*> s_Data = nullptr;

		JobRef MakeJob(std::function<void()> function, bool background = false)
		{
			auto job = CreateRef<Detail::JobState>();
			job->Function = std::move(function);
			job->Background = background;
			return job;
		}

		// Whether the calling thread does background work: on a background thread, or running a background job it waited for.
		bool IsInBackgroundWork()
		{
			return (t_IsPoolThread && t_ThreadPool == PoolKind::Background) || t_BackgroundJobDepth > 0;
		}

		JobHandle RunInline(std::function<void()>& function)
		{
			const JobRef job = MakeJob(std::move(function));
			TryRunJob(*job);
			return {};
		}

		// The batches of one ParallelFor, shared by the calling thread and its helpers. Helpers keep it alive, so one that
		// starts after the call returned finds no batch left and ends without touching the function.
		struct ParallelForState
		{
			const std::function<void(uint32_t, uint32_t)>* Function = nullptr;
			uint32_t Count = 0;
			uint32_t BatchSize = 0;
			uint32_t BatchCount = 0;
			std::atomic<uint32_t> NextBatch = 0;
			std::atomic<uint32_t> FinishedBatches = 0;
			std::mutex Mutex;
			std::condition_variable Condition;
		};

		void RunBatches(ParallelForState& state)
		{
			while (true)
			{
				const uint32_t batch = state.NextBatch.fetch_add(1, std::memory_order_relaxed);
				if (batch >= state.BatchCount)
					return;

				const uint32_t begin = batch * state.BatchSize;
				const uint32_t end = std::min(begin + state.BatchSize, state.Count);
				try
				{
					(*state.Function)(begin, end);
				}
				catch (const std::exception& exception)
				{
					ST_CORE_ERROR("JobSystem: a ParallelFor batch threw an exception: {}", exception.what());
				}
				catch (...)
				{
					ST_CORE_ERROR("JobSystem: a ParallelFor batch threw an unknown exception");
				}

				// The function is not touched after the last batch is counted: the call may return from then on.
				if (state.FinishedBatches.fetch_add(1, std::memory_order_acq_rel) + 1 == state.BatchCount)
				{
					std::scoped_lock<std::mutex> lock(state.Mutex);
					state.Condition.notify_all();
				}
			}
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

		const uint32_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
		uint32_t workerCount = specification.WorkerThreadCount;
		if (workerCount == 0)
			workerCount = hardwareThreads > 1 ? hardwareThreads - 1 : 1;
		const uint32_t backgroundCount = specification.BackgroundThreadCount > 0 ? specification.BackgroundThreadCount
			: std::min(workerCount, GetDefaultBackgroundThreadCount(hardwareThreads));

		auto* data = new JobSystemData();
		data->Workers.Start(workerCount, "Strata Worker", PoolKind::Workers);
		if (specification.IOThreadCount > 0)
		{
			data->IO.Start(specification.IOThreadCount, "Strata IO", PoolKind::IO);
			data->HasIOPool = true;
		}
		data->Background.Start(backgroundCount, "Strata Background", PoolKind::Background);
		s_Data.store(data);

		ST_CORE_INFO("JobSystem: {} worker threads, {} I/O threads, {} background threads", workerCount, specification.IOThreadCount, backgroundCount);
	}

	void JobSystem::Shutdown()
	{
		std::scoped_lock<std::mutex> lock(s_LifetimeMutex);
		JobSystemData* data = s_Data.load();
		if (!data)
			return;

		// I/O jobs commonly hand work to the other pools, and background work to the workers, so drain in that order.
		if (data->HasIOPool)
			data->IO.Stop();
		data->Background.Stop();
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

		const bool background = priority == JobPriority::Background || IsInBackgroundWork();
		JobRef state = MakeJob(std::move(job), background);
		if (background)
			data->Background.Push(state, priority == JobPriority::Background ? JobPriority::Normal : priority);
		else
			data->Workers.Push(state, priority);
		return JobHandle(std::move(state));
	}

	JobHandle JobSystem::SubmitIO(std::function<void()> job, JobPriority priority)
	{
		JobSystemData* data = s_Data.load();
		if (!data)
			return RunInline(job);

		JobRef state = MakeJob(std::move(job));
		JobPool& pool = data->HasIOPool ? data->IO : data->Workers;
		pool.Push(state, priority);
		return JobHandle(std::move(state));
	}

	void JobSystem::Wait(const JobHandle& handle)
	{
		if (handle.IsComplete())
			return;

		// Nobody started it yet: run it here, which also keeps a job that waits for another from deadlocking the pool.
		Detail::JobState& state = *handle.m_State;
		if (TryRunJob(state))
			return;

		std::unique_lock<std::mutex> lock(state.Mutex);
		state.Condition.wait(lock, [&state]() { return state.Completed.load(std::memory_order_acquire); });
	}

	void JobSystem::WaitAll(std::span<const JobHandle> handles)
	{
		for (const JobHandle& handle : handles)
			Wait(handle);
	}

	void JobSystem::ParallelFor(uint32_t count, uint32_t minBatchSize, const std::function<void(uint32_t begin, uint32_t end)>& function,
		JobPriority helperPriority)
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

		const bool background = helperPriority == JobPriority::Background || IsInBackgroundWork();
		JobPool& pool = background ? data->Background : data->Workers;
		// Helpers of work in progress go before work that has not started.
		const JobPriority queuePriority = background ? JobPriority::High : helperPriority;

		auto state = CreateRef<ParallelForState>();
		state->Function = &function;
		state->Count = count;
		state->BatchSize = batchSize;
		state->BatchCount = batchCount;

		const uint32_t helperCount = std::min(pool.GetThreadCount(), batchCount - 1);
		std::vector<JobRef> helpers;
		helpers.reserve(helperCount);
		for (uint32_t index = 0; index < helperCount; index++)
		{
			JobRef helper = MakeJob([state]() { RunBatches(*state); }, background);
			pool.Push(helper, queuePriority);
			helpers.push_back(std::move(helper));
		}

		RunBatches(*state);

		// Every batch is taken: helpers that have not started would find nothing to do.
		for (const JobRef& helper : helpers)
			DropJob(*helper);

		std::unique_lock<std::mutex> lock(state->Mutex);
		state->Condition.wait(lock, [&state]() { return state->FinishedBatches.load(std::memory_order_acquire) == state->BatchCount; });
	}

	uint32_t JobSystem::GetDefaultBackgroundThreadCount(uint32_t hardwareThreads)
	{
		const uint32_t half = hardwareThreads / 2;
		const uint32_t leftToFrames = hardwareThreads > c_FrameHardwareThreads ? hardwareThreads - c_FrameHardwareThreads : 0;
		return std::max(1u, std::max(half, leftToFrames));
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

	uint32_t JobSystem::GetBackgroundThreadCount()
	{
		JobSystemData* data = s_Data.load();
		return data ? data->Background.GetThreadCount() : 0;
	}

	bool JobSystem::IsWorkerThread()
	{
		return t_IsPoolThread && t_ThreadPool == PoolKind::Workers;
	}

	bool JobSystem::IsBackgroundThread()
	{
		return t_IsPoolThread && t_ThreadPool == PoolKind::Background;
	}

	uint64_t JobSystem::GetPendingJobCount()
	{
		JobSystemData* data = s_Data.load();
		if (!data)
			return 0;
		return data->Workers.GetPendingCount() + (data->HasIOPool ? data->IO.GetPendingCount() : 0) + data->Background.GetPendingCount();
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
		stats.Background = data->Background.GetStats(now);
		return stats;
	}

}
