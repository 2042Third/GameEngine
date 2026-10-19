#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Perf/ImportMemoryMeasurement.h"
#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Perf/WorldFixture.h"
#include "Scene/SceneLoaderTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/SceneLoader.h>
#include <Strata/Scene/SceneReadProgress.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

// Perf tests of world-scale I/O (Perf/WorldFixture.h; CTest StrataTests.Perf: Release and Dist only, see
// Docs/DeveloperGuide.md, "Perf tests"). Registration: EditorAssetManager::Scan (blocking, as tools and scripted runs scan)
// of a fresh copy of a project of 10,000 materials and a scene that uses them all, with their committed .meta files and no
// cache (a fresh clone), then a second scan of the same copy (the next start of the editor). One copy per attempt.
//
// Scene reads: SceneLoader loads from a manager of their own (FakeAssetManager, the document in memory). A load of the
// realistic 1M world (its file as the fixture writes it) cancelled while its document is read, and a load of a document over
// the entity limit.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr const char* c_CancelLoad1M = "Perf.Scene.CancelLoad1M";
	constexpr const char* c_RefuseOverCap1M = "Perf.Scene.RefuseOverCap1M";
	// The audit's document over the limit: a tenth more than a million entities.
	constexpr uint32_t c_OverCapEntities = 1'100'445;
	constexpr AssetHandle c_SceneHandle = UUID(0x5CE7E0000000001ull);

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

namespace
{

	// The realistic 1M world's document, as WriteWorldProject writes it (written once per run, on the caller's job system).
	const std::vector<uint8_t>& GetWorld1MDocument()
	{
		static const std::vector<uint8_t> s_Document = []()
		{
			const std::filesystem::path root = CreateTemporaryDirectory("PerfWorld1MDocument");
			std::string error;
			// The callers' job system writes it.
			const std::optional<WorldProject> project = WriteWorldProject(root / "World", WorldProjectSpec { { c_World1M } }, error);
			REQUIRE_MESSAGE(project.has_value(), error);
			std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(project->Scenes.front().Path);
			REQUIRE(bytes.has_value());
			std::error_code removeError;
			std::filesystem::remove_all(root, removeError);
			return std::move(*bytes);
		}();
		return s_Document;
	}

	// A document of `count` small entities in the writer's layout (an entity per line), with or without EntityCount.
	std::vector<uint8_t> MakeLargeDocument(uint32_t count, bool declare)
	{
		std::string text = "{\"Strata\":{\"Format\":\"Scene\",\"Version\":1},\"Scene\":{\"Name\":\"Over\",";
		if (declare)
			text += fmt::format("\"EntityCount\":{},", count);
		text += "\"Entities\":[\n";
		for (uint32_t index = 0; index < count; index++)
		{
			text += "{\"Components\":{\"Name\":{\"Name\":\"E\"}},\"ID\":";
			text += JsonUtils::Dump(UUIDToJson(UUID(index + 1)));
			text += index + 1 < count ? "},\n" : "}\n";
		}
		text += "]}}\n";
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// Polls the load until `done`, advancing the manager and the request; false after a minute.
	template<typename Condition>
	bool PollLoad(FakeAssetManager& manager, SceneLoadRequest& request, Condition&& done)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
		while (!done())
		{
			if (std::chrono::steady_clock::now() > deadline)
				return false;
			manager.Update();
			request.Update();
			std::this_thread::yield();
		}
		return true;
	}

	// Loads the document through SceneLoader until its read reached `share` of its progress, then cancels the load: the
	// milliseconds from the cancel until the read ended (its loader returned).
	double TimeCancelledLoad(const std::vector<uint8_t>& document, float share)
	{
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		ScopedActiveManager active(manager);
		const AssetHandle scene = manager->AddStored(c_SceneHandle, AssetType::Scene, document);
		SceneLoadRequest request = SceneLoader::LoadAsync(scene);
		REQUIRE(request.GetState() == SceneLoadState::LoadingScene);
		REQUIRE(PollLoad(*manager, request, [&]() { return SceneReadProgress::Get(scene).value_or(0.0f) >= share; }));
		const auto start = std::chrono::steady_clock::now();
		request.Cancel();
		while (SceneReadProgress::Get(scene).has_value())
			std::this_thread::yield();
		const double milliseconds = MillisecondsSince(start);
		CHECK(manager->GetAssetState(scene) != AssetState::Ready);
		return milliseconds;
	}

