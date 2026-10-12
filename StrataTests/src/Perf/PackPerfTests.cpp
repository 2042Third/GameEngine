#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Perf/PerfUtils.h"
#include "Perf/StressProject.h"
#include "Renderer/GPUTestUtils.h"
#include "Strata/Asset/AssetPack.h"
#include "Strata/Asset/EditorAssetManager.h"
#include "Strata/Asset/RuntimeAssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Renderer/Renderer.h"
#include "TestHelpers.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

// Asset packs version 2 on the streaming stress project (CTest StrataTests.PerfGPU: Release and Dist, with a GPU): how
// much smaller than version 1 they are before textures are block-compressed, how fast they open, and how fast the world
// streams from them, as the game ships it (block-compressed textures), compared with version 1.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

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
		// Whole textures: the versions compare on reading and decoding all of the world's bytes. With partial loads the
		// textures would be Ready with their mip tails, a few KiB each; mip streaming's reads are measured by PerfGPU.Streaming.
		manager->SetPartialLoads(false);
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

	// Writes the project's assets into a version 1 pack, in the order of the version 2 pack the editor builds, from the
	// imports BuildStressPack left in the project's cache.
	void WriteVersion1Pack(const StressProject& project, const std::filesystem::path& path)
	{
		EditorAssetManagerSpecification specification;
		specification.AssetDirectory = project.Directory / "Assets";
		specification.CacheDirectory = project.Directory / ".strata" / "Cache";
		specification.WatchFiles = false;
		Ref<EditorAssetManager> assets = CreateRef<EditorAssetManager>(specification);
		assets->Scan();
		AssetPackBuildOptions options;
		options.StartScene = project.WorldScene;
		AssetPackBuildPlan plan;
		std::string error;
		REQUIRE_MESSAGE(assets->PlanAssetPack(options, plan, &error), error);
		AssetPackWriteOptions version1;
		version1.Version = 1;
		REQUIRE_MESSAGE(assets->WriteAssetPack(path, plan, version1, nullptr, &error), error);
	}

	// Sets every texture of the project to import uncompressed (RGBA8, as before texture cooking v2), in its .meta.
	void ImportTexturesUncompressed(const StressProject& project)
	{
		uint32_t changed = 0;
		std::error_code errorCode;
		for (const std::filesystem::directory_entry& item : std::filesystem::directory_iterator(project.Directory / "Assets" / "Textures", errorCode))
		{
			const std::filesystem::path& file = item.path();
			if (file.extension() != ".meta")
				continue;
			const std::optional<std::string> text = FileSystem::ReadText(file);
			REQUIRE(text);
			std::optional<nlohmann::json> meta = JsonUtils::Parse(*text);
			REQUIRE(meta);
			(*meta)["ImportSettings"]["Compression"] = "Uncompressed";
			REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(*meta, 1, '\t') + "\n"));
			changed++;
		}
		REQUIRE_FALSE(errorCode);
		REQUIRE(changed == project.Textures.size());
	}

	uint64_t GetFileSize(const std::filesystem::path& path)
	{
		const uint64_t size = FileSystem::GetFileSize(path).value_or(0);
		REQUIRE(size > 0);
		return size;
	}

}

TEST_SUITE("PerfGPU.Pack")
{
	TEST_CASE("Version 2 packs of the stress project are smaller, open in microseconds and stream as fast as version 1")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("PackPerf");
		const StressProjectSpec spec;
		std::string error;
		std::optional<StressProject> project;
		std::optional<StressProject> uncompressed;
		const std::filesystem::path packV2 = directory / "StressV2.stpak";
		const std::filesystem::path packV1 = directory / "StressV1.stpak";
		const std::filesystem::path uncompressedV2 = directory / "UncompressedV2.stpak";
		const std::filesystem::path uncompressedV1 = directory / "UncompressedV1.stpak";
		ScopedJobSystem jobs(0, 2); // The default pools: a worker per hardware thread but one, two I/O threads
		{
			project = WriteStressProject(directory / "Stress", spec, error);
			REQUIRE_MESSAGE(project, error);
			const auto start = std::chrono::steady_clock::now();
			REQUIRE_MESSAGE(BuildStressPack(*project, packV2, error), error);
			MESSAGE("Imported and packed (version 2) in ", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), " s");
			WriteVersion1Pack(*project, packV1);

			// The same project with its textures as they were before block compression.
			uncompressed = WriteStressProject(directory / "Uncompressed", spec, error);
			REQUIRE_MESSAGE(uncompressed, error);
			ImportTexturesUncompressed(*uncompressed);
			REQUIRE_MESSAGE(BuildStressPack(*uncompressed, uncompressedV2, error), error);
			WriteVersion1Pack(*uncompressed, uncompressedV1);
		}

		// Sizes, deterministic: version 2 against version 1 before textures are block-compressed (zstd blocks against the raw
		// stored forms; the budget is the pack workstream's acceptance limit, 0.8), and as the game ships (BC7 textures
		// compress by a few percent only, so their blocks are mostly stored; Perf.TextureCook.PackSize gates that pack).
		const double uncompressedRatio = static_cast<double>(GetFileSize(uncompressedV2)) / static_cast<double>(GetFileSize(uncompressedV1));
		MESSAGE("Uncompressed textures: version 1 ", GetFileSize(uncompressedV1), " bytes, version 2 ", GetFileSize(uncompressedV2), " bytes (",
			uncompressedRatio, " of version 1); block-compressed textures: version 1 ", GetFileSize(packV1), " bytes, version 2 ", GetFileSize(packV2),
			" bytes");
		Perf::CheckBudgetConfirmed("PerfGPU.Pack.UncompressedSizeRatio", [&] { return uncompressedRatio; });

		// Opening reads the header and the tables, nothing of the data. It takes tens of microseconds, mostly opening the
		// file, so a scheduling hiccup of the machine moves even the median: the budget judges the fastest of the opens (the
		// pack workstream's acceptance limit, 5 ms, is far above it).
		Perf::CheckBudgetConfirmed("PerfGPU.Pack.OpenStressPack", [&]
		{
			const Perf::Measurement open = Perf::Measure("Open the stress pack", 3, 30, [&]()
			{
				const Scope<AssetPack> pack = AssetPack::Open(packV2, &error);
				REQUIRE(pack);
			});
			return open.MinMs;
		});

		// Streaming the whole world, checking every block and decompressing those stored compressed, against reading raw
		// version 1 entries: the medians of alternating runs, and their ratio, whose budget is the acceptance limit of 110%.
		std::vector<AssetHandle> world;
		{
			const Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(packV2, &error);
			REQUIRE_MESSAGE(manager, error);
			world = manager->GetDependencyClosure(project->WorldScene);
		}
		REQUIRE(world.size() > 30); // The scene, its materials and textures, the terrain's model and mesh
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::CheckBudgetsConfirmed({ "PerfGPU.Pack.StreamStressWorldRatio", "PerfGPU.Pack.StreamStressWorld" }, [&]
		{
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
			return Perf::MetricValues { { "PerfGPU.Pack.StreamStressWorldRatio", medianV2 / medianV1 }, { "PerfGPU.Pack.StreamStressWorld", medianV2 } };
		});
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
