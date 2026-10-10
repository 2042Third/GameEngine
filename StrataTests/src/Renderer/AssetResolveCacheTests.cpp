#include <doctest/doctest.h>

#include "Strata/Asset/AssetPack.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Renderer/AssetResolveCache.h"
#include "Strata/Renderer/Material.h"
#include "TestHelpers.h"

#include <algorithm>
#include <map>
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
		REQUIRE(AssetPack::Write(path, metadata, [&documents](const AssetMetadata& asset, std::vector<uint8_t>& outData, std::string*)
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
	TEST_CASE("Each asset is looked up once per frame, and its reference is let go when the next frame starts")
	{
		const AssetHandle first(0xC001);
		const AssetHandle second(0xC002);
		const Ref<RuntimeAssetManager> manager = CreateManager(first, second);
		AssetResolveCache cache;
		std::vector<AssetHandle> changed;
		bool allChanged = false;
		cache.BeginFrame(manager, changed, allChanged);
		CHECK(allChanged); // A new manager: anything may have changed

		// Still loading: null, counted as pending, requested once however often it is resolved.
		CHECK_FALSE(cache.Resolve<Material>(first, AssetPriority::High));
		CHECK_FALSE(cache.Resolve<Material>(first, AssetPriority::High));
		CHECK(cache.GetStats().GetAssetCalls == 1);
		CHECK(cache.GetStats().Requested == 1);
		CHECK(cache.GetStats().Pending == 1);
		CHECK(manager->GetAssetState(first) == AssetState::Loading);
		REQUIRE(manager->WaitForPendingLoads());

		cache.BeginFrame(manager, changed, allChanged);
		CHECK_FALSE(allChanged);
		CHECK(std::find(changed.begin(), changed.end(), first) != changed.end()); // Published by the load
		Ref<Material> material = cache.Resolve<Material>(first, AssetPriority::Normal);
		REQUIRE(material);
		CHECK(cache.Resolve<Material>(first, AssetPriority::Normal) == material);
		CHECK(cache.GetStats().GetAssetCalls == 1);
		CHECK(cache.GetStats().Pending == 0);
		const long held = material.use_count();
		material = nullptr;

		// The next frame drops the cache's reference: an asset no frame uses again can be evicted.
		changed.clear();
		cache.BeginFrame(manager, changed, allChanged);
		CHECK(changed.empty());
		material = cache.Resolve<Material>(first, AssetPriority::Normal);
		CHECK(material.use_count() == held);
		material = nullptr;
		cache.BeginFrame(manager, changed, allChanged);
		manager->Update();
		manager->Update();
		CHECK(manager->TrimUnused(1) == 1);
		CHECK(manager->GetAssetState(first) == AssetState::Unloaded);
		changed.clear();
		cache.BeginFrame(manager, changed, allChanged);
		CHECK(std::find(changed.begin(), changed.end(), first) != changed.end()); // The eviction is a content change
	}

	TEST_CASE("Resident lookups never start loads; prefetches request at low priority")
	{
		const AssetHandle first(0xC011);
		const AssetHandle second(0xC012);
		const Ref<RuntimeAssetManager> manager = CreateManager(first, second);
		AssetResolveCache cache;
		std::vector<AssetHandle> changed;
		bool allChanged = false;
		cache.BeginFrame(manager, changed, allChanged);

		CHECK_FALSE(cache.FindResident<Material>(first));
		CHECK(manager->GetAssetState(first) == AssetState::Unloaded);
		CHECK(cache.GetStats().ResidentLookups == 1);
		CHECK(cache.GetStats().GetAssetCalls == 0);
		// A request later in the same frame still asks the manager.
		CHECK_FALSE(cache.Resolve<Material>(first, AssetPriority::Normal));
		CHECK(manager->GetAssetState(first) == AssetState::Loading);

		cache.Prefetch(second);
		cache.Prefetch(second);
		CHECK(cache.GetStats().Prefetched == 1);
		CHECK(manager->GetAssetState(second) == AssetState::Loading);
		REQUIRE(manager->WaitForPendingLoads());

		cache.BeginFrame(manager, changed, allChanged);
		CHECK(cache.FindResident<Material>(second));
		CHECK(cache.GetStats().GetAssetCalls == 1);
		// Unknown handles and other types resolve to nothing.
		CHECK_FALSE(cache.Resolve<Material>(UUID(0xBAD), AssetPriority::Normal));
		CHECK(cache.GetStats().Pending == 0);

		// Without a manager nothing resolves.
		cache.BeginFrame(nullptr, changed, allChanged);
		CHECK(allChanged);
		CHECK_FALSE(cache.Resolve<Material>(first, AssetPriority::Normal));
		CHECK(cache.GetStats().GetAssetCalls == 0);
	}
}
