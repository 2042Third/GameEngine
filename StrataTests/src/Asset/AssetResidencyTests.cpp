#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"

#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint64_t c_KB = 1024;
	constexpr uint64_t c_MB = 1024 * 1024;

	Ref<FakeAssetManager> CreateManager(const AssetResidencyBudgets& budgets)
	{
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		manager->SetResidencyBudgets(budgets);
		return manager;
	}

	AssetResidencyBudgets MakeBudgets(uint64_t cpu, uint64_t gpuTextures, uint64_t gpuBuffers)
	{
		AssetResidencyBudgets budgets;
		budgets.Cpu = cpu;
		budgets.GpuTextures = gpuTextures;
		budgets.GpuBuffers = gpuBuffers;
		return budgets;
	}

	// Requests the assets and finalizes them (loads run inline without the job system; Update publishes them).
	void LoadAll(AssetManagerBase& manager, const std::vector<AssetHandle>& handles)
	{
		for (AssetHandle handle : handles)
			manager.RequestLoad(handle);
		manager.Update();
		for (AssetHandle handle : handles)
			REQUIRE(manager.GetAssetState(handle) == AssetState::Ready);
	}

}

TEST_SUITE("Asset.Residency")
{
	TEST_CASE("Default budgets come from the graphics device's memory budget")
	{
		const AssetResidencyBudgets unlimited = AssetResidencyBudgets::FromDeviceBudget(0);
		for (AssetMemoryPool pool : c_AssetMemoryPools)
			CHECK(unlimited.GetPoolBudget(pool) == AssetResidencyBudgets::c_Unlimited);
		CHECK(unlimited.InFlightBytes == 128 * c_MB);
		CHECK(unlimited.UploadBytesPerFrame == 64 * c_MB);
		CHECK(unlimited.FinalizeMsPerFrame == doctest::Approx(4.0f));

		const AssetResidencyBudgets device = AssetResidencyBudgets::FromDeviceBudget(8000 * c_MB);
		CHECK(device.GpuTextures == 4000 * c_MB);
		CHECK(device.GpuBuffers == 1200 * c_MB);
		CHECK(device.Cpu == AssetResidencyBudgets::c_Unlimited);

		CHECK(device.GetPoolBudget(AssetMemoryPool::GpuTextures) == 4000 * c_MB);
		CHECK(device.GetPoolBudget(AssetMemoryPool::GpuBuffers) == 1200 * c_MB);
		CHECK(GetPoolBytes(MakeUsage(1, 2, 3), AssetMemoryPool::Cpu) == 1);
		CHECK(GetPoolBytes(MakeUsage(1, 2, 3), AssetMemoryPool::GpuTextures) == 2);
		CHECK(GetPoolBytes(MakeUsage(1, 2, 3), AssetMemoryPool::GpuBuffers) == 3);

		// Without a renderer, managers start without pool limits.
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		CHECK(manager->GetResidencyBudgets().GpuTextures == AssetManagerBase::GetDefaultResidencyBudgets().GpuTextures);
		CHECK(manager->GetEvictionGraceFrames() >= 3);
	}

	TEST_CASE("Assets report their memory per pool")
	{
		CHECK(Material::Create()->GetMemoryUsage() == MakeUsage(sizeof(Material)));

		// Without a renderer, geometry stays on the CPU only.
		const Ref<Mesh> mesh = MeshFactory::CreateSphere(1.0f, 16, 8);
		const uint64_t geometry = mesh->GetPositions().size() * sizeof(glm::vec3) + mesh->GetAttributes().size() * sizeof(MeshVertexAttributes)
			+ mesh->GetIndices().size() * sizeof(uint32_t);
		CHECK(mesh->GetMemoryUsage().Cpu >= geometry);
		CHECK(mesh->GetMemoryUsage().Cpu < geometry + 1024);
		CHECK(mesh->GetMemoryUsage().GetGpu() == 0);

		// Documents count what the parsed JSON occupies, which grows with what they hold.
		const auto makeSnapshot = [](int entities)
		{
			Scene scene;
			std::vector<Entity> roots;
			for (int index = 0; index < entities; index++)
				roots.push_back(scene.CreateEntity("Entity " + std::to_string(index)));
			return SceneSerializer::SerializeEntities(scene, roots);
		};
		const Ref<Prefab> small = Prefab::CreateFromSnapshot(makeSnapshot(1));
		const Ref<Prefab> large = Prefab::CreateFromSnapshot(makeSnapshot(100));
		CHECK(small->GetMemoryUsage().Cpu > 0);
		CHECK(large->GetMemoryUsage().Cpu > 50 * small->GetMemoryUsage().Cpu);
		CHECK(large->GetMemoryUsage().Cpu > JsonUtils::Dump(large->GetSnapshot()).size()); // Parsed JSON takes more than its text
		CHECK(large->GetMemoryUsage().GetGpu() == 0);
		const Ref<Model> model = Model::CreateFromSnapshot(makeSnapshot(10));
		CHECK(model->GetMemoryUsage().Cpu > small->GetMemoryUsage().Cpu);

		Scene scene("Document");
		for (int index = 0; index < 20; index++)
			scene.CreateEntity("Entity " + std::to_string(index)).AddComponent<TagComponent>().Tag = "Tagged";
		const std::string text = JsonUtils::Dump(SceneSerializer::Serialize(scene));
		std::string error;
		const Ref<SceneAsset> sceneAsset = SceneAsset::Deserialize(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()), &error);
		REQUIRE_MESSAGE(sceneAsset, error);
		CHECK(sceneAsset->GetMemoryUsage().Cpu > text.size());
	}

	TEST_CASE("Resident memory stays within the budgets plus pinned and recently requested assets")
	{
		ScopedFakeLoader loader;
		const AssetResidencyBudgets budgets = MakeBudgets(512 * c_KB, 1 * c_MB, 384 * c_KB);
		Ref<FakeAssetManager> manager = CreateManager(budgets);
		const uint32_t grace = manager->GetEvictionGraceFrames();

		std::mt19937 random(20261009);
		const auto pick = [&random](uint64_t below) { return static_cast<uint64_t>(random() % below); };
		std::vector<AssetHandle> handles;
		for (uint64_t index = 0; index < 96; index++)
		{
			const AssetMemoryUsage usage = MakeUsage(1 + pick(64 * c_KB), pick(3) == 0 ? 0 : 1 + pick(256 * c_KB), pick(2) == 0 ? 0 : 1 + pick(96 * c_KB));
			handles.push_back(manager->Add(0x10000 + index, usage, pick(4) == 0 ? 0 : 1 + pick(512 * c_KB)));
		}

		std::vector<AssetPin> pins;
		uint64_t violations = 0;
		uint64_t evictedPinned = 0;
		uint64_t evictedRequested = 0;
		for (int step = 0; step < 10000; step++)
		{
			// The requests of this frame: they must all survive the update that follows.
			std::set<AssetHandle> requested;
			for (uint64_t request = pick(7); request > 0; request--)
			{
				const AssetHandle handle = handles[pick(handles.size())];
				if (pick(2) == 0)
					manager->GetAsset(handle, static_cast<AssetPriority>(pick(3)));
				else
					manager->RequestLoad(handle, static_cast<AssetPriority>(pick(3)), static_cast<float>(pick(100)));
				requested.insert(handle);
			}
			if (pick(40) == 0)
				pins.push_back(manager->Pin(handles[pick(handles.size())], AssetPriority::Low));
			if (!pins.empty() && pick(45) == 0)
				pins.erase(pins.begin() + static_cast<std::ptrdiff_t>(pick(pins.size())));
			if (pick(400) == 0)
				manager->TrimUnused(static_cast<uint32_t>(pick(8)));

			std::set<AssetHandle> readyBefore;
			for (AssetHandle handle : handles)
			{
				if (manager->GetAssetState(handle) == AssetState::Ready)
					readyBefore.insert(handle);
			}
			manager->Update();

			// Only eviction makes a ready asset leave between these two points.
			for (AssetHandle handle : readyBefore)
			{
				if (manager->GetAssetState(handle) == AssetState::Ready)
					continue;
				evictedPinned += manager->GetPinCount(handle) > 0 ? 1 : 0;
				evictedRequested += requested.count(handle);
			}

			const uint64_t frame = manager->GetFrameIndex();
			AssetMemoryUsage resident;
			AssetMemoryUsage protectedUsage;
			for (const AssetResidencyInfo& asset : manager->GetResidencyInfo())
			{
				resident += asset.Usage;
				const bool recent = frame - asset.LastRequestedFrame <= grace;
				if (asset.PinCount > 0 || asset.IsMemoryAsset || recent)
					protectedUsage += asset.Usage;
			}

			const AssetManagerStats stats = manager->GetStats();
			if (stats.Resident != resident)
				violations++;
			for (AssetMemoryPool pool : c_AssetMemoryPools)
			{
				if (GetPoolBytes(resident, pool) > budgets.GetPoolBudget(pool) + GetPoolBytes(protectedUsage, pool))
					violations++;
			}
		}
		CHECK(violations == 0);
		CHECK(evictedPinned == 0);
		CHECK(evictedRequested == 0);
		const AssetManagerStats stats = manager->GetStats();
		CHECK(stats.Evictions > 100); // The budgets were under pressure throughout
		CHECK(stats.TotalLoadsCompleted > stats.Evictions);
		CHECK(stats.LoadedMemory == stats.Resident.GetTotal());
		pins.clear();
	}

	TEST_CASE("Eviction takes the least recently requested assets first")
	{
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateManager(MakeBudgets(AssetResidencyBudgets::c_Unlimited, 5 * c_MB, AssetResidencyBudgets::c_Unlimited));
		const uint32_t grace = manager->GetEvictionGraceFrames();
		std::vector<AssetHandle> textures;
		for (uint64_t index = 0; index < 11; index++)
			textures.push_back(manager->Add(0x20000 + index, MakeUsage(0, 1 * c_MB)));

		// One request per frame, ten frames, then enough frames for the grace window to pass.
		for (size_t index = 0; index < 10; index++)
		{
			manager->GetAsset(textures[index]);
			manager->Update();
		}
		for (uint32_t frame = 0; frame <= grace; frame++)
			manager->Update();
		for (size_t index = 0; index < 10; index++)
		{
			CAPTURE(index);
			CHECK(manager->GetAssetState(textures[index]) == (index < 5 ? AssetState::Unloaded : AssetState::Ready));
		}
		CHECK(manager->GetStats().Resident.GpuTextures == 5 * c_MB);
		CHECK(manager->GetStats().Evictions == 5);

		// A request makes an asset the most recent one: the next oldest goes instead.
		manager->GetAsset(textures[5]);
		manager->GetAsset(textures[10]);
		for (uint32_t frame = 0; frame <= grace + 1; frame++)
			manager->Update();
		CHECK(manager->GetAssetState(textures[5]) == AssetState::Ready);
		CHECK(manager->GetAssetState(textures[6]) == AssetState::Unloaded);
		for (size_t index : { size_t(7), size_t(8), size_t(9), size_t(10) })
			CHECK(manager->GetAssetState(textures[index]) == AssetState::Ready);
		CHECK(manager->GetStats().Evictions == 6);
	}

	TEST_CASE("Pinned assets and assets requested within the grace window are never evicted")
	{
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateManager(MakeBudgets(0, 0, 0)); // Every pool is over budget all the time
		const uint32_t grace = manager->GetEvictionGraceFrames();
		const AssetHandle pinned = manager->Add(0x30000, MakeUsage(1 * c_KB, 1 * c_MB));
		const AssetHandle everyFrame = manager->Add(0x30001, MakeUsage(1 * c_KB, 0, 1 * c_MB));
		const AssetHandle once = manager->Add(0x30002, MakeUsage(1 * c_KB));

		AssetPin pin = manager->Pin(pinned);
		CHECK(pin.IsValid());
		CHECK(pin.GetHandle() == pinned);
		CHECK(manager->GetPinCount(pinned) == 1);
		CHECK(manager->GetAssetState(pinned) == AssetState::Loading); // Pinning requests the load
		manager->GetAsset(everyFrame);
		const uint64_t requested = manager->GetFrameIndex();
		manager->GetAsset(once);
		manager->Update(); // All three arrive
		CHECK(manager->GetAssetState(once) == AssetState::Ready);

		// The asset requested once stays for the grace window after its request, then goes; the others stay.
		while (manager->GetFrameIndex() - requested < grace)
		{
			manager->GetAsset(everyFrame);
			manager->Update();
			CHECK(manager->GetAssetState(once) == AssetState::Ready);
		}
		manager->GetAsset(everyFrame);
		manager->Update();
		CHECK(manager->GetAssetState(once) == AssetState::Unloaded);
		CHECK(manager->GetAssetState(pinned) == AssetState::Ready);
		CHECK(manager->GetAssetState(everyFrame) == AssetState::Ready);

		// A trim that keeps nothing older than this frame still keeps what was requested in it, and the pinned asset.
		manager->GetAsset(everyFrame);
		CHECK(manager->TrimUnused(0) == 0);
		CHECK(manager->GetAssetState(everyFrame) == AssetState::Ready);
		CHECK(manager->GetAssetState(pinned) == AssetState::Ready);

		// Pins add up, move along, and end with the last of them.
		AssetPin second = manager->Pin(pinned);
		CHECK(manager->GetPinCount(pinned) == 2);
		AssetPin moved = std::move(second);
		CHECK_FALSE(second.IsValid());
		CHECK(manager->GetPinCount(pinned) == 2);
		pin.Reset();
		CHECK(manager->GetPinCount(pinned) == 1);
		manager->Update();
		CHECK(manager->TrimUnused(0) == 1); // everyFrame (not requested in this frame); the pinned asset stays
		CHECK(manager->GetAssetState(pinned) == AssetState::Ready);
		moved = AssetPin();
		CHECK(manager->GetPinCount(pinned) == 0);
		CHECK(manager->TrimUnused(0) == 1);
		CHECK(manager->GetAssetState(pinned) == AssetState::Unloaded);

		// Unknown assets cannot be pinned; a pin outliving its manager does nothing.
		CHECK_FALSE(manager->Pin(UUID(0x999)).IsValid());
		AssetPin orphan = manager->Pin(once);
		manager.reset();
		orphan.Reset();
		CHECK_FALSE(orphan.IsValid());
	}

	TEST_CASE("An evicted asset is unloaded, announced, and loads again on its next request")
	{
		ScopedFakeLoader loader;
		constexpr uint64_t c_Unlimited = AssetResidencyBudgets::c_Unlimited;
		Ref<FakeAssetManager> manager = CreateManager(MakeBudgets(c_Unlimited, c_Unlimited, c_Unlimited));
		const AssetHandle handle = manager->Add(0x40000, MakeUsage(2 * c_KB, 3 * c_KB, 4 * c_KB));
		const AssetMemoryUsage builtins = manager->GetStats().Resident;
		LoadAll(*manager, { handle });
		CHECK(manager->GetStats().Resident == AssetMemoryUsage { builtins.Cpu + 2 * c_KB, builtins.GpuTextures + 3 * c_KB, builtins.GpuBuffers + 4 * c_KB });
		const uint32_t loads = loader.GetCallCount();

		// Within the budgets nothing is evicted, however old.
		for (uint32_t frame = 0; frame < manager->GetEvictionGraceFrames() + 5; frame++)
			manager->Update();
		CHECK(manager->GetAssetState(handle) == AssetState::Ready);

		// An object held outside the manager is not evicted: dropping it would free nothing, and the next request would
		// load a second copy.
		Ref<Asset> held = manager->GetAsset(handle);
		REQUIRE(held);
		const std::weak_ptr<Asset> first = held;
		manager->SetResidencyBudgets(MakeBudgets(0, 0, 0));
		for (uint32_t frame = 0; frame < manager->GetEvictionGraceFrames() + 2; frame++)
			manager->Update();
		CHECK(manager->GetAssetState(handle) == AssetState::Ready);
		CHECK(manager->TrimUnused(0) == 0);

		// Once its holder lets go, the next update evicts it and announces the change.
		const uint64_t version = manager->GetContentVersion();
		held.reset();
		manager->Update();
		CHECK(manager->GetAssetState(handle) == AssetState::Unloaded);
		std::vector<AssetHandle> changes;
		REQUIRE(manager->GetContentChanges(version, changes));
		CHECK(changes == std::vector<AssetHandle> { handle });
		const AssetManagerStats stats = manager->GetStats();
		CHECK(stats.Resident == builtins);
		CHECK(stats.Evictions == 1);
		for (const AssetResidencyInfo& asset : manager->GetResidencyInfo())
			CHECK(asset.Handle != handle);

		// The next request loads it again: a new object.
		manager->SetResidencyBudgets(MakeBudgets(c_Unlimited, c_Unlimited, c_Unlimited));
		CHECK(manager->GetAsset(handle) == nullptr);
		manager->Update();
		const Ref<Asset> second = manager->GetAsset(handle);
		REQUIRE(second);
		CHECK(first.expired()); // The evicted object was freed; this is a new one
		CHECK(loader.GetCallCount() == loads + 1);
	}

	TEST_CASE("Built-in and memory assets are never evicted")
	{
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateManager(MakeBudgets(0, 0, 0));
		AssetMetadata metadata;
		metadata.Name = "Runtime";
		const AssetHandle memory = manager->AddMemoryAsset(Material::Create(), metadata);
		const AssetHandle loaded = manager->Add(0x50000, MakeUsage(1 * c_KB));
		LoadAll(*manager, { loaded });

		for (uint32_t frame = 0; frame < manager->GetEvictionGraceFrames() + 3; frame++)
			manager->Update();
		manager->TrimUnused(0);
		CHECK(manager->GetAssetState(loaded) == AssetState::Unloaded);
		CHECK(manager->GetAssetState(memory) == AssetState::Ready);
		for (const BuiltinAssetInfo& info : BuiltinAssets::GetAll())
		{
			CAPTURE(std::string(info.Name));
			CHECK(manager->GetAssetState(info.Handle) == AssetState::Ready);
		}

		// They count as resident all the same.
		AssetMemoryUsage expected;
		for (const AssetResidencyInfo& asset : manager->GetResidencyInfo())
		{
			CHECK(asset.IsMemoryAsset);
			expected += asset.Usage;
		}
		CHECK(manager->GetStats().Resident == expected);
		CHECK(expected.Cpu >= sizeof(Material));
	}

	TEST_CASE("A scheduled trim runs after its delay and keeps what was requested since")
	{
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>(); // No budget pressure: only the trim evicts
		const AssetHandle previousScene = manager->Add(0x60000, MakeUsage(1 * c_KB));
		const AssetHandle shared = manager->Add(0x60001, MakeUsage(1 * c_KB));
		const AssetHandle nextScene = manager->Add(0x60002, MakeUsage(1 * c_KB));
		LoadAll(*manager, { previousScene, shared });
		manager->Update();

		// The switch, before the new scene's first frame: it uses the shared asset and a new one from then on.
		manager->ScheduleTrim(AssetResidency::c_SceneSwitchTrimFrames, AssetResidency::c_SceneSwitchTrimFrames);
		for (uint32_t frame = 0; frame < AssetResidency::c_SceneSwitchTrimFrames; frame++)
		{
			manager->GetAsset(shared);
			manager->GetAsset(nextScene);
			CHECK(manager->GetAssetState(previousScene) == AssetState::Ready);
			manager->Update();
		}
		CHECK(manager->GetAssetState(previousScene) == AssetState::Unloaded);
		CHECK(manager->GetAssetState(shared) == AssetState::Ready);
		CHECK(manager->GetAssetState(nextScene) == AssetState::Ready);
		CHECK(manager->GetStats().Evictions == 1);

		// It runs once.
		for (uint32_t frame = 0; frame < 10; frame++)
			manager->Update();
		CHECK(manager->GetAssetState(shared) == AssetState::Ready);
	}

	TEST_CASE("Residency is reported per asset and in the statistics")
	{
		ScopedFakeLoader loader;
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle ready = manager->Add(0x70000, MakeUsage(5 * c_KB, 7 * c_KB));
		const AssetHandle pinnedOnly = manager->Add(0x70001, MakeUsage(1 * c_KB));
		const AssetHandle untouched = manager->Add(0x70002, MakeUsage(1 * c_KB));
		LoadAll(*manager, { ready });
		// Pinned, then unloaded on purpose: listed although it holds nothing.
		const AssetPin pin = manager->Pin(pinnedOnly);
		manager->UnloadAsset(pinnedOnly);

		bool foundReady = false;
		bool foundPinned = false;
		for (const AssetResidencyInfo& asset : manager->GetResidencyInfo())
		{
			CHECK(asset.Handle != untouched);
			if (asset.Handle == ready)
			{
				foundReady = true;
				CHECK(asset.State == AssetState::Ready);
				CHECK(asset.Usage == MakeUsage(5 * c_KB, 7 * c_KB));
				CHECK(asset.LastRequestedFrame == 0); // Requested before the first update (arriving is no request)
				CHECK(asset.Type == c_FakeAssetType);
				CHECK(asset.Path == "Fake/" + std::to_string(0x70000));
				CHECK_FALSE(asset.IsMemoryAsset);
			}
			if (asset.Handle == pinnedOnly)
			{
				foundPinned = true;
				CHECK(asset.PinCount == 1);
				CHECK(asset.State == AssetState::Unloaded);
				CHECK(asset.Usage.GetTotal() == 0);
			}
		}
		CHECK(foundReady);
		CHECK(foundPinned);

		const AssetManagerStats stats = manager->GetStats();
		CHECK(stats.PinnedAssets == 1);
		CHECK(stats.Frame == 1);
		CHECK(stats.LoadedMemory == stats.Resident.GetTotal());
		CHECK(stats.Cancellations == 1); // The pinned asset was unloaded while its load waited to be finalized
	}

	TEST_CASE("An arrival in use makes room before it is finalized; one nobody requests lately does not")
	{
		// Fake assets that note how much the manager held when they were finalized.
		std::vector<uint64_t> residentAtFinalize;
		class ProbeAsset final : public Asset
		{
		public:
			ProbeAsset(const AssetMemoryUsage& usage, std::vector<uint64_t>& residentAtFinalize)
				: m_Usage(usage), m_ResidentAtFinalize(residentAtFinalize)
			{
			}

			AssetType GetType() const override { return c_FakeAssetType; }
			AssetMemoryUsage GetMemoryUsage() const override { return m_Usage; }
			AssetFinalizeResult FinalizeOnMainThread(const AssetFinalizeContext& context) override
			{
				m_ResidentAtFinalize.push_back(context.Manager->GetStats().Resident.Cpu);
				return AssetFinalizeResult::Done;
			}
		private:
			AssetMemoryUsage m_Usage;
			std::vector<uint64_t>& m_ResidentAtFinalize;
		};
		const AssetLoadFunction* previous = AssetLoaderRegistry::Find(c_FakeAssetType);
		REQUIRE(previous);
		const AssetLoadFunction restore = *previous;
		AssetLoaderRegistry::Register(c_FakeAssetType, [&residentAtFinalize](const AssetMetadata&, std::span<const uint8_t> data, std::string*) -> Ref<Asset>
		{
			AssetMemoryUsage usage;
			std::memcpy(&usage, data.data(), std::min(data.size(), sizeof(usage)));
			return CreateRef<ProbeAsset>(usage, residentAtFinalize);
		});
		struct RestoreLoader
		{
			const AssetLoadFunction& Previous;
			~RestoreLoader() { AssetLoaderRegistry::Register(c_FakeAssetType, Previous); }
		} restoreLoader { restore };

		{
			// 100 KB of CPU memory beside the built-in assets: one 60 KB asset fits. The old one goes before the new one in use
			// is finalized.
			Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
			const uint64_t builtins = manager->GetStats().Resident.Cpu;
			const AssetHandle old = manager->Add(0x80000, MakeUsage(60 * c_KB));
			const AssetHandle arrival = manager->Add(0x80001, MakeUsage(60 * c_KB));
			manager->SetResidencyBudgets(MakeBudgets(builtins + 100 * c_KB, AssetResidencyBudgets::c_Unlimited, AssetResidencyBudgets::c_Unlimited));
			LoadAll(*manager, { old });
			for (uint32_t frame = 0; frame <= manager->GetEvictionGraceFrames(); frame++)
				manager->Update();
			REQUIRE(manager->GetAssetState(old) == AssetState::Ready);

			residentAtFinalize.clear();
			manager->GetAsset(arrival);
			manager->Update();
			CHECK(manager->GetAssetState(arrival) == AssetState::Ready);
			CHECK(manager->GetAssetState(old) == AssetState::Unloaded);
			REQUIRE(residentAtFinalize.size() == 1);
			CHECK(residentAtFinalize[0] == builtins); // The old asset was gone already
			CHECK(manager->GetStats().Evictions == 1);
		}
		{
			// An arrival whose requests stopped before it arrived is no reason to evict what is in use: it goes itself.
			ScopedJobSystem jobs(2, 1);
			Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
			const uint64_t builtins = manager->GetStats().Resident.Cpu;
			manager->SetResidencyBudgets(MakeBudgets(builtins + 100 * c_KB, AssetResidencyBudgets::c_Unlimited, AssetResidencyBudgets::c_Unlimited));
			const AssetHandle inUse = manager->Add(0x80002, MakeUsage(60 * c_KB));
			const AssetHandle late = manager->Add(0x80003, MakeUsage(60 * c_KB));
			manager->GetAsset(inUse);
			REQUIRE(manager->WaitForPendingLoads());
			REQUIRE(manager->GetAssetState(inUse) == AssetState::Ready);

			manager->CloseGate();
			manager->RequestLoad(late);
			REQUIRE(manager->WaitForWaitingReads(1));
			for (uint32_t frame = 0; frame <= manager->GetEvictionGraceFrames() + 1; frame++)
			{
				manager->GetAsset(inUse);
				manager->Update();
			}
			residentAtFinalize.clear();
			manager->OpenGate();
			REQUIRE(manager->WaitForPendingLoads()); // Finalized without a budget: still no room made for it
			manager->GetAsset(inUse);
			manager->Update();
			REQUIRE(residentAtFinalize.size() == 1);
			CHECK(residentAtFinalize[0] == builtins + 60 * c_KB); // In use, so it stayed
			CHECK(manager->GetAssetState(inUse) == AssetState::Ready);
			CHECK(manager->GetAssetState(late) == AssetState::Unloaded);
		}
	}
}

