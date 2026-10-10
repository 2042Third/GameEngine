#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Perf/PerfUtils.h"
#include "Perf/StressProject.h"
#include "Renderer/GPUTestUtils.h"
#include "Strata/Asset/AssetPack.h"
#include "Strata/Asset/EditorAssetManager.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Renderer/Renderer.h"
#include "TestHelpers.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

// Asset packs version 2 on the streaming stress project (CTest StrataTests.PerfGPU: Release and Dist, with a GPU): how
// much smaller than version 1 they are before textures are block-compressed, how fast they open, and how fast the world
// streams from them compared with version 1.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr double c_MiB = 1024.0 * 1024.0;
	// The acceptance criteria of pack version 2 (streaming takes at most 110% of version 1's time: the budget of
	// PerfGPU.Pack.StreamStressWorldRatio).
	constexpr double c_MaxSizeRatio = 0.8;
	constexpr double c_MaxOpenMs = 5.0;
	// Streams of each version, alternating, so that the machine's other work hits both alike; the medians count.
	constexpr uint32_t c_StreamingRuns = 5;

	template<typename T>
	T Median(std::vector<T> values)
	{
		std::sort(values.begin(), values.end());
		return values[values.size() / 2];
	}

	// Loads the given assets (the world scene and everything it uses) from a pack with a RuntimeAssetManager, pinned in the
	// order given, running device frames like an application (the uploads go through staging that frames recycle), as fast
	// as the frames go. Version 1 packs have no dependency table, so both versions get the same list. Returns milliseconds.
	double StreamAssets(GraphicsDevice& device, const std::filesystem::path& pack, const std::vector<AssetHandle>& assets)
	{
		std::string error;
		Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack, &error);
		REQUIRE_MESSAGE(manager, error);
		const auto start = std::chrono::steady_clock::now();
		std::vector<AssetPin> pins;
		for (AssetHandle asset : assets)
			pins.push_back(manager->Pin(asset, AssetPriority::High));
		const auto allReady = [&]()
		{
			return std::all_of(assets.begin(), assets.end(), [&](AssetHandle asset) { return manager->GetAssetState(asset) == AssetState::Ready; });
		};
		const auto deadline = start + std::chrono::minutes(2);
		while (!allReady())
		{
			REQUIRE(std::chrono::steady_clock::now() < deadline);
			REQUIRE(device.BeginFrame());
			Renderer::BeginFrame();
			manager->Update();
			device.EndFrame();
		}
		device.WaitForIdle();
		const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		CHECK(manager->GetStats().FailedAssets == 0);
		return milliseconds;
	}

}

TEST_SUITE("PerfGPU.Pack")
{
	TEST_CASE("Version 2 packs of the stress project are smaller, open in milliseconds and stream as fast as version 1")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("PackPerf");
		const StressProjectSpec spec;
		std::string error;
		std::optional<StressProject> project;
		const std::filesystem::path packV2 = directory / "StressV2.stpak";
		const std::filesystem::path packV1 = directory / "StressV1.stpak";
		ScopedJobSystem jobs(0, 2); // The default pools: a worker per hardware thread but one, two I/O threads
		{
			project = WriteStressProject(directory / "Stress", spec, error);
			REQUIRE_MESSAGE(project, error);
			const auto start = std::chrono::steady_clock::now();
			REQUIRE_MESSAGE(BuildStressPack(*project, packV2, error), error);
			MESSAGE("Imported and packed (version 2) in ", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), " s");

			// The same assets in the same order as version 1, from the import the pack build left in the cache.
			EditorAssetManagerSpecification specification;
			specification.AssetDirectory = project->Directory / "Assets";
			specification.CacheDirectory = project->Directory / ".strata" / "Cache";
			specification.WatchFiles = false;
			Ref<EditorAssetManager> assets = CreateRef<EditorAssetManager>(specification);
			assets->Scan();
			AssetPackBuildOptions options;
			options.StartScene = project->WorldScene;
			AssetPackBuildPlan plan;
			REQUIRE_MESSAGE(assets->PlanAssetPack(options, plan, &error), error);
			AssetPackWriteOptions version1;
			version1.Version = 1;
			REQUIRE_MESSAGE(assets->WriteAssetPack(packV1, plan, version1, nullptr, &error), error);
		}

		// Size, before textures are block-compressed: zstd blocks against the raw stored forms.
		const uint64_t sizeV1 = FileSystem::GetFileSize(packV1).value_or(0);
		const uint64_t sizeV2 = FileSystem::GetFileSize(packV2).value_or(0);
		REQUIRE(sizeV1 > 0);
		const double ratio = static_cast<double>(sizeV2) / static_cast<double>(sizeV1);
		MESSAGE("Stress pack: version 1 ", sizeV1, " bytes, version 2 ", sizeV2, " bytes (", ratio, " of version 1)");
		CHECK(ratio <= c_MaxSizeRatio);
		Perf::CheckBudget("PerfGPU.Pack.StressPackSize", static_cast<double>(sizeV2) / c_MiB);

		// Opening reads the header and the tables, nothing of the data. It takes tens of microseconds, mostly opening the
		// file, so a scheduling hiccup of the machine moves even the median: the budget judges the fastest of the opens.
		const Perf::Measurement open = Perf::Measure("Open the stress pack", 3, 30, [&]()
		{
			const Scope<AssetPack> pack = AssetPack::Open(packV2, &error);
			REQUIRE(pack);
		});
		CHECK(open.MedianMs <= c_MaxOpenMs);
		Perf::CheckBudget("PerfGPU.Pack.OpenStressPack", open.MinMs);

		// Streaming the whole world, decompressing and checking every block, against reading raw version 1 entries.
		std::vector<AssetHandle> world;
		{
			const Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(packV2, &error);
			REQUIRE_MESSAGE(manager, error);
			world = manager->GetDependencyClosure(project->WorldScene);
		}
		REQUIRE(world.size() > 30); // The scene, its materials and textures, the terrain's model and mesh
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		std::vector<double> streamV1;
		std::vector<double> streamV2;
		for (uint32_t run = 0; run < c_StreamingRuns; run++)
		{
			streamV1.push_back(StreamAssets(gpu.GetDevice(), packV1, world));
			streamV2.push_back(StreamAssets(gpu.GetDevice(), packV2, world));
		}
		const double medianV1 = Median(streamV1);
		const double medianV2 = Median(streamV2);
		MESSAGE("Streaming the world: version 1 ", medianV1, " ms, version 2 ", medianV2, " ms (median of ", c_StreamingRuns, ")");
		Perf::CheckBudget("PerfGPU.Pack.StreamStressWorldRatio", medianV2 / medianV1);
		Perf::CheckBudget("PerfGPU.Pack.StreamStressWorld", medianV2);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
