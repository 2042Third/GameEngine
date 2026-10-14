#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Perf/WorldFixture.h"
#include "TestHelpers.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>

#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

// Perf tests of world-scale I/O (Perf/WorldFixture.h; CTest StrataTests.Perf: Release and Dist only, see
// Docs/DeveloperGuide.md, "Perf tests"). Registration: EditorAssetManager::Scan (blocking, as tools and scripted runs scan)
// of a fresh copy of a project of 10,000 materials and a scene that uses them all, with their committed .meta files and no
// cache (a fresh clone), then a second scan of the same copy (the next start of the editor). One copy per attempt.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr const char* c_FirstScan10k = "Perf.Asset.FirstScan10k";
	constexpr const char* c_WarmScan10k = "Perf.Asset.WarmScan10k";
	constexpr uint32_t c_ScanMaterials = 10'000;

	double MillisecondsSince(std::chrono::steady_clock::time_point start)
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	// One scan of the project's assets by a manager of its own, as the editor makes one when the project opens.
	double TimeScan(const std::filesystem::path& projectDirectory, size_t expectedAssets)
	{
		EditorAssetManagerSpecification specification;
		specification.AssetDirectory = projectDirectory / "Assets";
		specification.CacheDirectory = projectDirectory / ".strata" / "Cache";
		specification.WatchFiles = false;
		EditorAssetManager manager(specification);
		const auto start = std::chrono::steady_clock::now();
		manager.Scan();
		const double milliseconds = MillisecondsSince(start);
		CHECK(manager.GetAllMetadata().size() >= expectedAssets);
		CHECK_FALSE(manager.HasPendingImports());
		return milliseconds;
	}

}

TEST_SUITE("Perf.Asset")
{
	TEST_CASE("The first and the second scan of a project of 10,000 materials")
	{
		const Perf::ScopedApplicationJobSystem jobs;
		const std::filesystem::path root = CreateTemporaryDirectory("PerfScan10k");
		std::string error;
		const std::optional<ManyAssetsProject> project = WriteManyAssetsProject(root / "Template", c_ScanMaterials, error);
		REQUIRE_MESSAGE(project.has_value(), error);
		uint32_t attempt = 0;
		Perf::CheckBudgetsConfirmed({ c_FirstScan10k, c_WarmScan10k }, [&]()
		{
			// A fresh copy: the committed files only, as a clone has them.
			const std::filesystem::path copy = root / fmt::format("Copy{}", attempt++);
			std::error_code copyError;
			std::filesystem::copy(project->Directory, copy, std::filesystem::copy_options::recursive, copyError);
			REQUIRE_MESSAGE(!copyError, copyError.message());
			// Local editor data (the fixture's writer leaves an empty cache) is no part of a clone.
			std::filesystem::remove_all(copy / ".strata", copyError);
			REQUIRE_FALSE(FileSystem::Exists(copy / ".strata"));
			// The materials, the scene and the folders' assets.
			const double first = TimeScan(copy, c_ScanMaterials + 1);
			const double warm = TimeScan(copy, c_ScanMaterials + 1);
			MESSAGE(fmt::format("Scans of 10,000 materials: first {:.0f} ms, second {:.0f} ms", first, warm));
			return Perf::MetricValues { { c_FirstScan10k, first }, { c_WarmScan10k, warm } };
		});
	}
}
