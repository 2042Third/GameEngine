#pragma once

#include "Strata/Core/Base.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <span>

namespace Strata
{

	enum class JobPriority : uint8_t
	{
		High = 0,
		Normal,
		Low
	};

	namespace Detail
	{
		struct JobState
		{
			std::atomic<bool> Completed = false;
			std::mutex Mutex;
			std::condition_variable Condition;
		};
	}

	// Handle to a submitted job. A default-constructed handle refers to no job and counts as complete.
	class JobHandle
	{
	public:
		JobHandle() = default;

		bool IsValid() const { return m_State != nullptr; }
		bool IsComplete() const { return !m_State || m_State->Completed.load(std::memory_order_acquire); }
		void Wait() const;
	private:
		explicit JobHandle(Ref<Detail::JobState> state)
			: m_State(std::move(state))
		{
		}

		Ref<Detail::JobState> m_State;

		friend class JobSystem;
	};

	struct JobSystemSpecification
	{
		uint32_t WorkerThreadCount = 0; // 0 = hardware threads - 1 (at least 1)
		uint32_t IOThreadCount = 2;     // 0 = I/O jobs run on the worker pool
	};

	// Thread pools for CPU work and blocking I/O. Jobs submitted before Init (or after Shutdown) run inline
	// on the calling thread, so engine code never needs to special-case single-threaded operation.
	// Waiting threads help execute queued CPU jobs, so waiting from inside a job cannot deadlock the pool.
	class JobSystem
	{
	public:
		static void Init(const JobSystemSpecification& specification = {});
		// Stops accepting jobs, runs every job already queued, then joins all threads.
		static void Shutdown();
		static bool IsInitialized();

		static JobHandle Submit(std::function<void()> job, JobPriority priority = JobPriority::Normal);
		static JobHandle SubmitIO(std::function<void()> job, JobPriority priority = JobPriority::Normal);

		static void Wait(const JobHandle& handle);
		static void WaitAll(std::span<const JobHandle> handles);

		// Invokes function(begin, end) over [0, count) in batches of at least minBatchSize, using the worker
		// pool plus the calling thread. Returns once every batch has completed.
		static void ParallelFor(uint32_t count, uint32_t minBatchSize, const std::function<void(uint32_t begin, uint32_t end)>& function);

		static uint32_t GetWorkerThreadCount();
		// Threads that run I/O jobs: the I/O pool's, or the worker pool's when there is no I/O pool (0 before Init).
		static uint32_t GetIOThreadCount();
		static bool IsWorkerThread();
		static uint64_t GetPendingJobCount();
	};

}
