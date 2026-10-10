#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Strata/Asset/AssetStreamingQueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint64_t c_MB = 1024 * 1024;

	// A queue whose dispatches are recorded; the test finishes loads explicitly.
	struct RecordingQueue
	{
		std::vector<AssetStreamingRequest> Dispatched;
		AssetStreamingQueue Queue { [this](const AssetStreamingRequest& request)
		{
			Dispatched.push_back(request);
			return true;
		} };

		std::vector<uint64_t> GetDispatchedHandles() const
		{
			std::vector<uint64_t> handles;
			for (const AssetStreamingRequest& request : Dispatched)
				handles.push_back(static_cast<uint64_t>(request.Handle));
			return handles;
		}
	};

}

TEST_SUITE("Asset.StreamingQueue")
{
	TEST_CASE("Requests are dispatched by priority, then score, then request order")
	{
		RecordingQueue recorder;
		AssetStreamingQueue& queue = recorder.Queue;
		queue.SetLimits({ 1000 * c_MB, 1000 });
		queue.Enqueue(UUID(1), AssetPriority::Low, 0.0f, 10, 0);
		queue.Enqueue(UUID(2), AssetPriority::Normal, 0.0f, 10, 0);
		queue.Enqueue(UUID(3), AssetPriority::High, 0.0f, 10, 0);
		queue.Enqueue(UUID(4), AssetPriority::Normal, 5.0f, 10, 0);
		queue.Enqueue(UUID(5), AssetPriority::Normal, 0.0f, 10, 0);
		queue.Enqueue(UUID(6), AssetPriority::High, std::numeric_limits<float>::quiet_NaN(), 10, 0); // Counts as 0
		CHECK(queue.GetQueuedCount() == 6);
		CHECK(queue.GetStats().Queued == std::array<uint32_t, 3> { 2, 3, 1 });
		queue.Pump();
		CHECK(recorder.GetDispatchedHandles() == std::vector<uint64_t> { 3, 6, 4, 2, 5, 1 });
		CHECK(queue.GetQueuedCount() == 0);
		const AssetStreamingQueueStats stats = queue.GetStats();
		CHECK(stats.InFlightLoads == 6);
		CHECK(stats.OutstandingReads == 6);
		CHECK(stats.InFlightBytes == 60);
		CHECK(stats.Dispatched == 6);
	}

	TEST_CASE("A raised request overtakes the requests it now outranks, and requests are never lowered")
	{
		RecordingQueue recorder;
		AssetStreamingQueue& queue = recorder.Queue;
		queue.SetLimits({ 1000 * c_MB, 1 });
		// One load reads: nothing else is dispatched until it is done.
		queue.Enqueue(UUID(1000), AssetPriority::Normal, 0.0f, 1, 0);
		queue.Pump();
		REQUIRE(recorder.Dispatched.size() == 1);

		for (uint64_t index = 0; index < 50; index++)
			queue.Enqueue(UUID(index + 1), AssetPriority::Normal, 0.0f, 1, 0);
		queue.Enqueue(UUID(500), AssetPriority::Low, 0.0f, 1, 0);
		CHECK(queue.Raise(UUID(500), AssetPriority::High, 0.0f));
		CHECK_FALSE(queue.Raise(UUID(500), AssetPriority::Low, 100.0f)); // Never lowered
		CHECK_FALSE(queue.Raise(UUID(500), AssetPriority::High, 0.0f));  // Nothing changes
		CHECK_FALSE(queue.Raise(UUID(999), AssetPriority::High, 0.0f));  // Not queued
		CHECK(queue.Raise(UUID(50), AssetPriority::Normal, 1.0f));      // A higher score within the priority
		CHECK(queue.GetStats().Queued == std::array<uint32_t, 3> { 1, 50, 0 });

		// Each finished read lets the next one through.
		for (int read = 0; read < 51; read++)
			queue.OnReadFinished();
		const std::vector<uint64_t> order = recorder.GetDispatchedHandles();
		REQUIRE(order.size() == 52);
		CHECK(order[1] == 500);
		CHECK(order[2] == 50);
		CHECK(order[3] == 1); // Then the rest in request order
		CHECK(order[51] == 49);
	}

	TEST_CASE("Cancelled requests are never dispatched")
	{
		RecordingQueue recorder;
		AssetStreamingQueue& queue = recorder.Queue;
		queue.SetLimits({ 1000 * c_MB, 1 });
		queue.Enqueue(UUID(1), AssetPriority::Normal, 0.0f, 1, 0);
		queue.Pump();
		queue.Enqueue(UUID(2), AssetPriority::Normal, 0.0f, 1, 0);
		queue.Enqueue(UUID(3), AssetPriority::Normal, 0.0f, 1, 0);
		CHECK(queue.IsQueued(UUID(2)));
		CHECK(queue.Cancel(UUID(2)));
		CHECK_FALSE(queue.Cancel(UUID(2)));
		CHECK_FALSE(queue.Cancel(UUID(1))); // Dispatched already
		CHECK_FALSE(queue.IsQueued(UUID(2)));
		queue.OnReadFinished();
		queue.OnReadFinished();
		CHECK(recorder.GetDispatchedHandles() == std::vector<uint64_t> { 1, 3 });

		// A closed queue dispatches nothing more.
		queue.OnLoadFinished(1);
		queue.OnLoadFinished(1);
		queue.SetLimits({ 1000 * c_MB, 1000 });
		queue.Enqueue(UUID(4), AssetPriority::High, 0.0f, 1, 0);
		queue.Close();
		queue.Enqueue(UUID(5), AssetPriority::High, 0.0f, 1, 0);
		queue.Pump();
		CHECK(recorder.Dispatched.size() == 2);
		CHECK(queue.GetQueuedCount() == 0);
		CHECK_FALSE(queue.HasWork());
	}

	TEST_CASE("A repeated request replaces the waiting one unless that one is of a later generation")
	{
		RecordingQueue recorder;
		AssetStreamingQueue& queue = recorder.Queue;
		queue.SetLimits({ 1000 * c_MB, 1 });
		queue.Enqueue(UUID(1), AssetPriority::Normal, 0.0f, 1, 1);
		queue.Pump(); // Reads: what follows waits
		queue.Enqueue(UUID(2), AssetPriority::Normal, 0.0f, 1, 1);
		queue.Enqueue(UUID(3), AssetPriority::Low, 0.0f, 1, 5);

		// The owner requested asset 3 again (generation 6, e.g. after an unload): the request is replaced, in its place.
		queue.Enqueue(UUID(3), AssetPriority::Normal, 0.0f, 2, 6);
		CHECK(queue.GetStats().Queued == std::array<uint32_t, 3> { 0, 2, 0 });
		// A request of an earlier generation arriving late (a thread that decided to load before the unload) is stale: the
		// newer request stays, so the load that follows is the one the owner still expects.
		queue.Enqueue(UUID(3), AssetPriority::High, 0.0f, 3, 5);
		CHECK(queue.GetStats().Queued == std::array<uint32_t, 3> { 0, 2, 0 });
		CHECK(queue.GetQueuedCount() == 2);

		queue.OnReadFinished();
		queue.OnReadFinished();
		REQUIRE(recorder.Dispatched.size() == 3);
		CHECK(recorder.GetDispatchedHandles() == std::vector<uint64_t> { 1, 2, 3 });
		CHECK(recorder.Dispatched[2].Generation == 6);
		CHECK(recorder.Dispatched[2].Bytes == 2);
		CHECK(recorder.Dispatched[2].Priority == AssetPriority::Normal);
	}

	TEST_CASE("Bytes in flight stay below the limit plus one asset, and one load always runs")
	{
		RecordingQueue recorder;
		AssetStreamingQueue& queue = recorder.Queue;
		queue.SetLimits({ 64 * c_MB, 1000 });
		for (uint64_t index = 0; index < 200; index++)
			queue.Enqueue(UUID(index + 1), AssetPriority::Normal, 0.0f, 8 * c_MB, 0);
		queue.Pump();
		CHECK(recorder.Dispatched.size() == 8); // 8 x 8 MB reach the limit

		// Finish loads in a random order (reads first, the whole load later), the way loads complete.
		std::mt19937 random(7);
		std::vector<AssetStreamingRequest> reading = recorder.Dispatched;
		std::vector<AssetStreamingRequest> decoded;
		size_t seen = recorder.Dispatched.size();
		uint64_t maximum = 0;
		while (!reading.empty() || !decoded.empty())
		{
			if (!reading.empty() && (decoded.empty() || random() % 2 == 0))
			{
				const size_t index = random() % reading.size();
				decoded.push_back(reading[index]);
				reading.erase(reading.begin() + static_cast<std::ptrdiff_t>(index));
				queue.OnReadFinished();
			}
			else
			{
				const size_t index = random() % decoded.size();
				const uint64_t bytes = decoded[index].Bytes;
				decoded.erase(decoded.begin() + static_cast<std::ptrdiff_t>(index));
				queue.OnLoadFinished(bytes);
			}
			for (; seen < recorder.Dispatched.size(); seen++)
				reading.push_back(recorder.Dispatched[seen]);
			maximum = std::max(maximum, queue.GetStats().InFlightBytes);
		}
		CHECK(recorder.Dispatched.size() == 200);
		CHECK(maximum <= 64 * c_MB + 8 * c_MB);
		CHECK(queue.GetStats().InFlightBytesHighWater <= 64 * c_MB + 8 * c_MB);
		CHECK(queue.GetStats().InFlightBytes == 0);
		CHECK_FALSE(queue.HasWork());

		// A load larger than the limit still runs when nothing else does; unknown sizes count as 1 MB.
		queue.Enqueue(UUID(1000), AssetPriority::Normal, 0.0f, 500 * c_MB, 0);
		queue.Enqueue(UUID(1001), AssetPriority::Normal, 0.0f, 0, 0);
		queue.Pump();
		REQUIRE(recorder.Dispatched.size() == 201);
		queue.OnReadFinished();
		queue.OnLoadFinished(500 * c_MB);
		REQUIRE(recorder.Dispatched.size() == 202);
		CHECK(recorder.Dispatched.back().Bytes == AssetStreamingQueue::c_UnknownSizeBytes);
	}

	TEST_CASE("Outstanding reads are limited, and a refused dispatch is forgotten")
	{
		bool refuse = false;
		std::vector<uint64_t> dispatched;
		AssetStreamingQueue queue([&](const AssetStreamingRequest& request)
		{
			if (refuse)
				return false;
			dispatched.push_back(static_cast<uint64_t>(request.Handle));
			return true;
		});
		queue.SetLimits({ 1000 * c_MB, 3 });
		for (uint64_t index = 0; index < 10; index++)
			queue.Enqueue(UUID(index + 1), AssetPriority::Normal, 0.0f, 1, 0);
		queue.Pump();
		CHECK(dispatched.size() == 3);
		CHECK(queue.GetStats().OutstandingReads == 3);
		queue.OnReadFinished();
		CHECK(dispatched.size() == 4);

		refuse = true;
		queue.OnReadFinished(); // Every waiting request is refused (e.g. its asset was unloaded meanwhile)
		CHECK(queue.GetQueuedCount() == 0);
		CHECK(queue.GetStats().InFlightLoads == 4);
		CHECK(queue.GetStats().OutstandingReads == 2);
	}

	TEST_CASE("Loads that finish inside the dispatch do not make the queue recurse")
	{
		// Without a job system, loads run inline: the dispatch itself finishes the load and pumps again.
		size_t dispatched = 0;
		size_t depth = 0;
		size_t maximumDepth = 0;
		AssetStreamingQueue* queuePointer = nullptr;
		AssetStreamingQueue queue([&](const AssetStreamingRequest& request)
		{
			depth++;
			maximumDepth = std::max(maximumDepth, depth);
			dispatched++;
			queuePointer->OnReadFinished();
			queuePointer->OnLoadFinished(request.Bytes);
			depth--;
			return true;
		});
		queuePointer = &queue;
		queue.SetLimits({ 1, 1 });
		for (uint64_t index = 0; index < 20000; index++)
			queue.Enqueue(UUID(index + 1), AssetPriority::Normal, 0.0f, 4, 0);
		queue.Pump();
		CHECK(dispatched == 20000);
		CHECK(maximumDepth == 1);
		CHECK_FALSE(queue.HasWork());
	}

	TEST_CASE("The queue is thread-safe")
	{
		std::atomic<uint64_t> dispatched = 0;
		std::vector<std::thread> loads;
		std::mutex loadsMutex;
		AssetStreamingQueue* queuePointer = nullptr;
		AssetStreamingQueue queue([&](const AssetStreamingRequest& request)
		{
			dispatched++;
			std::scoped_lock<std::mutex> lock(loadsMutex);
			loads.emplace_back([&queuePointer, bytes = request.Bytes]()
			{
				queuePointer->OnReadFinished();
				queuePointer->OnLoadFinished(bytes);
			});
			return true;
		});
		queuePointer = &queue;
		queue.SetLimits({ 16, 4 });

		std::vector<std::thread> producers;
		for (uint64_t producer = 0; producer < 4; producer++)
		{
			producers.emplace_back([&queue, producer]()
			{
				for (uint64_t index = 0; index < 100; index++)
				{
					queue.Enqueue(UUID(producer * 1000 + index + 1), static_cast<AssetPriority>(index % 3), 0.0f, 3, 0);
					queue.Pump();
				}
			});
		}
		for (std::thread& producer : producers)
			producer.join();

		// Every load finishes on a thread of its own, pumping the next ones.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (queue.HasWork() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		CHECK_FALSE(queue.HasWork());
		CHECK(dispatched == 400);
		std::scoped_lock<std::mutex> lock(loadsMutex);
		for (std::thread& load : loads)
			load.join();
		CHECK(queue.GetStats().InFlightBytesHighWater <= 16 + 3);
	}

	TEST_CASE("A Low request raised to High overtakes 50 queued Normal requests")
	{
		ScopedFakeLoader loader;
		ScopedJobSystem jobs(2, 1); // One I/O thread: two reads at a time
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		manager->CloseGate();

		std::vector<AssetHandle> normal;
		for (uint64_t index = 0; index < 52; index++)
			normal.push_back(manager->Add(0x1000 + index, MakeUsage(1)));
		const AssetHandle low = manager->Add(0x2000, MakeUsage(1));

		// The first two are dispatched (one reads, one waits for the I/O thread); the rest queue behind them.
		for (AssetHandle handle : normal)
			manager->RequestLoad(handle, AssetPriority::Normal);
		manager->RequestLoad(low, AssetPriority::Low);
		REQUIRE(manager->WaitForWaitingReads(1));
		CHECK(manager->GetStats().QueuedLoads == std::array<uint32_t, 3> { 0, 50, 1 });

		// Requesting it again at a higher priority raises it.
		CHECK(manager->GetAsset(low, AssetPriority::High) == nullptr);
		CHECK(manager->GetStats().QueuedLoads == std::array<uint32_t, 3> { 1, 50, 0 });

		manager->OpenGate();
		REQUIRE(manager->WaitForPendingLoads());
		const std::vector<AssetHandle> order = manager->GetReadOrder();
		REQUIRE(order.size() == 53);
		CHECK(order.front() == normal.front());
		// Read before every one of the 50 that were queued ahead of it (with its High job priority it even passes the
		// dispatched read that waits for the I/O thread).
		const auto position = [&order](AssetHandle handle) { return std::find(order.begin(), order.end(), handle) - order.begin(); };
		for (size_t index = 2; index < normal.size(); index++)
			CHECK(position(low) < position(normal[index]));
		CHECK(position(low) <= 2);
		CHECK(manager->GetAssetState(low) == AssetState::Ready);
	}

	TEST_CASE("Cancelled loads are never read before the read, and never decoded after it")
	{
		ScopedFakeLoader loader;
		ScopedJobSystem jobs(2, 1);
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		manager->CloseGate();
		const AssetHandle reading = manager->Add(0x3000, MakeUsage(1));
		const AssetHandle waiting = manager->Add(0x3001, MakeUsage(1));
		const AssetHandle queued = manager->Add(0x3002, MakeUsage(1));
		const AssetHandle pinned = manager->Add(0x3003, MakeUsage(1));

		manager->RequestLoad(reading);
		REQUIRE(manager->WaitForWaitingReads(1)); // Reads (held at the gate)
		manager->RequestLoad(waiting);              // Dispatched, waits for the I/O thread
		manager->RequestLoad(queued);               // Waits in the queue
		const AssetPin pin = manager->Pin(pinned, AssetPriority::Low);
		CHECK(manager->GetStats().QueuedLoads[1] == 1);

		CHECK(manager->CancelLoad(queued));
		CHECK(manager->CancelLoad(waiting));
		CHECK(manager->CancelLoad(reading));
		CHECK_FALSE(manager->CancelLoad(reading)); // Not loading any more
		CHECK_FALSE(manager->CancelLoad(pinned));  // Pins are a stronger claim
		CHECK_FALSE(manager->CancelLoad(UUID(0x9999)));
		CHECK(manager->GetAssetState(queued) == AssetState::Unloaded);
		CHECK(manager->GetAssetState(reading) == AssetState::Unloaded);

		manager->AllowReads(1); // The read in progress completes
		manager->OpenGate();
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(manager->GetReadCount(reading) == 1);
		CHECK(manager->GetReadCount(waiting) == 0);
		CHECK(manager->GetReadCount(queued) == 0);
		CHECK(manager->GetReadCount(pinned) == 1);
		CHECK(loader.GetCallCount() == 1); // Only the pinned asset was decoded
		CHECK(manager->GetAssetState(pinned) == AssetState::Ready);
		CHECK(manager->GetAssetState(reading) == AssetState::Unloaded);
		const AssetManagerStats stats = manager->GetStats();
		CHECK(stats.Cancellations == 3);
		CHECK(stats.InFlightBytes == 0);
		CHECK(stats.InFlightLoads == 0);

		// A cancelled asset loads normally when it is requested again.
		manager->RequestLoad(queued);
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(manager->GetAssetState(queued) == AssetState::Ready);
	}

	TEST_CASE("With 200 latched reads of 8 MB and a 64 MB limit, the bytes in flight never exceed the limit plus one asset")
	{
		ScopedFakeLoader loader;
		ScopedJobSystem jobs(4, 8); // Enough reads at once for the byte limit to be what holds loads back
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.InFlightBytes = 64 * c_MB;
		manager->SetResidencyBudgets(budgets);
		manager->CloseGate();

		std::vector<AssetHandle> handles;
		for (uint64_t index = 0; index < 200; index++)
			handles.push_back(manager->Add(0x4000 + index, MakeUsage(1), 8 * c_MB));
		for (AssetHandle handle : handles)
			manager->RequestLoad(handle);
		REQUIRE(manager->WaitForWaitingReads(8));
		CHECK(manager->GetStats().InFlightLoads == 8);
		CHECK(manager->GetStats().QueuedLoads[1] == 192);

		// Let reads through a few at a time; the frames finalize what was read and let the next loads in.
		uint64_t maximum = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (manager->HasPendingLoads() && std::chrono::steady_clock::now() < deadline)
		{
			manager->AllowReads(3);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			maximum = std::max(maximum, manager->GetStats().InFlightBytes);
			manager->Update();
		}
		const AssetManagerStats stats = manager->GetStats();
		CHECK_FALSE(manager->HasPendingLoads());
		CHECK(stats.TotalLoadsCompleted == 200);
		CHECK(maximum <= 64 * c_MB + 8 * c_MB);
		CHECK(stats.InFlightBytesHighWater <= 64 * c_MB + 8 * c_MB);
		CHECK(stats.InFlightBytesHighWater >= 56 * c_MB);
		CHECK(stats.InFlightBytes == 0);
	}

	TEST_CASE("Reads are issued in priority order")
	{
		ScopedFakeLoader loader;
		ScopedJobSystem jobs(2, 1);
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		manager->CloseGate();
		const AssetHandle first = manager->Add(0x5000, MakeUsage(1));
		const AssetHandle second = manager->Add(0x5001, MakeUsage(1));
		manager->RequestLoad(first);
		REQUIRE(manager->WaitForWaitingReads(1));
		manager->RequestLoad(second); // Dispatched behind the first

		// Queued in a mixed order: read by priority, then score, then request order.
		std::vector<std::pair<AssetHandle, AssetPriority>> requests;
		for (uint64_t index = 0; index < 30; index++)
			requests.emplace_back(manager->Add(0x6000 + index, MakeUsage(1)), static_cast<AssetPriority>((index * 7) % 3));
		for (const auto& [handle, priority] : requests)
			manager->RequestLoad(handle, priority);
		const AssetHandle scored = manager->Add(0x7000, MakeUsage(1));
		manager->RequestLoad(scored, AssetPriority::Normal, 10.0f);

		manager->OpenGate();
		REQUIRE(manager->WaitForPendingLoads());
		std::vector<AssetHandle> expected;
		for (AssetPriority priority : { AssetPriority::High, AssetPriority::Normal, AssetPriority::Low })
		{
			if (priority == AssetPriority::Normal)
				expected.push_back(scored);
			for (const auto& [handle, requestPriority] : requests)
			{
				if (requestPriority == priority)
					expected.push_back(handle);
			}
		}
		// The queued requests are read in their order (the second request was dispatched before them: the I/O pool's own
		// job priorities let High reads pass it, which is not the queue's doing).
		std::vector<AssetHandle> order = manager->GetReadOrder();
		REQUIRE(order.size() == expected.size() + 2);
		CHECK(order.front() == first);
		std::erase(order, first);
		std::erase(order, second);
		CHECK(order == expected);
	}
}
