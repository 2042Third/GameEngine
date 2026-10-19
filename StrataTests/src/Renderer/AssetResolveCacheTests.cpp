#include <doctest/doctest.h>

#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Renderer/AssetResolveCache.h"
#include "Strata/Renderer/Material.h"
#include "TestHelpers.h"

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// A pack of two materials, read by a runtime asset manager (loads finish in its Update).
	Ref<RuntimeAssetManager> CreateManager(AssetHandle first, AssetHandle second)
	{
		std::map<uint64_t, std::string> documents;
		documents[static_cast<uint64_t>(first)] = Material::Create()->Serialize().dump();
		documents[static_cast<uint64_t>(second)] = Material::Create()->Serialize().dump();
		std::vector<AssetMetadata> metadata(2);
		metadata[0].Handle = first;
		metadata[1].Handle = second;
		for (AssetMetadata& entry : metadata)
		{
			entry.Type = AssetType::Material;
			entry.Path = "Materials/" + entry.Handle.ToString() + ".stmat";
		}
		const std::filesystem::path path = Tests::CreateTemporaryDirectory("AssetResolveCache") / "Materials.stpak";
		REQUIRE(AssetPackWriter::Write(path, metadata, [&documents](const AssetMetadata& asset, std::vector<uint8_t>& outData, std::string*)
		{
			const std::string& document = documents.at(static_cast<uint64_t>(asset.Handle));
			outData.assign(document.begin(), document.end());
			return true;
		}));
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(path);
		REQUIRE(manager);
		return manager;
	}

}

