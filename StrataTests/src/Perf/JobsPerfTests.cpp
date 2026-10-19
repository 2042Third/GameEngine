#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Strata/Core/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// The job system's own costs, so that its next version is decided on numbers: what a job costs to submit and wait for,
// what ParallelFor costs around its batches, and how long a parked worker takes to start a job. Every job is empty.
// Under the application's job system (Perf::ScopedApplicationJobSystem), as the engine runs it.
namespace
{

	constexpr uint32_t c_SubmitWaitBatch = 1000;
	constexpr uint32_t c_SubmitManyJobs = 100000;
	constexpr uint32_t c_ParallelForItemsPerWorker = 64;
	constexpr auto c_ParkingIdle = std::chrono::milliseconds(50);
	constexpr uint32_t c_WakeSamples = 100;

	double ElapsedUs(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
	{
		return std::chrono::duration<double, std::micro>(end - start).count();
	}

}

TEST_SUITE("Perf.Jobs")
{
	TEST_CASE("Submitting a job and waiting for it costs microseconds")
	{
		Perf::ScopedApplicationJobSystem jobs;
		// Per Submit and Wait from the main thread, as code that hands one piece of work to the pool and needs its result
		// does: the median of 30 batches of 1,000.
		Perf::CheckBudgetConfirmed("Perf.Jobs.SubmitWait", [&]
		{
			const Perf::Measurement batches = Perf::Measure("Perf.Jobs.SubmitWait (1000 jobs)", 3, 30, [&]
			{
				for (uint32_t index = 0; index < c_SubmitWaitBatch; index++)
					JobSystem::Wait(JobSystem::Submit([] {}));
			});
			return batches.MedianMs * 1000.0 / c_SubmitWaitBatch;
		});
	}

	TEST_CASE("Submitting many jobs and waiting for them all costs a fraction of a microsecond per job")
	{
		Perf::ScopedApplicationJobSystem jobs;
		std::vector<JobHandle> handles;
		handles.reserve(c_SubmitManyJobs);
		std::atomic<uint32_t> ran = 0;
		// Per job: 100,000 submitted at once, then WaitAll.
		Perf::CheckBudgetConfirmed("Perf.Jobs.SubmitMany", [&]
		{
			const Perf::Measurement rounds = Perf::Measure("Perf.Jobs.SubmitMany (100000 jobs)", 1, 9, [&]
			{
				handles.clear();
				for (uint32_t index = 0; index < c_SubmitManyJobs; index++)
					handles.push_back(JobSystem::Submit([&ran] { ran.fetch_add(1, std::memory_order_relaxed); }));
				JobSystem::WaitAll(handles);
			});
			return rounds.MedianMs * 1000.0 / c_SubmitManyJobs;
		});
		CHECK(ran.load() % c_SubmitManyJobs == 0);
		CHECK(ran.load() >= c_SubmitManyJobs * 10);
	}

	TEST_CASE("A ParallelFor of empty batches costs microseconds per call")
	{
		Perf::ScopedApplicationJobSystem jobs;
		const uint32_t count = std::max(JobSystem::GetWorkerThreadCount(), 1u) * c_ParallelForItemsPerWorker;
		std::atomic<uint64_t> items = 0;
		// Per call over workers x 64 items in batches of one: the pool's cost of splitting and joining, with nothing to
		// run. The median of 200 calls.
		Perf::CheckBudgetConfirmed("Perf.Jobs.ParallelForOverhead", [&]
		{
			const Perf::Measurement calls = Perf::Measure("Perf.Jobs.ParallelForOverhead", 20, 200, [&]
			{
				JobSystem::ParallelFor(count, 1, [&items](uint32_t begin, uint32_t end) { items.fetch_add(end - begin, std::memory_order_relaxed); });
			});
			return calls.MedianMs * 1000.0;
		});
		CHECK(items.load() % count == 0);
	}

	TEST_CASE("A parked worker starts a job within microseconds")
	{
		Perf::ScopedApplicationJobSystem jobs;
		// From Submit to the job's first instruction after 50 ms without work, when the workers sleep. Waiting would run
		// a job no worker has started on this thread, so this thread watches for the job's start before it waits. The
		// median of 100 samples: a latency's tail is the machine's scheduler, not the job system.
		Perf::CheckBudgetConfirmed("Perf.Jobs.WakeLatency", [&]
		{
			std::vector<double> samplesMs;
			samplesMs.reserve(c_WakeSamples);
			for (uint32_t sample = 0; sample < c_WakeSamples; sample++)
			{
				std::this_thread::sleep_for(c_ParkingIdle);
				std::atomic<int64_t> startedNs = 0;
				const auto submitted = std::chrono::steady_clock::now();
				const JobHandle handle = JobSystem::Submit([&startedNs]
				{
					startedNs.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
				});
				while (startedNs.load(std::memory_order_acquire) == 0)
					std::this_thread::yield();
				JobSystem::Wait(handle);
				const std::chrono::steady_clock::time_point started { std::chrono::steady_clock::duration(startedNs.load()) };
				samplesMs.push_back(ElapsedUs(submitted, started) / 1000.0);
			}
			const Perf::Measurement latency = Perf::Summarize(samplesMs);
			MESSAGE("Perf.Jobs.WakeLatency: median ", latency.MedianMs * 1000.0, " us, 95th percentile ", latency.P95Ms * 1000.0, " us, max ",
				latency.MaxMs * 1000.0, " us");
			return latency.MedianMs * 1000.0;
		});
	}
}
