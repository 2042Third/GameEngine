#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Perf/StressProject.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Process.h>
#include <Strata/Project/Project.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Frame times of the real editor (CTest StrataTests.PerfGPU: Release and Dist, with a GPU and a display): StrataEditor opens
// generated scenes in a maximized window and runs a command script whose editor.wait steps report the CPU time of their
// frames, and exports the streaming stress project, whose project.export reports the frames that ran while it exported.
// Runs the StrataEditor next to the test executable, or the one STRATA_TEST_EDITOR_PATH names.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Opening the 1,000,000-entity scene alone takes seconds.
	constexpr std::chrono::milliseconds c_EditorTimeout = std::chrono::minutes(10);
	constexpr int c_WaitFrames = 120;

	std::filesystem::path GetEditorPath()
	{
		if (const std::optional<std::string> path = Platform::GetEnvVar("STRATA_TEST_EDITOR_PATH"); path && !path->empty())
			return FileSystem::FromUTF8(*path);
#if defined(ST_PLATFORM_WINDOWS)
		return Platform::GetExecutableDirectory() / "StrataEditor.exe";
#else
		return Platform::GetExecutableDirectory() / "StrataEditor";
#endif
	}

	// A scene of `count` empty entities nested under one root, with a camera and a light, saved where the editor finds it.
	// Written without indentation, which would make the million-entity file much larger.
	void WriteNestedScene(const std::filesystem::path& file, size_t count)
	{
		Scene scene("Nested");
		Perf::AddCameraAndLight(scene);
		Perf::CreateNestedHierarchy(scene, count);
		REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(SceneSerializer::Serialize(scene)) + "\n"));
	}

	// A grid of `count` cubes (the built-in mesh: one render proxy each) with a camera and a light, saved like WriteNestedScene's.
	void WriteCubeScene(const std::filesystem::path& file, size_t count)
	{
		Scene scene("Cubes");
		Perf::AddCameraAndLight(scene);
		Perf::CreateCubeGrid(scene, count);
		REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(SceneSerializer::Serialize(scene)) + "\n"));
	}

	// The results the command script logged for its steps running `command` ("editor.wait -> {...}"), in order.
	std::vector<nlohmann::json> FindCommandResults(const std::string& output, std::string_view command)
	{
		const std::string marker = std::string(command) + " -> ";
		std::vector<nlohmann::json> results;
		size_t position = 0;
		while ((position = output.find(marker, position)) != std::string::npos)
		{
			position += marker.size();
			const size_t end = output.find('\n', position);
			std::string line = output.substr(position, end == std::string::npos ? std::string::npos : end - position);
			while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
				line.pop_back();
			const std::optional<nlohmann::json> result = JsonUtils::Parse(line);
			REQUIRE_MESSAGE(result.has_value(), "Unreadable ", std::string(command), " result: ", line);
			results.push_back(*result);
		}
		return results;
	}

	nlohmann::json OpenScene(std::string_view scene)
	{
		return { { "command", "scene.open" }, { "parameters", { { "scene", scene } } } };
	}

	nlohmann::json Command(std::string_view command)
	{
		return { { "command", command } };
	}

	nlohmann::json Wait(int frames = c_WaitFrames)
	{
		return { { "command", "editor.wait" }, { "parameters", { { "frames", frames } } } };
	}

	nlohmann::json Command(std::string_view command, const nlohmann::json& parameters)
	{
		return { { "command", command }, { "parameters", parameters } };
	}

	// A number in a command's result (JSON pointer), REQUIREd to be there.
	double GetNumber(const nlohmann::json& result, const std::string& pointer)
	{
		const nlohmann::json::json_pointer path(pointer);
		REQUIRE_MESSAGE(result.contains(path), "No ", pointer, " in ", result.dump());
		REQUIRE(result.at(path).is_number());
		return result.at(path).get<double>();
	}

}

