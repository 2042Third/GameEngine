#include <doctest/doctest.h>

#include "Strata/Core/JobSystem.h"
#include "Strata/Core/Platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace Strata;

namespace
{
	// Initializes the job system for the duration of a test.
	struct ScopedJobSystem
	{
		explicit ScopedJobSystem(uint32_t workers = 4, uint32_t ioThreads = 2)
		{
			JobSystemSpecification specification;
			specification.WorkerThreadCount = workers;
			specification.IOThreadCount = ioThreads;
			JobSystem::Init(specification);
		}

		~ScopedJobSystem()
		{
			JobSystem::Shutdown();
		}
	};
}

TEST_SUITE("Core.JobSystem")
{
	TEST_CASE("Jobs run inline when the job system is not initialized")
	{
		REQUIRE_FALSE(JobSystem::IsInitialized());
		const std::thread::id caller = std::this_thread::get_id();
		std::thread::id executor;
		JobHandle handle = JobSystem::Submit([&]() { executor = std::this_thread::get_id(); });
		CHECK(handle.IsComplete());
		CHECK(executor == caller);

		std::vector<int> values(100, 0);
		JobSystem::ParallelFor(100, 10, [&](uint32_t begin, uint32_t end)
		{
			for (uint32_t index = begin; index < end; index++)
				values[index] = 1;
		});
		CHECK(std::accumulate(values.begin(), values.end(), 0) == 100);
	}

	TEST_CASE("Submitted jobs complete on worker threads")
	{
		ScopedJobSystem jobSystem;
		CHECK(JobSystem::GetWorkerThreadCount() == 4);

		// Note: waiting threads help execute queued jobs, so jobs may also run on the waiting thread.
		std::atomic<int> counter = 0;
		std::vector<JobHandle> handles;
		for (int index = 0; index < 256; index++)
			handles.push_back(JobSystem::Submit([&]() { counter.fetch_add(1); }));
		JobSystem::WaitAll(handles);
		CHECK(counter.load() == 256);
		for (const JobHandle& handle : handles)
			CHECK(handle.IsComplete());
	}

	TEST_CASE("I/O jobs run off the worker pool")
	{
		CHECK(JobSystem::GetIOThreadCount() == 0);
		{
			// Without an I/O pool, the workers run I/O jobs.
			ScopedJobSystem withoutIOPool(3, 0);
			CHECK(JobSystem::GetIOThreadCount() == 3);
		}
		ScopedJobSystem jobSystem;
		CHECK(JobSystem::GetIOThreadCount() == 2);
		std::atomic<bool> onWorker = true;
		JobHandle handle = JobSystem::SubmitIO([&]() { onWorker = JobSystem::IsWorkerThread(); });
		handle.Wait();
		CHECK_FALSE(onWorker.load());
	}

	TEST_CASE("The background threads leave hardware threads to the frame")
	{
		// Every hardware thread but four (the main thread, the graphics driver's threads, the frame's helpers), and at least
		// half of them.
		CHECK(JobSystem::GetDefaultBackgroundThreadCount(1) == 1);
		CHECK(JobSystem::GetDefaultBackgroundThreadCount(2) == 1);
		CHECK(JobSystem::GetDefaultBackgroundThreadCount(4) == 2);
		CHECK(JobSystem::GetDefaultBackgroundThreadCount(8) == 4);
		CHECK(JobSystem::GetDefaultBackgroundThreadCount(16) == 12);
		CHECK(JobSystem::GetDefaultBackgroundThreadCount(24) == 20);
		CHECK(JobSystem::GetDefaultBackgroundThreadCount(64) == 60);

		// As applications start it: fewer background threads than hardware threads wherever there are more than two.
		JobSystem::Init(JobSystemSpecification());
		const uint32_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
		CHECK(JobSystem::GetBackgroundThreadCount() == std::min(JobSystem::GetWorkerThreadCount(), JobSystem::GetDefaultBackgroundThreadCount(hardwareThreads)));
		if (hardwareThreads > 2)
			CHECK(JobSystem::GetBackgroundThreadCount() < hardwareThreads - 1);
		JobSystem::Shutdown();
	}

	TEST_CASE("Workers and I/O threads run at the normal priority, background threads below it")
	{
		REQUIRE(Platform::GetCurrentThreadPriority() == ThreadPriority::Normal);
		ScopedJobSystem jobSystem(3, 2);
		// As many background threads as workers, unless the machine has too few hardware threads to leave the frame its own.
		const uint32_t hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
		CHECK(JobSystem::GetBackgroundThreadCount() == std::min(3u, JobSystem::GetDefaultBackgroundThreadCount(hardwareThreads)));
		CHECK_FALSE(JobSystem::IsBackgroundThread());
		// Which kind of thread ran a job, at what priority. Polled rather than waited for: a waiting thread would run the
		// job itself.
		struct Ran
		{
			bool Worker = false;
			bool Background = false;
			ThreadPriority Priority = ThreadPriority::Normal;
		};
		const auto runAndRead = [](auto submit)
		{
			Ran ran;
			std::atomic<bool> done = false;
			const JobHandle handle = submit([&]()
			{
				ran.Worker = JobSystem::IsWorkerThread();
				ran.Background = JobSystem::IsBackgroundThread();
				ran.Priority = Platform::GetCurrentThreadPriority();
				done = true;
			});
			while (!handle.IsComplete())
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			CHECK(done.load());
			return ran;
		};

		const Ran worker = runAndRead([](std::function<void()> job) { return JobSystem::Submit(std::move(job)); });
		CHECK(worker.Worker);
		CHECK_FALSE(worker.Background);
		CHECK(worker.Priority == ThreadPriority::Normal);
		const Ran io = runAndRead([](std::function<void()> job) { return JobSystem::SubmitIO(std::move(job)); });
		CHECK_FALSE(io.Worker);
		CHECK_FALSE(io.Background);
		CHECK(io.Priority == ThreadPriority::Normal);
		const Ran background = runAndRead([](std::function<void()> job) { return JobSystem::Submit(std::move(job), JobPriority::Background); });
		CHECK_FALSE(background.Worker);
		CHECK(background.Background);
		CHECK(background.Priority == ThreadPriority::BelowNormal);

		// What background work starts stays on the background threads, whatever priority it asks for. This thread polls the
		// inner job: a background job that did would hold the only background thread of a machine with three hardware
		// threads or fewer (CI's macOS runners), where the inner job could never start.
		std::atomic<bool> innerBackground = false;
		std::atomic<ThreadPriority> innerPriority = ThreadPriority::Normal;
		JobHandle inner;
		const JobHandle outer = JobSystem::Submit([&]()
		{
			inner = JobSystem::Submit([&]()
			{
				innerBackground = JobSystem::IsBackgroundThread();
				innerPriority = Platform::GetCurrentThreadPriority();
			}, JobPriority::High);
		}, JobPriority::Background);
		while (!outer.IsComplete())
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		// The outer job's completion publishes the handle it wrote.
		REQUIRE(inner.IsValid());
		while (!inner.IsComplete())
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		CHECK(innerBackground.load());
		CHECK(innerPriority.load() == ThreadPriority::BelowNormal);
		CHECK(Platform::GetCurrentThreadPriority() == ThreadPriority::Normal);

		// The priority is the calling thread's own.
		std::thread([]()
		{
			CHECK(Platform::GetCurrentThreadPriority() == ThreadPriority::Normal);
			CHECK(Platform::SetCurrentThreadPriority(ThreadPriority::BelowNormal));
			CHECK(Platform::GetCurrentThreadPriority() == ThreadPriority::BelowNormal);
		}).join();
		CHECK(Platform::GetCurrentThreadPriority() == ThreadPriority::Normal);
	}

	TEST_CASE("Jobs may wait on other jobs without deadlocking")
	{
		// With a single worker, the outer job can only finish because Wait() helps run the inner job.
		ScopedJobSystem jobSystem(1, 0);
		std::atomic<int> result = 0;
		JobHandle outer = JobSystem::Submit([&]()
		{
			JobHandle inner = JobSystem::Submit([&]() { result.fetch_add(1); });
			inner.Wait();
			result.fetch_add(10);
		});
		outer.Wait();
		CHECK(result.load() == 11);
	}

	TEST_CASE("A thread that waits for a job runs that job, never other queued ones")
	{
		const std::thread::id caller = std::this_thread::get_id();
		std::atomic<int> awaitedRuns = 0;
		std::atomic<bool> awaitedRanHere = false;
		std::atomic<bool> longRanHere = false;
		{
			// The only worker is held, so everything queued stays queued until the test lets it go.
			ScopedJobSystem jobSystem(1, 0);
			std::atomic<bool> release = false;
			std::atomic<bool> holding = false;
			JobSystem::Submit([&]()
			{
				holding = true;
				while (!release.load())
					std::this_thread::sleep_for(std::chrono::microseconds(100));
			});
			while (!holding.load())
				std::this_thread::sleep_for(std::chrono::microseconds(100));

			// Queued first: a long job the waiting thread must leave to the workers.
			const JobHandle longJob = JobSystem::Submit([&]() { longRanHere = std::this_thread::get_id() == caller; }, JobPriority::High);
			const JobHandle awaited = JobSystem::Submit([&]()
			{
				awaitedRuns++;
				awaitedRanHere = std::this_thread::get_id() == caller;
			}, JobPriority::Low);
			awaited.Wait();
			CHECK(awaitedRanHere.load());
			CHECK_FALSE(longJob.IsComplete());

			release = true;
			while (!longJob.IsComplete())
				std::this_thread::sleep_for(std::chrono::microseconds(100));
			// Shutdown runs what is still queued: the worker only drops the job this thread ran.
		}
		CHECK_FALSE(longRanHere.load());
		CHECK(awaitedRuns.load() == 1);
	}

	TEST_CASE("ParallelFor runs no other queued job on the calling thread")
	{
		for (const JobPriority helpers : { JobPriority::High, JobPriority::Background })
		{
			CAPTURE(static_cast<int>(helpers));
			ScopedJobSystem jobSystem(1, 0);
			// Hold the pool the helpers go to, then queue a long job in front of them there.
			std::atomic<bool> release = false;
			std::atomic<bool> holding = false;
			const bool background = helpers == JobPriority::Background;
			const JobHandle hold = JobSystem::Submit([&]()
			{
				holding = true;
				while (!release.load())
					std::this_thread::sleep_for(std::chrono::microseconds(100));
			}, background ? JobPriority::Background : JobPriority::Normal);
			while (!holding.load())
				std::this_thread::sleep_for(std::chrono::microseconds(100));
			const std::thread::id caller = std::this_thread::get_id();
			std::atomic<bool> longRanHere = false;
			const JobHandle longJob = JobSystem::Submit([&]() { longRanHere = std::this_thread::get_id() == caller; },
				background ? JobPriority::Background : JobPriority::High);

			// The helper cannot start while the pool is held: the calling thread takes every batch and returns.
			std::atomic<uint32_t> batchesHere = 0;
			std::atomic<uint32_t> batchesElsewhere = 0;
			JobSystem::ParallelFor(64, 1, [&](uint32_t, uint32_t)
			{
				(std::this_thread::get_id() == caller ? batchesHere : batchesElsewhere)++;
			}, helpers);
			CHECK(batchesHere.load() == 64);
			CHECK(batchesElsewhere.load() == 0);
			CHECK_FALSE(longJob.IsComplete());

			release = true;
			while (!longJob.IsComplete() || !hold.IsComplete())
				std::this_thread::sleep_for(std::chrono::microseconds(100));
			CHECK_FALSE(longRanHere.load());
		}
	}

	TEST_CASE("ParallelFor waits for the batches its helpers run, also on the background threads")
	{
		ScopedJobSystem jobSystem(4, 1);
		for (const JobPriority helpers : { JobPriority::High, JobPriority::Background })
		{
			// Slow batches, so helpers are still running when the calling thread has taken the last one.
			constexpr uint32_t count = 64;
			std::vector<std::atomic<int>> visits(count);
			std::atomic<uint32_t> backgroundBatches = 0;
			std::atomic<uint32_t> belowNormalBatches = 0;
			JobSystem::ParallelFor(count, 1, [&](uint32_t begin, uint32_t end)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				if (JobSystem::IsBackgroundThread())
					backgroundBatches++;
				if (Platform::GetCurrentThreadPriority() == ThreadPriority::BelowNormal)
					belowNormalBatches++;
				for (uint32_t index = begin; index < end; index++)
					visits[index].fetch_add(1);
			}, helpers);
			bool allOnce = true;
			for (const std::atomic<int>& visit : visits)
				allOnce &= visit.load() == 1;
			CHECK(allOnce);
			// Frame work (the default helpers) never runs below the normal priority.
			if (helpers != JobPriority::Background)
			{
				CHECK(backgroundBatches.load() == 0);
				CHECK(belowNormalBatches.load() == 0);
			}
		}

		// A ParallelFor of background work uses the background threads, whatever it asks for, also when the thread that waits
		// for the background job runs it itself.
		std::atomic<uint32_t> workerBatches = 0;
		JobSystem::Submit([&]()
		{
			JobSystem::ParallelFor(64, 1, [&](uint32_t, uint32_t)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				if (JobSystem::IsWorkerThread())
					workerBatches++;
			});
		}, JobPriority::Background).Wait();
		CHECK(workerBatches.load() == 0);
	}

	TEST_CASE("A background job that a waiting thread runs keeps what it starts on the background threads")
	{
		ScopedJobSystem jobSystem(1, 0);
		// Both pools held: whatever the job queues stays queued where it went.
		std::atomic<bool> release = false;
		std::atomic<uint32_t> holding = 0;
		const auto hold = [&]()
		{
			holding++;
			while (!release.load())
				std::this_thread::sleep_for(std::chrono::microseconds(100));
		};
		const JobHandle holdWorker = JobSystem::Submit(hold);
		const JobHandle holdBackground = JobSystem::Submit(hold, JobPriority::Background);
		while (holding.load() < 2)
			std::this_thread::sleep_for(std::chrono::microseconds(100));

		uint64_t workerQueued = 0;
		uint64_t backgroundQueued = 0;
		const JobHandle job = JobSystem::Submit([&]()
		{
			JobSystem::ParallelFor(2, 1, [&](uint32_t begin, uint32_t)
			{
				// The first batch is the calling thread's, while the helper for the second is queued.
				if (begin == 0)
				{
					const JobSystemStats stats = JobSystem::GetStats();
					workerQueued = stats.Workers.QueuedJobs;
					backgroundQueued = stats.Background.QueuedJobs;
				}
			});
			const JobHandle started = JobSystem::Submit([]() {});
			CHECK(JobSystem::GetStats().Background.QueuedJobs >= 1);
			CHECK(JobSystem::GetStats().Workers.QueuedJobs == 0);
			started.Wait();
		}, JobPriority::Background);
		job.Wait(); // The background thread is held: this thread runs it
		CHECK(job.IsComplete());
		CHECK(workerQueued == 0);
		CHECK(backgroundQueued >= 1);

		release = true;
		holdWorker.Wait();
		holdBackground.Wait();
	}

	TEST_CASE("A throwing ParallelFor batch is contained and the others still run")
	{
		ScopedJobSystem jobSystem;
		std::atomic<uint32_t> batches = 0;
		JobSystem::ParallelFor(32, 1, [&](uint32_t begin, uint32_t)
		{
			batches++;
			if (begin == 7)
				throw std::runtime_error("batch failure");
		});
		CHECK(batches.load() == 32);
	}

	TEST_CASE("ParallelFor covers every index exactly once")
	{
		ScopedJobSystem jobSystem;
		constexpr uint32_t count = 100003;
		std::vector<std::atomic<int>> visits(count);
		JobSystem::ParallelFor(count, 64, [&](uint32_t begin, uint32_t end)
		{
			for (uint32_t index = begin; index < end; index++)
				visits[index].fetch_add(1);
		});

		bool allOnce = true;
		for (const std::atomic<int>& visit : visits)
			allOnce &= visit.load() == 1;
		CHECK(allOnce);
	}

	TEST_CASE("A throwing job is contained and still completes")
	{
		ScopedJobSystem jobSystem;
		JobHandle handle = JobSystem::Submit([]() { throw std::runtime_error("job failure"); });
		handle.Wait();
		CHECK(handle.IsComplete());

		std::atomic<bool> ran = false;
		JobSystem::Submit([&]() { ran = true; }).Wait();
		CHECK(ran.load());
	}

	TEST_CASE("Nothing of a submitted job stays behind in Submit: what it captured is released where it runs")
	{
		// Scene::ReleaseInBackground relies on it: the job's capture is the scene's last reference, so the scene is destroyed
		// on the thread that runs the job. libc++ copies a callable that fits std::function's own storage (a capture of one
		// shared pointer does) when it moves it, and a copy left in Submit's parameter would hold a reference until Submit
		// returned. A pool that has stopped runs a job inside Submit, where such a copy would still exist: a worker job submits
		// background jobs until the background pool, which Shutdown stops first, runs one in place. The jobs reach what they
		// observe through a static, so that each captures nothing but the pointer it counts.
		struct Observation
		{
			std::thread::id Submitter;
			std::atomic<bool> InsideSubmit = false;
			std::atomic<long> UseCount = -1; // Of the capture, seen by a job run inside Submit
		};
		static Observation s_Observation;
		s_Observation.InsideSubmit = false;
		s_Observation.UseCount = -1;
		{
			ScopedJobSystem jobSystem(1, 0);
			JobSystem::Submit([]()
			{
				s_Observation.Submitter = std::this_thread::get_id();
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
				while (s_Observation.UseCount.load() < 0 && std::chrono::steady_clock::now() < deadline)
				{
					auto capture = std::make_shared<int>(0);
					s_Observation.InsideSubmit = true;
					const JobHandle handle = JobSystem::Submit([capture = std::move(capture)]()
					{
						if (s_Observation.InsideSubmit.load() && std::this_thread::get_id() == s_Observation.Submitter)
							s_Observation.UseCount = capture.use_count();
					}, JobPriority::Background);
					s_Observation.InsideSubmit = false;
					// Run by the background pool, or here if it never started it (the pool stopping meanwhile).
					JobSystem::Wait(handle);
				}
			});
		}
		CHECK(s_Observation.UseCount.load() == 1);
	}

	TEST_CASE("Shutdown runs every queued job")
	{
		std::atomic<int> counter = 0;
		{
			ScopedJobSystem jobSystem(1, 1);
			for (int index = 0; index < 64; index++)
			{
				JobSystem::Submit([&]()
				{
					std::this_thread::sleep_for(std::chrono::microseconds(100));
					counter.fetch_add(1);
				}, JobPriority::Low);
				JobSystem::SubmitIO([&]() { counter.fetch_add(1); });
			}
		}
		CHECK(counter.load() == 128);
		CHECK_FALSE(JobSystem::IsInitialized());
	}
}
