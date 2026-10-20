#pragma once

#include "Strata/Core/Base.h"

#include <atomic>
#include <chrono>
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
		Low,
		// Long work no frame waits for (the texture compression of imports, a game export's pack compression): runs on the
		// background threads, below the normal thread priority, so that it never takes the processors from the frame's
		// threads or from the frame work of the workers. Work it starts (ParallelFor, Submit) stays on the background
		// threads. I/O jobs (SubmitIO) only queue it last, like Low.
		Background
	};

	namespace Detail
	{
		// A submitted job. Whoever claims it first runs it: the thread of its pool that takes it from the queue, or a thread
		// that waits for it (JobSystem::Wait); the other one leaves it alone.
		struct JobState
		{
			std::function<void()> Function;
			bool Background = false; // Background work, also where a waiting thread runs it: what it starts stays background
			std::atomic<bool> Claimed = false;
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
		uint32_t WorkerThreadCount = 0;     // 0 = hardware threads - 1 (at least 1)
		uint32_t IOThreadCount = 2;         // 0 = I/O jobs run on the worker pool
		uint32_t BackgroundThreadCount = 0; // 0 = as many as worker threads, at most JobSystem::GetDefaultBackgroundThreadCount
	};

	// The work of one thread pool since JobSystem::Init: what its own threads ran (a job that a waiting thread runs itself,
	// see JobSystem::Wait, counts for that thread instead).
	struct JobPoolStats
	{
		uint32_t Threads = 0;
		uint32_t RunningJobs = 0;  // Jobs the pool's threads are running now
		uint64_t QueuedJobs = 0;   // Jobs waiting for a thread (a job a waiting thread ran may count until a thread drops it)
		uint64_t CompletedJobs = 0;
		// Time the pool's threads spent running jobs, the running ones up to SampleTime included: over an interval, the
		// difference divided by the interval and the thread count is how busy the pool was.
		std::chrono::nanoseconds BusyTime { 0 };
	};

	struct JobSystemStats
	{
		JobPoolStats Workers;
		JobPoolStats IO; // No threads when I/O jobs run on the worker pool (JobSystemSpecification::IOThreadCount 0)
		JobPoolStats Background;
		std::chrono::steady_clock::time_point SampleTime;
	};

	// Thread pools for CPU work, blocking I/O and long background work. Jobs submitted before Init (or after Shutdown) run
	// inline on the calling thread, so engine code never needs to special-case single-threaded operation.
	//
	// Workers and I/O threads run at the normal thread priority, like the main thread: frame work runs on the workers
	// (box3d's tasks, transform propagation, the renderer's extraction), and a frame that waits for a job a worker has started
	// can only go on once that worker got the processor back. Background threads run below it (Platform's
	// ThreadPriority::BelowNormal): a pool as large as the processors busy with long jobs would otherwise share them evenly
	// with the frame's threads and stretch frames by whole scheduling quanta.
	//
	// A thread that waits for a job runs that job itself if no thread has started it yet (Wait), so waiting from inside a
	// job cannot deadlock the pool; it never runs other jobs meanwhile, which could hold it far longer than what it waits for.
	class JobSystem
	{
	public:
		static void Init(const JobSystemSpecification& specification = {});
		// Stops accepting jobs, runs every job already queued, then joins all threads.
		static void Shutdown();
		static bool IsInitialized();

		// Queues a job on the worker pool, or on the background threads with JobPriority::Background (always from background
		// work: a background thread's job, or a background job a waiting thread runs; what background work starts stays
		// background).
		static JobHandle Submit(std::function<void()> job, JobPriority priority = JobPriority::Normal);
		static JobHandle SubmitIO(std::function<void()> job, JobPriority priority = JobPriority::Normal);

		// Returns once the job completed. A job no thread has started yet runs on the calling thread.
		static void Wait(const JobHandle& handle);
		static void WaitAll(std::span<const JobHandle> handles);

		// Invokes function(begin, end) over [0, count) in batches of at least minBatchSize, on the calling thread and on
		// helpers of the worker pool, or of the background threads with JobPriority::Background (always from background
		// work). Returns once every batch has completed. The calling thread runs batches itself until none is left and then
		// waits only for the batches helpers are running: helpers that had not started by then never run, and the calling
		// thread never runs other queued jobs. A batch that throws is contained like a throwing job (logged; the rest of its
		// indices are skipped).
		static void ParallelFor(uint32_t count, uint32_t minBatchSize, const std::function<void(uint32_t begin, uint32_t end)>& function,
			JobPriority helperPriority = JobPriority::High);

		// The background pool's size when the specification leaves it to the system: the hardware threads but
		// c_FrameHardwareThreads, and at least half of them (at least 1). Below the normal priority alone was not enough:
		// with every processor running background work (an import compressing a 4096x4096 texture to BC7), a thread of the
		// normal priority that became ready (the graphics driver's, which the frame's vkBeginCommandBuffer waited for) waited
		// up to a scheduling quantum for a processor, which held frames for 5-27 ms; with processors left to the frame's
		// threads it runs at once.
		static uint32_t GetDefaultBackgroundThreadCount(uint32_t hardwareThreads);
		// Hardware threads the default background pool leaves to the main thread, the graphics driver's threads and the
		// frame's helpers.
		static constexpr uint32_t c_FrameHardwareThreads = 4;

		static uint32_t GetWorkerThreadCount();
		// Threads that run I/O jobs: the I/O pool's, or the worker pool's when there is no I/O pool (0 before Init).
		static uint32_t GetIOThreadCount();
		static uint32_t GetBackgroundThreadCount();
		static bool IsWorkerThread();
		// Whether the calling thread is one of the background threads (JobPriority::Background).
		static bool IsBackgroundThread();
		static uint64_t GetPendingJobCount();
		// Counters of the pools, read without stopping them (each thread's counters are consistent; the pools' jobs keep
		// running meanwhile). All zero before Init and after Shutdown. Any thread.
		static JobSystemStats GetStats();
	};

}
