#include <doctest/doctest.h>

#include "Strata/Core/JobSystem.h"

#include <atomic>
#include <numeric>
#include <thread>
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
