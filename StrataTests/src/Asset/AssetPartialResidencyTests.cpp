#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Strata/Asset/AssetManager.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// What the asset layer gives assets that are partly resident (textures streaming their mip levels): memory changes of a
// loaded asset reported without replacing it (UpdateMemoryUsage), and reads of byte ranges of a stored form through the
// streaming queue (RequestRange). No GPU: fake assets stand for the textures.
namespace
{

	constexpr uint64_t c_MB = 1024 * 1024;

	// The pool totals the statistics report against the sum of what every asset holds.
	bool PoolTotalsMatch(const AssetManagerBase& manager)
	{
		AssetMemoryUsage sum;
		for (const AssetResidencyInfo& asset : manager.GetResidencyInfo())
			sum += asset.Usage;
		return manager.GetStats().Resident == sum;
	}

	Ref<FakeSizedAsset> PeekFake(const AssetManagerBase& manager, AssetHandle handle)
	{
		return std::dynamic_pointer_cast<FakeSizedAsset>(manager.PeekLoadedAsset(handle));
	}

	// Updates until the range read is no longer pending, for ten seconds at most.
	AssetRangeStatus PollUntilDone(AssetManagerBase& manager, uint64_t id, std::vector<uint8_t>& outData, std::string* outError = nullptr)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		AssetRangeStatus status = AssetRangeStatus::Pending;
		while (status == AssetRangeStatus::Pending && std::chrono::steady_clock::now() < deadline)
		{
			manager.Update();
			status = manager.PollRange(id, outData, outError);
			if (status == AssetRangeStatus::Pending)
				std::this_thread::yield();
		}
		return status;
	}

}