TEST_SUITE("PerfGPU.Editor")
{
	TEST_CASE("Editor frames of 100,000 and 1,000,000 nested entities, opened, edited and played")
	{
		const std::filesystem::path editor = GetEditorPath();
		REQUIRE_MESSAGE(FileSystem::IsRegularFile(editor), "The editor frame measurements need the built StrataEditor: ", FileSystem::ToUTF8(editor));

		const std::filesystem::path directory = CreateTemporaryDirectory("PerfEditorFrames");
		std::string error;
		const Ref<Project> project = Project::Create(directory / "Project", "PerfFrames", &error);
		REQUIRE_MESSAGE(project, error);
		const std::filesystem::path scenes = project->GetAssetDirectory() / "Scenes";
		REQUIRE(FileSystem::CreateDirectories(scenes));
		const std::vector<std::filesystem::path> sceneFiles = { scenes / "Nested100k.stscene", scenes / "Nested1M.stscene", scenes / "Cubes100k.stscene" };
		WriteNestedScene(sceneFiles[0], 100'000);
		WriteNestedScene(sceneFiles[1], 1'000'000);
		WriteCubeScene(sceneFiles[2], 100'000);

		// Once the editor's first frames are drawn (they create its renderers), each scene is opened (scene.open reports the
		// frames that ran while it loaded), edited for 120 frames, then played for 120 frames. After the million entities are
		// in, editor.status reports the editor's memory; the frame that starts playing them (copying the scene) is the longest
		// of the latest few frames editor.perf summarizes. Then the smaller scene is opened again over the million entities,
		// which it replaces, and last 100,000 cubes over it: render proxies the viewport draws, which their load builds over
		// frames before the swap (RenderScene::PrepareLoadedScene), so the frame that shows them builds none.
		constexpr int c_PlayStartFrames = 8;
		const nlohmann::json script = nlohmann::json::array({ Wait(),
			OpenScene("Scenes/Nested100k.stscene"), Wait(), Command("play.start"), Wait(), Command("play.stop"),
			OpenScene("Scenes/Nested1M.stscene"), Wait(), Command("editor.status"), Command("play.start"), Wait(c_PlayStartFrames / 2),
			Command("editor.perf", { { "frames", c_PlayStartFrames } }), Wait(), Command("play.stop"), OpenScene("Scenes/Nested100k.stscene"),
			OpenScene("Scenes/Cubes100k.stscene") });
		const std::filesystem::path scriptFile = directory / "Commands.json";
		REQUIRE(FileSystem::WriteText(scriptFile, script.dump(1, '\t') + "\n"));

		// One editor run measures every metric; a metric over its budget is confirmed by another run. The frames of editing and
		// playing are medians of a wait's frames (which ignore the frames the machine's other work slowed down). Opening a
		// scene and starting to play are maxima by nature: the longest frame while the scene loads (the frame that swaps it in,
		// and lets go of the scene it replaces, included) and the frame that starts playing, the hitches these loads must not
		// cause. The working set is the editor's memory once the million entities are open, a level that stays.
		const std::vector<std::string> frameMetrics = {
			"PerfGPU.Editor.EditFrameNested100k", "PerfGPU.Editor.PlayFrameNested100k",
			"PerfGPU.Editor.EditFrameNested1M", "PerfGPU.Editor.PlayFrameNested1M" };
		std::vector<std::string> metrics = frameMetrics;
		metrics.insert(metrics.end(), { "PerfGPU.Editor.SceneOpen100kMaxFrame", "PerfGPU.Editor.SceneOpen1MMaxFrame", "PerfGPU.Editor.SceneOpen1MWorkingSet",
			"PerfGPU.Editor.PlayStart1MMaxFrame", "PerfGPU.Editor.SceneOpenOver1MMaxFrame", "PerfGPU.Editor.SceneOpen100kCubesMaxFrame" });
		Perf::CheckBudgetsConfirmed(metrics, [&]
		{
			// Maximized (the default): the frames draw the editor at the size people use it. --frames keeps the run from
			// saving its panel layout over the user's; the script ends the run long before.
			ProcessSpecification specification;
			specification.Executable = editor;
			specification.Arguments = { "--no-automation", "--frames", "100000", "--quit-after-commands", "--project",
				FileSystem::ToUTF8(project->GetProjectFile()), "--commands", FileSystem::ToUTF8(scriptFile) };
			specification.Output = ProcessOutputMode::Capture;
			const Process::RunResult run = Process::Run(specification, c_EditorTimeout);
			INFO("Editor output:\n", run.Output);
			REQUIRE_MESSAGE(run.Started, run.Error);
			REQUIRE_FALSE(run.TimedOut);
			if (run.ExitCode != 0)
			{
				// Once, under load, the editor failed to open the million-entity scene ("Invalid JSON", cause unknown) that
				// every other run opened: whether the file on disk is damaged, or the editor's read or parse failed on a good
				// one, tells where to look next.
				for (const std::filesystem::path& sceneFile : sceneFiles)
					MESSAGE(DescribeJsonFile(sceneFile));
			}
			REQUIRE(run.ExitCode == 0);

			// The waits of editing and playing each scene (the first one, while the editor starts, and the short one after
			// starting to play the million entities aside).
			std::vector<nlohmann::json> waits = FindCommandResults(run.Output, "editor.wait");
			REQUIRE(waits.size() == frameMetrics.size() + 2);
			waits.erase(waits.begin() + 4);
			waits.erase(waits.begin());
			Perf::MetricValues values;
			for (size_t index = 0; index < waits.size(); index++)
			{
				CAPTURE(frameMetrics[index]);
				const nlohmann::json* frameTimes = JsonUtils::Find(waits[index], "frameTimes");
				REQUIRE(frameTimes != nullptr);
				const nlohmann::json* count = JsonUtils::Find(*frameTimes, "count");
				const nlohmann::json* median = JsonUtils::Find(*frameTimes, "medianMs");
				REQUIRE((count && count->is_number_integer() && median && median->is_number()));
				// Every frame but the first of the wait (which also opened the scene or started playing) is measured.
				CHECK(count->get<int>() >= c_WaitFrames - 2);
				values[frameMetrics[index]] = median->get<double>();
			}

			// scene.open answers once the scene is in, with the frames that ran meanwhile.
			const std::vector<nlohmann::json> opens = FindCommandResults(run.Output, "scene.open");
			REQUIRE(opens.size() == 4);
			CHECK(GetNumber(opens[0], "/entities") == 100'000 + 2);
			CHECK(GetNumber(opens[1], "/entities") == 1'000'000 + 2);
			CHECK(GetNumber(opens[2], "/entities") == 100'000 + 2);
			CHECK(GetNumber(opens[3], "/entities") == 100'000 + 2);
			MESSAGE("Opening 100,000 entities: ", GetNumber(opens[0], "/seconds"), " s over ", GetNumber(opens[0], "/frames"), " frames; 1,000,000: ",
				GetNumber(opens[1], "/seconds"), " s over ", GetNumber(opens[1], "/frames"), " frames");
			values["PerfGPU.Editor.SceneOpen100kMaxFrame"] = GetNumber(opens[0], "/frameTimes/maxMs");
			values["PerfGPU.Editor.SceneOpen1MMaxFrame"] = GetNumber(opens[1], "/frameTimes/maxMs");
			values["PerfGPU.Editor.SceneOpenOver1MMaxFrame"] = GetNumber(opens[2], "/frameTimes/maxMs");
			values["PerfGPU.Editor.SceneOpen100kCubesMaxFrame"] = GetNumber(opens[3], "/frameTimes/maxMs");
			const std::vector<nlohmann::json> statuses = FindCommandResults(run.Output, "editor.status");
			REQUIRE(statuses.size() == 1);
			values["PerfGPU.Editor.SceneOpen1MWorkingSet"] = GetNumber(statuses[0], "/memory/workingSetMB");
			const std::vector<nlohmann::json> profiles = FindCommandResults(run.Output, "editor.perf");
			REQUIRE(profiles.size() == 1);
			CHECK(GetNumber(profiles[0], "/frames") == c_PlayStartFrames);
			values["PerfGPU.Editor.PlayStart1MMaxFrame"] = GetNumber(profiles[0], "/frame/maxMs");
			return values;
		});
	}

	TEST_CASE("Exporting the stress project keeps the editor's frames within their budgets")
	{
		const std::filesystem::path editor = GetEditorPath();
		REQUIRE_MESSAGE(FileSystem::IsRegularFile(editor), "The export measurement needs the built StrataEditor: ", FileSystem::ToUTF8(editor));

		// The stress project, imported here, so that the editor finds its imports current and only exports.
		const std::filesystem::path directory = CreateTemporaryDirectory("PerfEditorExport");
		std::string error;
		std::optional<StressProject> project;
		{
			ScopedJobSystem jobs(0, 2);
			project = WriteStressProject(directory / "Stress", StressProjectSpec(), error);
			REQUIRE_MESSAGE(project, error);
			REQUIRE_MESSAGE(BuildStressPack(*project, directory / "Imported.stpak", error), error);
		}

		// The editor shows the world (and streams its textures) while it exports it; the export answers with the frame times
		// of the frames that ran while it worked. One editor run measures every metric: the median frame, the longest frame (a
		// maximum by nature: the export's worst hitch; the pack workstream's acceptance limit of 50 ms is far above the
		// budget) and the export's time; a metric over its budget is confirmed by another run, which exports anew.
		uint32_t attempt = 0;
		Perf::CheckBudgetsConfirmed({ "PerfGPU.Editor.ExportFrameStress", "PerfGPU.Editor.ExportMaxFrameStress", "PerfGPU.Editor.ExportStress" }, [&]
		{
			attempt++;
			const nlohmann::json script = nlohmann::json::array({ Wait(), { { "command", "project.export" }, { "parameters", {
				{ "directory", FileSystem::ToUTF8(directory / ("Build" + std::to_string(attempt))) }, { "includeRuntime", false } } } } });
			const std::filesystem::path scriptFile = directory / ("Commands" + std::to_string(attempt) + ".json");
			REQUIRE(FileSystem::WriteText(scriptFile, script.dump(1, '\t') + "\n"));
			ProcessSpecification specification;
			specification.Executable = editor;
			specification.Arguments = { "--no-automation", "--frames", "100000", "--quit-after-commands", "--project", FileSystem::ToUTF8(project->Directory),
				"--commands", FileSystem::ToUTF8(scriptFile) };
			specification.Output = ProcessOutputMode::Capture;
			const Process::RunResult run = Process::Run(specification, c_EditorTimeout);
			INFO("Editor output:\n", run.Output);
			REQUIRE_MESSAGE(run.Started, run.Error);
			REQUIRE_FALSE(run.TimedOut);
			REQUIRE(run.ExitCode == 0);

			const std::vector<nlohmann::json> exports = FindCommandResults(run.Output, "project.export");
			REQUIRE(exports.size() == 1);
			const nlohmann::json& result = exports[0];
			const nlohmann::json* frameTimes = JsonUtils::Find(result, "frameTimes");
			REQUIRE(frameTimes != nullptr);
			const double maxMs = JsonUtils::GetFloat(*frameTimes, "maxMs", 1.0e9f);
			const double medianMs = JsonUtils::GetFloat(*frameTimes, "medianMs", 1.0e9f);
			const double seconds = JsonUtils::GetFloat(result, "seconds", 0.0f);
			MESSAGE("Export of the stress project: ", seconds, " s over ", JsonUtils::GetUInt(result, "frames", 0), " frames; frame time median ", medianMs,
				" ms, at most ", maxMs, " ms; pack ", JsonUtils::GetUInt(result, "packBytes", 0), " bytes");
			// The editor drew frames the whole time.
			CHECK(JsonUtils::GetUInt(*frameTimes, "count", 0) >= 2);
			return Perf::MetricValues { { "PerfGPU.Editor.ExportFrameStress", medianMs }, { "PerfGPU.Editor.ExportMaxFrameStress", maxMs },
				{ "PerfGPU.Editor.ExportStress", seconds * 1000.0 } };
		});
	}
}
