#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Perf/PerfUtils.h"
#include "Perf/StressProject.h"
#include "Strata/Asset/EditorAssetManager.h"
#include "Strata/Asset/TextureCooker.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JobSystem.h"
#include "TestHelpers.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Decimal megabytes, like the streaming audit's numbers.
	constexpr double c_MB = 1000.0 * 1000.0;

	// The metrics of a cold import of the stress project (16 textures of 2048^2 and one of 4096^2, which cooked to 22.4 MB
	// and 89.5 MB each, 447.4 MB in all, and a 472.7 MB pack in RGBA8). The sizes are exact; the budgets of the sizes are the
	// acceptance limits of texture cooking v2 (each 2048^2 texture at most 5.6 MB, the 4096^2 one 22.4 MB, all of them
	// 120 MB) and of pack version 2 (the exported pack 140 MB, below texture cooking's 170 MB). The import time is far below
	// texture cooking's limit of 20 s.
	constexpr const char* c_ColdImport = "Perf.TextureCook.ColdImport";
	constexpr const char* c_LargestTexture2k = "Perf.TextureCook.LargestTexture2k";
	constexpr const char* c_Texture4k = "Perf.TextureCook.Texture4k";
	constexpr const char* c_StoredTextures = "Perf.TextureCook.StoredTextures";
	constexpr const char* c_PackSize = "Perf.TextureCook.PackSize";

	// BC7's throughput is measured on the top left corner of this size of the first 2048x2048 stress texture: noise over
	// a gradient, where every block is searched in full (no flat blocks, no exact fits).
	constexpr uint32_t c_ThroughputSize = 512;

}

TEST_SUITE("Perf.TextureCook")
{
	TEST_CASE("The stress project's textures cook within their size and time budgets")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("TextureCookPerf");
		const StressProjectSpec spec;
		std::string error;
		ScopedJobSystem jobs(0, 2); // The default pools: a worker per hardware thread but one, two I/O threads
		const std::optional<StressProject> project = WriteStressProject(directory / "Stress", spec, error);
		REQUIRE_MESSAGE(project, error);

		// Every attempt imports the whole project from an empty cache, as the editor does when it opens a project for the
		// first time (textures decode, filter their mips and encode their blocks on the workers; the time of that import),
		// and packs it.
		uint32_t attempt = 0;
		Perf::CheckBudgetsConfirmed({ c_ColdImport, c_LargestTexture2k, c_Texture4k, c_StoredTextures, c_PackSize }, [&]
		{
			attempt++;
			const std::filesystem::path intermediate = project->Directory / ".strata";
			REQUIRE((!FileSystem::Exists(intermediate) || FileSystem::Remove(intermediate)));
			const auto start = std::chrono::steady_clock::now();
			const Ref<EditorAssetManager> manager = ImportStressProject(*project, error);
			const double importMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			REQUIRE_MESSAGE(manager, error);

			double totalMB = 0.0;
			double largest2kMB = 0.0;
			double texture4kMB = 0.0;
			for (size_t index = 0; index < project->Textures.size(); index++)
			{
				CAPTURE(index);
				const std::optional<AssetMetadata> metadata = manager->GetMetadata(project->Textures[index]);
				REQUIRE(metadata);
				const double storedMB = static_cast<double>(metadata->StoredSize) / c_MB;
				if (index >= spec.Textures2k)
					texture4kMB = std::max(texture4kMB, storedMB);
				else
					largest2kMB = std::max(largest2kMB, storedMB);
				totalMB += storedMB;
			}
			const std::filesystem::path pack = directory / ("Stress" + std::to_string(attempt) + ".stpak");
			REQUIRE_MESSAGE(manager->BuildAssetPack(pack, &error), error);
			const std::optional<uint64_t> packBytes = FileSystem::GetFileSize(pack);
			REQUIRE(packBytes);
			const double packMB = static_cast<double>(*packBytes) / c_MB;
			MESSAGE("Cold import with ", JobSystem::GetWorkerThreadCount(), " workers: ", importMs, " ms; textures stored in ", totalMB, " MB, pack ",
				packMB, " MB");
			return Perf::MetricValues { { c_ColdImport, importMs }, { c_LargestTexture2k, largest2kMB }, { c_Texture4k, texture4kMB },
				{ c_StoredTextures, totalMB }, { c_PackSize, packMB } };
		});
	}

	TEST_CASE("BC7 encodes the stress textures' blocks at its measured rate per thread")
	{
		// Without the job system EncodeLevel runs on the calling thread, so the time per block is one thread's.
		REQUIRE_FALSE(JobSystem::IsInitialized());
		TextureCooker::Image image;
		image.Width = c_ThroughputSize;
		image.Height = c_ThroughputSize;
		image.Channels = 3;
		const StressProjectSpec spec;
		const std::vector<uint8_t> texture = CreateStressTexels(spec.SmallTextureSize, 1);
		for (uint32_t row = 0; row < c_ThroughputSize; row++)
		{
			const auto rowStart = texture.begin() + static_cast<ptrdiff_t>(row) * spec.SmallTextureSize * 3;
			image.Pixels.insert(image.Pixels.end(), rowStart, rowStart + c_ThroughputSize * 3);
		}
		const double blocks = static_cast<double>(c_ThroughputSize / 4) * static_cast<double>(c_ThroughputSize / 4);
		for (const bool highQuality : { true, false })
		{
			const std::string name = highQuality ? "Perf.TextureCook.BC7High" : "Perf.TextureCook.BC7Fast";
			std::vector<uint8_t> encoded;
			// The median encode of the crop, per block.
			Perf::CheckBudgetConfirmed(name, [&]
			{
				const Perf::Measurement measurement = Perf::Measure(name, 2, 7, [&]()
				{
					REQUIRE(TextureCooker::EncodeLevel(image, TextureFormat::BC7SRGB, highQuality, encoded));
				});
				const double microsecondsPerBlock = measurement.MedianMs * 1000.0 / blocks;
				MESSAGE(name, ": ", microsecondsPerBlock, " us per block, ", 1.0e6 / microsecondsPerBlock, " blocks per second per thread");
				return microsecondsPerBlock;
			});
		}
	}
}