TEST_SUITE("Asset.PartialResidency")
{
	TEST_CASE("Memory changes of a loaded asset keep pool totals, state counts and eviction order exact through raise, drop and evict")
	{
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle older = manager->Add(0x81000, MakeUsage(0, 2 * c_MB), 1024);
		const AssetHandle newer = manager->Add(0x81001, MakeUsage(0, 1 * c_MB), 1024);
		const AssetHandle buffers = manager->Add(0x81002, MakeUsage(0, 0, 3 * c_MB), 1024);
		manager->RequestLoad(older);
		manager->Update();
		manager->RequestLoad(newer);
		manager->RequestLoad(buffers);
		manager->Update();
		REQUIRE(manager->GetAssetState(older) == AssetState::Ready);
		REQUIRE(manager->GetAssetState(newer) == AssetState::Ready);
		REQUIRE(PoolTotalsMatch(*manager));
		REQUIRE(StateCountsMatch(*manager));
		// What the built-in assets hold (CPU geometry) beside the test's.
		AssetMemoryUsage builtin = manager->GetStats().Resident;
		builtin -= MakeUsage(0, 3 * c_MB, 3 * c_MB);
		const auto resident = [&manager, &builtin]()
		{
			AssetMemoryUsage usage = manager->GetStats().Resident;
			usage -= builtin;
			return usage;
		};

		// The older one raises (more GPU texture memory, and CPU memory for the bytes on their way), then drops back.
		Ref<FakeSizedAsset> olderAsset = PeekFake(*manager, older);
		REQUIRE(olderAsset);
		olderAsset->SetUsage(MakeUsage(1 * c_MB, 8 * c_MB));
		manager->UpdateMemoryUsage(older, *olderAsset);
		CHECK(resident() == MakeUsage(1 * c_MB, 9 * c_MB, 3 * c_MB));
		CHECK(PoolTotalsMatch(*manager));
		CHECK(StateCountsMatch(*manager));
		olderAsset->SetUsage(MakeUsage(0, 4 * c_MB));
		manager->UpdateMemoryUsage(older, *olderAsset);
		CHECK(resident() == MakeUsage(0, 5 * c_MB, 3 * c_MB));
		CHECK(PoolTotalsMatch(*manager));
		CHECK(StateCountsMatch(*manager));
		olderAsset.reset(); // A reference held here would keep it from being evicted

		// Another object than the loaded one, or an unknown asset, changes nothing.
		FakeSizedAsset stranger(MakeUsage(0, 100 * c_MB));
		manager->UpdateMemoryUsage(older, stranger);
		manager->UpdateMemoryUsage(UUID(0x81FFF), stranger);
		CHECK(resident() == MakeUsage(0, 5 * c_MB, 3 * c_MB));

		// Its place in the eviction order is its latest request's, not its latest change's: over a budget that holds one of
		// the two textures, the older one goes, although it changed last. The buffers' pool is not touched.
		for (int frame = 0; frame < 10; frame++)
			manager->Update();
		AssetResidencyBudgets budgets = manager->GetResidencyBudgets();
		budgets.GpuTextures = 2 * c_MB;
		manager->SetResidencyBudgets(budgets);
		manager->Update();
		CHECK(manager->GetAssetState(older) == AssetState::Unloaded);
		CHECK(manager->GetAssetState(newer) == AssetState::Ready);
		CHECK(manager->GetAssetState(buffers) == AssetState::Ready);
		CHECK(resident() == MakeUsage(0, 1 * c_MB, 3 * c_MB));
		CHECK(PoolTotalsMatch(*manager));
		CHECK(StateCountsMatch(*manager));

		// An asset that grows into another pool is found by that pool's eviction too.
		Ref<FakeSizedAsset> newerAsset = PeekFake(*manager, newer);
		REQUIRE(newerAsset);
		newerAsset->SetUsage(MakeUsage(0, 1 * c_MB, 2 * c_MB));
		manager->UpdateMemoryUsage(newer, *newerAsset);
		CHECK(resident() == MakeUsage(0, 1 * c_MB, 5 * c_MB));
		CHECK(PoolTotalsMatch(*manager));
		newerAsset.reset();
		budgets.GpuBuffers = 4 * c_MB;
		manager->SetResidencyBudgets(budgets);
		for (int frame = 0; frame < 10; frame++)
			manager->Update();
		CHECK(manager->GetAssetState(newer) == AssetState::Unloaded); // Requested before the buffers
		CHECK(manager->GetAssetState(buffers) == AssetState::Ready);
		CHECK(resident() == MakeUsage(0, 0, 3 * c_MB));
		CHECK(PoolTotalsMatch(*manager));
		CHECK(StateCountsMatch(*manager));
	}

	TEST_CASE("Range reads of a loaded asset's stored form go through the streaming queue and hand their bytes over once")
	{
		ScopedFakeLoader loader;
		ScopedJobSystem jobs(1, 1); // Reads wait at the gate on an I/O thread
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle asset = manager->Add(0x82000, MakeUsage(64), 0);
		std::vector<uint8_t> stored = EncodeFakeAsset(MakeUsage(64));
		manager->RequestLoad(asset);
		REQUIRE(manager->WaitForPendingLoads());
		REQUIRE(manager->GetAssetState(asset) == AssetState::Ready);

		// Only loaded assets with a stored form, and ranges inside it.
		const AssetHandle unloaded = manager->Add(0x82001, MakeUsage(64), 0);
		CHECK(manager->RequestRange(unloaded, 0, 4) == 0);
		CHECK(manager->RequestRange(UUID(0x82FFF), 0, 4) == 0);
		CHECK(manager->RequestRange(asset, 0, 0) == 0);
		std::vector<uint8_t> data;
		CHECK(manager->PollRange(12345, data) == AssetRangeStatus::Unknown);

		// The reads complete without the manager's updates finalizing anything, and their bytes count in flight until they
		// are polled.
		manager->CloseGate();
		const uint64_t id = manager->RequestRange(asset, 4, 8, AssetPriority::Low, 1.0f);
		REQUIRE(id != 0);
		CHECK(manager->GetStats().RangeReads == 1);
		CHECK(manager->PollRange(id, data) == AssetRangeStatus::Pending);
		CHECK_FALSE(manager->HasPendingLoads()); // Range reads are their owner's to poll
		manager->OpenGate();
		std::string error;
		REQUIRE(PollUntilDone(*manager, id, data, &error) == AssetRangeStatus::Complete);
		CHECK(data == std::vector<uint8_t>(stored.begin() + 4, stored.begin() + 12));
		CHECK(manager->GetStats().InFlightBytes == 0);
		CHECK(manager->GetStats().RangeReads == 0);
		CHECK(manager->GetStats().RangeReadsCompleted == 1);
		CHECK(manager->PollRange(id, data) == AssetRangeStatus::Unknown); // Done with

		// A read of an asset that changes before it completes fails: its bytes would be another version's.
		manager->CloseGate();
		const uint64_t stale = manager->RequestRange(asset, 0, 8);
		REQUIRE(stale != 0);
		manager->ReloadAsset(asset);
		manager->OpenGate();
		CHECK(PollUntilDone(*manager, stale, data, &error) == AssetRangeStatus::Failed);
		CHECK_FALSE(error.empty());
		REQUIRE(manager->WaitForPendingLoads()); // The reload
		CHECK(manager->GetStats().InFlightBytes == 0);
	}

	TEST_CASE("Cancelled range reads release their place in the streaming queue")
	{
		ScopedFakeLoader loader;
		ScopedJobSystem jobs(1, 1);
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle asset = manager->Add(0x83000, MakeUsage(64), 0);
		manager->RequestLoad(asset);
		REQUIRE(manager->WaitForPendingLoads());
		REQUIRE(manager->GetAssetState(asset) == AssetState::Ready);

		// One read waits at the gate (running), the others wait in the queue behind the read limit: cancelling each releases
		// what it held, whether it was queued, running or done.
		manager->CloseGate();
		std::vector<uint64_t> reads;
		for (int index = 0; index < 6; index++)
			reads.push_back(manager->RequestRange(asset, 0, 4));
		REQUIRE(manager->WaitForWaitingReads(1));
		for (uint64_t read : reads)
		{
			REQUIRE(read != 0);
			manager->CancelRange(read);
		}
		manager->OpenGate();
		// Until the I/O thread, which may wake from the gate late (a busy machine: 1,000 updates took a few milliseconds),
		// has let go of the reads, for ten seconds at most.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while ((manager->GetStats().InFlightBytes > 0 || manager->GetStats().RangeReads > 0) && std::chrono::steady_clock::now() < deadline)
		{
			manager->Update();
			std::this_thread::yield();
		}
		CHECK(manager->GetStats().InFlightBytes == 0);
		CHECK(manager->GetStats().RangeReads == 0);
		CHECK(manager->GetStats().RangeReadsCompleted == 0);
		std::vector<uint8_t> data;
		for (uint64_t read : reads)
			CHECK(manager->PollRange(read, data) == AssetRangeStatus::Unknown);

		// The queue is free for loads again.
		const AssetHandle other = manager->Add(0x83001, MakeUsage(64), 0);
		manager->RequestLoad(other);
		REQUIRE(manager->WaitForPendingLoads());
		CHECK(manager->GetAssetState(other) == AssetState::Ready);
	}
}
