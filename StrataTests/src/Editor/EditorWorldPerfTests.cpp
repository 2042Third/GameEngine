#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/WorldSaveMeasurement.h"
#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Perf/WorldFixture.h"
#include "TestHelpers.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Timestep.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

// Perf tests of the editor on the realistic worlds (Perf/WorldFixture.h; CTest StrataTests.Perf: Release and Dist only, see
// Docs/DeveloperGuide.md, "Perf tests"). Saving: the editor with its UI (no GPU) opens the world, one edit makes it
// modified, and scene.save {wait: false} starts inside a frame, as Ctrl+S does (Editor/WorldSaveMeasurement.h). The frame
// takes only a snapshot of the scene; the document is written on a background thread while frames go on. The metrics,
// one save per attempt in a helper process of its own (its memory is the editor's and the save's alone):
// - SaveMaxFrame: the longest frame from the one that started the save to the one that published it, a maximum by nature
//   (the save's hitch is what the person feels). The snapshot's frame is the longest.
// - SaveSeconds: from the save's frame to its publication (the document written, renamed over the old one, registered).
// - SavePeakPrivateIncrease: the peak of private memory during the save over the private memory before it (the snapshot
//   and the writer's buffers), a maximum by nature.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr const char* c_SaveMaxFrame250k = "Perf.Editor.SaveMaxFrame250k";
	constexpr const char* c_SaveMaxFrame1M = "Perf.Editor.SaveMaxFrame1M";
	constexpr const char* c_SaveSeconds1M = "Perf.Editor.SaveSeconds1M";
	constexpr const char* c_SavePeakPrivateIncrease1M = "Perf.Editor.SavePeakPrivateIncrease1M";
	constexpr const char* c_ImportFolder600MaxFrame = "Perf.Editor.ImportFolder600MaxFrame";

	// A project holding one realistic world (and the fixture's empty start scene).
	WorldProject WriteWorld(const std::string& name, const WorldSceneSpec& world)
	{
		const Perf::ScopedApplicationJobSystem jobs;
		WorldProjectSpec spec;
		spec.Scenes.push_back(world);
		std::string error;
		std::optional<WorldProject> project = WriteWorldProject(CreateTemporaryDirectory(name) / "World", spec, error);
		REQUIRE_MESSAGE(project.has_value(), error);
		REQUIRE(project->Scenes.size() == 1);
		return std::move(*project);
	}

	WorldSaveMeasurement MeasureSave(const WorldProject& project, const WorldSceneSpec& world)
	{
		std::string error;
		const std::optional<WorldSaveMeasurement> measurement = MeasureWorldSaveInHelper(project.Directory, project.Scenes.front().Handle, error);
		REQUIRE_MESSAGE(measurement.has_value(), error);
		// The world and the edit's entity.
		CHECK(measurement->Entities == GetWorldEntityCount(world) + 1);
		MESSAGE(fmt::format("{}: opened in {:.2f} s; saved {} entities, {:.1f} MB in {:.2f} s over {} frames; the save's frame {:.2f} ms, the longest "
			"{:.2f} ms; private {:.0f} MB before, {:.0f} MB more at the peak", world.Name, measurement->OpenSeconds, measurement->Entities,
			static_cast<double>(measurement->Bytes) / (1024.0 * 1024.0), measurement->Seconds, measurement->Frames, measurement->SaveFrameMs,
			measurement->MaxFrameMs, measurement->PrivateMBBefore, measurement->PeakPrivateIncreaseMB));
		return *measurement;
	}

}

TEST_SUITE("Perf.Editor")
{
	// asset.import {directories, wait: false} of a folder of 600 64x64 PNGs (Perf/WorldFixture.h): the copy runs on an I/O
	// job, which also prepares the files for registration (their .meta files written, cached imports checked); a frame only
	// inserts them, and the imports run on the background threads while frames go on. The longest editor update (with the
	// command's poll) from the command until every import is applied, a maximum by nature; one import per attempt, into a
	// folder of its own.
	TEST_CASE("Importing a folder of 600 small textures holds no frame")
	{
		const Perf::ScopedApplicationJobSystem jobs;
		const std::filesystem::path root = CreateTemporaryDirectory("PerfImportFolder600");
		std::string error;
		REQUIRE_MESSAGE(WriteSmallTextureFolder(root / "Outside" / "Textures", error).has_value(), error);
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		RunCommandToCompletion(context, commands, "project.create", { { "directory", FileSystem::ToUTF8(root / "Game") }, { "name", "Game" } });
		EditorAssetManager& assets = *context.GetAssetManager();
		Perf::CheckBudgetConfirmed(c_ImportFolder600MaxFrame, [&]()
		{
			const auto start = std::chrono::steady_clock::now();
			const EditorCommandResult started = commands.Execute(context, "asset.import",
				{ { "directories", { FileSystem::ToUTF8(root / "Outside" / "Textures") } }, { "wait", false } });
			REQUIRE_MESSAGE(started.Success, started.Error);
			double longest = 0.0;
			uint64_t frames = 0;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
			while (assets.GetExternalImportCount() > 0 || assets.HasPendingImports())
			{
				REQUIRE(std::chrono::steady_clock::now() < deadline);
				const auto frameStart = std::chrono::steady_clock::now();
				context.Update(Timestep(1.0f / 60.0f));
				longest = std::max(longest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count());
				frames++;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			CHECK_FALSE(assets.HasImportFailures());
			MESSAGE(fmt::format("600 textures imported in {:.2f} s over {} frames; the longest {:.2f} ms",
				std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), frames, longest));
			return longest;
		});
	}

	TEST_CASE("Saving the realistic 250k world holds no frame")
	{
		const WorldProject project = WriteWorld("PerfWorldSave250k", c_World250k);
		Perf::CheckBudgetConfirmed(c_SaveMaxFrame250k, [&]() { return MeasureSave(project, c_World250k).MaxFrameMs; });
	}

	TEST_CASE("Saving the realistic 1M world holds no frame, takes seconds and little memory")
	{
		const WorldProject project = WriteWorld("PerfWorldSave1M", c_World1M);
		Perf::CheckBudgetsConfirmed({ c_SaveMaxFrame1M, c_SaveSeconds1M, c_SavePeakPrivateIncrease1M }, [&]()
		{
			const WorldSaveMeasurement measurement = MeasureSave(project, c_World1M);
			return Perf::MetricValues {
				{ c_SaveMaxFrame1M, measurement.MaxFrameMs },
				{ c_SaveSeconds1M, measurement.Seconds * 1000.0 },
				{ c_SavePeakPrivateIncrease1M, measurement.PeakPrivateIncreaseMB } };
		});
	}
}
