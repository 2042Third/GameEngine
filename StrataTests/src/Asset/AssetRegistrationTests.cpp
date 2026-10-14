#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/SaveTestUtils.h"
#include "Perf/WorldFixture.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetRegistration.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	EditorAssetManagerSpecification MakeSpecification(const std::filesystem::path& projectDirectory, bool watchFiles = false)
	{
		EditorAssetManagerSpecification specification;
		specification.AssetDirectory = projectDirectory / "Assets";
		specification.CacheDirectory = projectDirectory / ".strata" / "Cache";
		specification.WatchFiles = watchFiles;
		return specification;
	}

	size_t CountAssetsAt(const EditorAssetManager& manager, const std::string& path)
	{
		size_t count = 0;
		for (const AssetMetadata& metadata : manager.GetAllMetadata())
			count += metadata.Path == path && !metadata.IsSubAsset() ? 1 : 0;
		return count;
	}

}

TEST_SUITE("Asset.Registration")
{
	TEST_CASE("Duplicated handles go to the file the records name, else to the shortest path")
	{
		const std::vector<AssetHandle> handles = { UUID(1), UUID(2), UUID(1), UUID::Null(), UUID(1), UUID(2) };
		const std::vector<std::string> paths = { "Copy of Brick.png", "Moss.png", "Brick.png", "New.png", "Brick (1).png", "Moss - Copy.png" };
		// Without records: the shortest path keeps the handle.
		std::vector<DuplicateHandle> duplicates = FindDuplicateHandles(handles, paths, {});
		REQUIRE(duplicates.size() == 3);
		CHECK(duplicates[0].File == 0);
		CHECK(duplicates[0].Keeper == 2);
		CHECK(duplicates[1].File == 4);
		CHECK(duplicates[2].File == 5);
		CHECK(duplicates[2].Keeper == 1);
		// The records name where a handle belongs.
		duplicates = FindDuplicateHandles(handles, paths, { { UUID(1), "Brick (1).png" } });
		REQUIRE(duplicates.size() == 3);
		CHECK(duplicates[0].File == 0);
		CHECK(duplicates[0].Keeper == 4);
		CHECK(duplicates[1].File == 2);
	}

	TEST_CASE("A slice of registration ends once its time is up, checked every so many entries, by the given clock")
	{
		uint64_t now = 0;
		AssetRegistrationOptions options;
		options.SliceMicroseconds = 1000;
		options.EntriesPerCheck = 64;
		options.Clock = [&now]() { return now += 50; };
		RegistrationSlice slice(options, false);
		uint64_t entries = 0;
		while (slice.HasTime())
			entries++;
		// The clock is read at the start and every 64 entries, 50 microseconds later each time: the 20th read is a millisecond on.
		CHECK(entries + 1 == 64 * 20);
		CHECK_FALSE(slice.HasTime());

		RegistrationSlice unlimited(options, true);
		for (int index = 0; index < 100'000; index++)
			REQUIRE(unlimited.HasTime());
	}

	TEST_CASE("A scan of 10,000 assets is inserted in slices that each keep to the slice's budget, and touches no file on the main thread")
	{
		ScopedJobSystem jobs(4, 2);
		const std::filesystem::path root = CreateTemporaryDirectory("RegistrationSlices");
		std::string error;
		const std::optional<ManyAssetsProject> project = WriteManyAssetsProject(root / "Many", 10'000, error);
		REQUIRE_MESSAGE(project.has_value(), error);

		EditorAssetManager manager(MakeSpecification(project->Directory));
		uint64_t now = 0;
		AssetRegistrationOptions options;
		options.SliceMicroseconds = 1000;
		options.EntriesPerCheck = 64;
		options.Clock = [&now]() { return now += 50; };
		manager.SetRegistrationOptions(options);
		manager.BeginScan(ScanImports::InBackground);
		CHECK(manager.IsScanning());
		CHECK(manager.HasPendingImports());
		REQUIRE(manager.GetScanProgress().has_value());

		size_t slices = 0;
		size_t previous = manager.GetAllMetadata().size();
		AssetScanPhase lastPhase = AssetScanPhase::Walking;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
		while (manager.IsScanning())
		{
			REQUIRE(std::chrono::steady_clock::now() < deadline);
			const std::optional<AssetScanProgress> progress = manager.GetScanProgress();
			REQUIRE(progress.has_value());
			CHECK(static_cast<int>(progress->Phase) >= static_cast<int>(lastPhase));
			lastPhase = progress->Phase;

			const uint64_t operations = FileSystem::GetThreadOperationCount();
			manager.Update();
			CHECK(FileSystem::GetThreadOperationCount() == operations);
			const size_t registered = manager.GetAllMetadata().size();
			if (registered != previous)
			{
				// At most the slice's entries (1,280 by the clock), rounded up to the batch that crossed it.
				CHECK(registered - previous <= 64 * 20 + 64);
				slices++;
				previous = registered;
				CHECK(StateCountsMatch(manager));
			}
			else
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		CHECK(slices >= 10'000 / (64 * 20 + 64));
		CHECK(manager.FindAssetByPath("Materials/Folder_09/Material_09999.stmat") == project->Materials.back());
		CHECK(manager.FindAssetByPath("Scenes/Many.stscene") == project->Scene);
		// What the scene references was found: every material, and the built-in cube.
		CHECK(manager.GetDependencies(project->Scene).size() == 10'001);
		CHECK_FALSE(manager.GetScanProgress().has_value());
		CHECK(StateCountsMatch(manager));
	}

	TEST_CASE("Closing the manager during a scan ends it at once; the job stops on its own")
	{
		GatedJobSystem jobs;
		const std::filesystem::path root = CreateTemporaryDirectory("RegistrationClose");
		std::string error;
		const std::optional<ManyAssetsProject> project = WriteManyAssetsProject(root / "Many", 200, error);
		REQUIRE_MESSAGE(project.has_value(), error);
		auto manager = CreateScope<EditorAssetManager>(MakeSpecification(project->Directory));
		{
			// The scan's job waits behind the held background thread.
			BackgroundGate gate;
			manager->BeginScan(ScanImports::InBackground);
			CHECK(manager->IsScanning());
			const auto start = std::chrono::steady_clock::now();
			manager->Close();
			manager.reset();
			CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
		}
		// The gate opened: the job runs (the job system's shutdown waits for it) without the manager, which is gone, and stops
		// at its first check.
	}

	TEST_CASE("A file added while a scan runs registers once")
	{
		GatedJobSystem jobs;
		const std::filesystem::path root = CreateTemporaryDirectory("RegistrationAddedDuringScan");
		std::string error;
		const std::optional<ManyAssetsProject> project = WriteManyAssetsProject(root / "Many", 50, error);
		REQUIRE_MESSAGE(project.has_value(), error);
		EditorAssetManager manager(MakeSpecification(project->Directory, true));
		const std::filesystem::path added = project->Directory / "Assets" / "Materials" / "Added.stmat";
		{
			BackgroundGate gate;
			manager.BeginScan(ScanImports::InBackground);
			// Written after the scan began, before its walk: the walk finds it, and the watcher reports it too. (A copy of a
			// material of the project, without its .meta.)
			const std::optional<std::string> material = FileSystem::ReadText(project->Directory / "Assets" / "Materials" / "Folder_00" / "Material_00000.stmat");
			REQUIRE(material.has_value());
			REQUIRE(FileSystem::WriteText(added, *material));
		}
		REQUIRE(WaitUntil([&]()
		{
			manager.Update();
			return !manager.IsScanning();
		}, std::chrono::seconds(30)));
		// The watcher's report arrives after the scan applied: it finds the file registered.
		const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
		while (std::chrono::steady_clock::now() < settle)
		{
			manager.Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		const AssetHandle handle = manager.FindAssetByPath("Materials/Added.stmat");
		REQUIRE(handle.IsValid());
		CHECK(CountAssetsAt(manager, "Materials/Added.stmat") == 1);
		// Its .meta was written once, by the scan, with that handle.
		const AssetMeta meta = ReadAssetMeta(project->Directory / "Assets" / "Materials" / "Added.stmat.meta");
		CHECK(meta.Status == AssetMetaStatus::Valid);
		CHECK(meta.Handle == handle);
	}
}