	// Loads the document through SceneLoader until it fails: the milliseconds from the load's start.
	double TimeRefusedLoad(const std::vector<uint8_t>& document)
	{
		Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		ScopedActiveManager active(manager);
		const AssetHandle scene = manager->AddStored(c_SceneHandle, AssetType::Scene, document);
		const auto start = std::chrono::steady_clock::now();
		SceneLoadRequest request = SceneLoader::LoadAsync(scene);
		REQUIRE(PollLoad(*manager, request, [&]() { return request.IsFinished(); }));
		const double milliseconds = MillisecondsSince(start);
		CHECK(request.GetState() == SceneLoadState::Failed);
		INFO(request.GetError());
		CHECK(request.GetError().find("that fit") != std::string::npos);
		return milliseconds;
	}

}

TEST_SUITE("Perf.Scene")
{
	TEST_CASE("A load of the realistic 1M world cancelled at a quarter, half and three quarters of its read ends at once")
	{
		const Perf::ScopedApplicationJobSystem jobs;
		const std::vector<uint8_t>& document = GetWorld1MDocument();
		// The read's progress: the text parsed (the entities created as it goes) up to 0.8, then the links.
		Perf::CheckBudgetConfirmed(c_CancelLoad1M, [&]()
		{
			double slowest = 0.0;
			for (const float read : { 0.25f, 0.5f, 0.75f })
			{
				const double milliseconds = TimeCancelledLoad(document, 0.8f * read);
				MESSAGE(fmt::format("Cancelled at {:.0f}% of the read: the read ended {:.1f} ms later", read * 100.0f, milliseconds));
				slowest = std::max(slowest, milliseconds);
			}
			return slowest;
		});
	}

	TEST_CASE("A load of a document over the entity limit fails at once, also without its count")
	{
		const Perf::ScopedApplicationJobSystem jobs;
		const std::vector<uint8_t> declared = MakeLargeDocument(c_OverCapEntities, true);
		Perf::CheckBudgetConfirmed(c_RefuseOverCap1M, [&]()
		{
			std::vector<double> times;
			for (int load = 0; load < 3; load++)
				times.push_back(TimeRefusedLoad(declared));
			std::sort(times.begin(), times.end());
			return times[1];
		});
		const double undeclared = TimeRefusedLoad(MakeLargeDocument(c_OverCapEntities, false));
		MESSAGE(fmt::format("Without EntityCount the load failed after {:.1f} ms", undeclared));
	}
}

TEST_SUITE("Perf.Editor")
{
	TEST_CASE("Importing 24 textures of 4096x4096 stays within the import memory budget")
	{
		// The budget given (the default, a quarter of physical memory, would not bind on a large machine), plus 256 MB for the
		// cache's writes and the manager's own work.
		constexpr uint64_t c_Budget = 1024ull << 20;
		const std::filesystem::path root = CreateTemporaryDirectory("PerfImportFolder24x4k");
		std::string error;
		{
			const Perf::ScopedApplicationJobSystem jobs;
			REQUIRE_MESSAGE(WriteTextureFolder(root / "Outside" / "Textures", 24, 4096, 1, error).has_value(), error);
		}
		uint32_t attempt = 0;
		Perf::CheckBudgetConfirmed("Perf.Editor.ImportFolder24x4kPeakPrivate", [&]()
		{
			const std::optional<ImportMemoryMeasurement> measurement = MeasureFolderImportInHelper(root / fmt::format("Project{}", attempt++),
				root / "Outside" / "Textures", c_Budget, error);
			REQUIRE_MESSAGE(measurement.has_value(), error);
			CHECK(measurement->Imported == 24);
			MESSAGE(fmt::format("24 textures of 4096x4096 imported in {:.1f} s, peak {:.0f} MB over the {:.0f} MB before", measurement->Seconds,
				measurement->PeakPrivateIncreaseMB, measurement->PrivateMBBefore));
			return measurement->PeakPrivateIncreaseMB;
		});
	}
}