TEST_SUITE("Renderer.AssetResolveCache")
{
	TEST_CASE("Each asset is looked up once per frame, and the frame's references are let go when it ends")
	{
		const AssetHandle first(0xC001);
		const AssetHandle second(0xC002);
		const Ref<RuntimeAssetManager> manager = CreateManager(first, second);
		AssetResolveCache cache;
		cache.BeginFrame(manager);

		// Still loading: null, counted as pending, requested once however often it is resolved.
		CHECK_FALSE(cache.Resolve<Material>(first, AssetPriority::High));
		CHECK_FALSE(cache.Resolve<Material>(first, AssetPriority::High));
		CHECK(cache.GetStats().GetAssetCalls == 1);
		CHECK(cache.GetStats().Requested == 1);
		CHECK(cache.GetStats().Pending == 1);
		CHECK(manager->GetAssetState(first) == AssetState::Loading);
		cache.EndFrame();
		REQUIRE(manager->WaitForPendingLoads());

		cache.BeginFrame(manager);
		Ref<Material> material = cache.Resolve<Material>(first, AssetPriority::Normal);
		REQUIRE(material);
		CHECK(cache.Resolve<Material>(first, AssetPriority::Normal) == material);
		CHECK(cache.GetStats().GetAssetCalls == 1);
		CHECK(cache.GetStats().Pending == 0);
		const std::weak_ptr<Material> held = material;
		material = nullptr;
		CHECK(held.use_count() == 2); // The manager's reference and the frame's

		// The end of the frame lets go: without another frame, the asset can be evicted once it goes unused.
		cache.EndFrame();
		CHECK(held.use_count() == 1);
		manager->Update();
		manager->Update();
		CHECK(manager->TrimUnused(1) == 1);
		CHECK(manager->GetAssetState(first) == AssetState::Unloaded);
		CHECK(held.expired());
	}

	TEST_CASE("The cache keeps no asset manager alive between frames, and answers for the frame's manager only")
	{
		const AssetHandle first(0xC021);
		const AssetHandle second(0xC022);
		AssetResolveCache cache;
		std::weak_ptr<AssetManagerBase> closed;
		{
			const Ref<RuntimeAssetManager> manager = CreateManager(first, second);
			closed = manager;
			cache.BeginFrame(manager);
			CHECK_FALSE(cache.Resolve<Material>(first, AssetPriority::Normal));
			REQUIRE(manager->WaitForPendingLoads());
			cache.EndFrame();
		}
		CHECK(closed.expired());

		// Another manager (another project): it is asked afresh.
		const Ref<RuntimeAssetManager> next = CreateManager(first, second);
		cache.BeginFrame(next);
		CHECK_FALSE(cache.Resolve<Material>(first, AssetPriority::Normal));
		CHECK(cache.GetStats().GetAssetCalls == 1);
		CHECK(next->GetAssetState(first) == AssetState::Loading);
		cache.EndFrame();
		REQUIRE(next->WaitForPendingLoads());
	}

	TEST_CASE("Resident lookups never start loads; prefetches request at low priority")
	{
		const AssetHandle first(0xC011);
		const AssetHandle second(0xC012);
		const Ref<RuntimeAssetManager> manager = CreateManager(first, second);
		AssetResolveCache cache;
		cache.BeginFrame(manager);

		CHECK_FALSE(cache.FindResident<Material>(first));
		CHECK(manager->GetAssetState(first) == AssetState::Unloaded);
		CHECK(cache.GetStats().ResidentLookups == 1);
		CHECK(cache.GetStats().GetAssetCalls == 0);
		// A resident lookup that found nothing requested nothing: a prefetch later in the frame still requests the load.
		cache.Prefetch(first);
		CHECK(cache.GetStats().Prefetched == 1);
		CHECK(manager->GetAssetState(first) == AssetState::Loading);
		// A request still asks the manager, at its own priority.
		CHECK_FALSE(cache.Resolve<Material>(first, AssetPriority::Normal));
		CHECK(cache.GetStats().GetAssetCalls == 1);

		cache.Prefetch(second);
		cache.Prefetch(second);
		CHECK(cache.GetStats().Prefetched == 2);
		CHECK(manager->GetAssetState(second) == AssetState::Loading);
		cache.EndFrame();
		REQUIRE(manager->WaitForPendingLoads());

		cache.BeginFrame(manager);
		CHECK(cache.FindResident<Material>(second));
		CHECK(cache.GetStats().GetAssetCalls == 1);
		// Found loaded (and stamped as used): there is nothing to prefetch.
		cache.Prefetch(second);
		CHECK(cache.GetStats().Prefetched == 0);
		// Unknown handles and other types resolve to nothing.
		CHECK_FALSE(cache.Resolve<Material>(UUID(0xBAD), AssetPriority::Normal));
		CHECK(cache.GetStats().Pending == 0);
		cache.EndFrame();

		// Without a manager nothing resolves.
		cache.BeginFrame(nullptr);
		CHECK_FALSE(cache.Resolve<Material>(first, AssetPriority::Normal));
		cache.Prefetch(first);
		CHECK(cache.GetStats().GetAssetCalls == 0);
		CHECK(cache.GetStats().Prefetched == 0);
		cache.EndFrame();
	}

	TEST_CASE("A frame's lookups of loaded assets are replayed by a later frame in one call, counting as the frame counted them")
	{
		const AssetHandle first(0xC031);
		const AssetHandle second(0xC032);
		const AssetHandle elsewhere(0xC033); // Unknown: prefetched only
		const Ref<RuntimeAssetManager> manager = CreateManager(first, second);
		REQUIRE(manager->LoadAssetSync(first));
		REQUIRE(manager->LoadAssetSync(second));
		AssetResolveCache cache;

		// Recorded: a request, a resident lookup, a lookup the frame answers itself, and a prefetch of what is not loaded,
		// which is left out (its request is the caller's to make again).
		cache.BeginFrame(manager);
		cache.BeginRecording();
		const Ref<Material> secondMaterial = cache.Resolve<Material>(second, AssetPriority::Normal);
		REQUIRE(secondMaterial);
		REQUIRE(cache.FindResident<Material>(first));
		CHECK(cache.Resolve<Material>(second, AssetPriority::High) == secondMaterial);
		cache.Prefetch(elsewhere);
		AssetResolveCache::Recording recording;
		REQUIRE(cache.EndRecording(recording));
		const AssetResolveStats recorded = cache.GetStats();
		cache.EndFrame();
		REQUIRE(recording.GetHandles().size() == 2);
		CHECK(recording.GetHandles()[0] == first); // Sorted
		CHECK(recording.GetHandles()[1] == second);
		CHECK(recording.GetAssets()[1].lock() == secondMaterial);

		// Replayed: one call stamps both as used; the frame counts what the recorded one counted, and its later lookups of
		// them ask the manager nothing.
		const uint64_t calls = manager->GetStats().GetAssetCalls;
		cache.BeginFrame(manager);
		REQUIRE(cache.Replay(recording));
		CHECK(manager->GetStats().GetAssetCalls == calls + 2);
		CHECK(cache.Resolve<Material>(second, AssetPriority::Normal) == secondMaterial);
		CHECK(cache.FindResident<Material>(first));
		cache.Prefetch(first); // Found loaded: nothing to prefetch
		cache.Prefetch(elsewhere);
		CHECK(manager->GetStats().GetAssetCalls == calls + 2);
		CHECK(cache.GetStats().GetAssetCalls == recorded.GetAssetCalls);
		CHECK(cache.GetStats().Requested == recorded.Requested);
		CHECK(cache.GetStats().ResidentLookups == recorded.ResidentLookups);
		CHECK(cache.GetStats().Prefetched == recorded.Prefetched);
		CHECK(cache.GetStats().Pending == 0);
		CHECK_FALSE(cache.Replay(recording)); // Once per frame
		// The replay's references end with the frame, as the lookups' do.
		const std::weak_ptr<Material> held = secondMaterial;
		cache.EndFrame();
		CHECK(held.use_count() == 2); // The manager's and the test's

		// An asset unloaded since: the replay changes nothing, and the frame looks them up one by one.
		manager->UnloadAsset(first);
		const uint64_t before = manager->GetStats().GetAssetCalls;
		cache.BeginFrame(manager);
		CHECK_FALSE(cache.Replay(recording));
		CHECK(manager->GetStats().GetAssetCalls == before);
		CHECK(cache.GetStats().GetAssetCalls == 0);
		// A recording of a frame that found an asset loading cannot be replayed.
		cache.BeginRecording();
		CHECK(cache.Resolve<Material>(second, AssetPriority::Normal));
		CHECK_FALSE(cache.Resolve<Material>(first, AssetPriority::Normal));
		AssetResolveCache::Recording loading;
		CHECK_FALSE(cache.EndRecording(loading));
		CHECK(loading.IsEmpty());
		cache.EndFrame();

		// Loaded again, it is another object: the old recording no longer replays.
		REQUIRE(manager->WaitForPendingLoads());
		cache.BeginFrame(manager);
		CHECK_FALSE(cache.Replay(recording));
		cache.EndFrame();
	}
}
