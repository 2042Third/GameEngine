#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Perf/PerfUtils.h"
#include "Perf/StressProject.h"
#include "Strata/Asset/EditorAssetManager.h"
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

	// The acceptance criteria of texture cooking v2 for the stress project (16 textures of 2048^2 and one of 4096^2, which
	// cooked to 22.4 MB and 89.5 MB each, 447.4 MB in all, and a 472.7 MB pack in RGBA8).
	constexpr double c_MaxSmallTextureMB = 5.6;
	constexpr double c_MaxLargeTextureMB = 22.4;
	constexpr double c_MaxTexturesMB = 120.0;
	constexpr double c_MaxPackMB = 170.0;
	constexpr double c_MaxColdImportMs = 20000.0;

	// Cold imports timed; the median is judged.
	constexpr uint32_t c_Runs = 3;

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

		// Every run imports the whole project from an empty cache, as the editor does when it opens a project for the
		// first time: textures decode, filter their mips and encode their blocks on the workers.
		std::vector<double> importMs;
		Ref<EditorAssetManager> manager;
		for (uint32_t run = 0; run < c_Runs; run++)
		{
			manager.reset();
			const std::filesystem::path intermediate = project->Directory / ".strata";
			REQUIRE((!FileSystem::Exists(intermediate) || FileSystem::Remove(intermediate)));
			const auto start = std::chrono::steady_clock::now();
			manager = ImportStressProject(*project, error);
			importMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
			REQUIRE_MESSAGE(manager, error);
		}
		std::sort(importMs.begin(), importMs.end());
		const double medianImportMs = importMs[importMs.size() / 2];

		double totalMB = 0.0;
		for (size_t index = 0; index < project->Textures.size(); index++)
		{
			CAPTURE(index);
			const std::optional<AssetMetadata> metadata = manager->GetMetadata(project->Textures[index]);
			REQUIRE(metadata);
			const double storedMB = static_cast<double>(metadata->StoredSize) / c_MB;
			const bool large = index >= spec.Textures2k;
			CHECK(storedMB <= (large ? c_MaxLargeTextureMB : c_MaxSmallTextureMB));
			totalMB += storedMB;
		}
		const std::filesystem::path pack = directory / "Stress.stpak";
		REQUIRE_MESSAGE(manager->BuildAssetPack(pack, &error), error);
		const std::optional<uint64_t> packBytes = FileSystem::GetFileSize(pack);
		REQUIRE(packBytes);
		const double packMB = static_cast<double>(*packBytes) / c_MB;
		MESSAGE("Cold imports with ", JobSystem::GetWorkerThreadCount(), " workers: ", importMs[0], " / ", medianImportMs, " / ", importMs.back(),
			" ms (min / median / max); textures stored in ", totalMB, " MB, pack ", packMB, " MB");

		CHECK(totalMB <= c_MaxTexturesMB);
		CHECK(packMB <= c_MaxPackMB);
		CHECK(medianImportMs <= c_MaxColdImportMs);
		Perf::CheckBudget("Perf.TextureCook.ColdImport", medianImportMs);
		Perf::CheckBudget("Perf.TextureCook.StoredTextures", totalMB);
		Perf::CheckBudget("Perf.TextureCook.PackSize", packMB);
	}
}
