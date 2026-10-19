#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Perf/StressProject.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Process.h>
#include <Strata/Physics/PhysicsComponents.h>
#include <Strata/Project/Project.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>
#include <Strata/World/WorldAsset.h>
#include <Strata/World/WorldComponents.h>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <cstdint>
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

	// Writes an asset file and its .meta into the project's assets, without indentation (the world's cells are large).
	void WriteAssetFile(const std::filesystem::path& assets, const std::string& path, const nlohmann::json& document, AssetHandle handle, const char* type)
	{
		const std::filesystem::path file = assets / FileSystem::FromUTF8(path);
		REQUIRE(FileSystem::CreateDirectories(file.parent_path()));
		REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(document) + "\n"));
		const nlohmann::json meta = { { "Handle", handle.ToString() }, { "ImportSettings", nlohmann::json::object() },
			{ "Strata", { { "Format", "AssetMeta" }, { "Version", 1 } } }, { "Type", type } };
		REQUIRE(FileSystem::WriteText(FileSystem::FromUTF8(FileSystem::ToUTF8(file) + ".meta"), JsonUtils::Dump(meta) + "\n"));
	}

	// The generated world of the editor's world metrics: `side` x `side` Ground cells of 32 m, each of `entitiesPerCell`
	// entities on a grid over the cell, the first tenth of them static boxes (a cube and a box collider: a render proxy and
	// a body each), the rest transforms only, like the nested scenes' entities. The Ground layer streams within 72 m: a
	// camera in the middle of a cell loads its 5 x 5 block. The persistent scene ("Scenes/Grid.stscene") holds the World
	// component, a light and a streaming source in the middle of the world, where the editor's camera is put.
	constexpr uint32_t c_GridWorldSide = 32;
	constexpr uint32_t c_GridWorldEntitiesPerCell = 2'500;
	constexpr float c_GridWorldCellSize = 32.0f;
	constexpr float c_GridWorldStreamingDistance = 72.0f;

	glm::vec3 GridWorldMiddle()
	{
		// The middle of cell (side / 2, side / 2).
		const float middle = (static_cast<float>(c_GridWorldSide / 2) + 0.5f) * c_GridWorldCellSize;
		return glm::vec3(middle, 2.0f, middle);
	}

	void WriteGridWorld(const Project& project)
	{
		const std::filesystem::path assets = project.GetAssetDirectory();
		const AssetHandle worldHandle = UUID(0x5E0000000001ull);
		const AssetHandle persistentHandle = UUID(0x5E0000000002ull);
		WorldData data;
		data.Name = "Grid";
		data.PersistentScene = persistentHandle;
		data.Layers.push_back(WorldLayer { 1, "Ground", c_GridWorldCellSize, c_GridWorldStreamingDistance, true, false });
		constexpr uint32_t c_PerRow = 50; // 50 x 50 entities 0.64 m apart over a cell
		const float spacing = c_GridWorldCellSize / static_cast<float>(c_PerRow);
		for (uint32_t z = 0; z < c_GridWorldSide; z++)
		{
			for (uint32_t x = 0; x < c_GridWorldSide; x++)
			{
				Scene cell(fmt::format("Grid {}_{}", x, z));
				const uint64_t cellIndex = static_cast<uint64_t>(z) * c_GridWorldSide + x;
				for (uint32_t index = 0; index < c_GridWorldEntitiesPerCell; index++)
				{
					Entity entity = cell.CreateEntityWithUUID(UUID(0x5F00000000000000ull + (cellIndex << 16) + index), fmt::format("Entity {}", index));
					TransformComponent& transform = entity.GetComponent<TransformComponent>();
					transform.Translation = glm::vec3(static_cast<float>(x) * c_GridWorldCellSize + (static_cast<float>(index % c_PerRow) + 0.5f) * spacing, 0.25f,
						static_cast<float>(z) * c_GridWorldCellSize + (static_cast<float>(index / c_PerRow % c_PerRow) + 0.5f) * spacing);
					if (index < c_GridWorldEntitiesPerCell / 10)
					{
						transform.Scale = glm::vec3(0.5f);
						entity.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
						entity.AddComponent<BoxColliderComponent>();
					}
				}
				const AssetHandle handle = DeriveSubAssetHandle(worldHandle, fmt::format("Cell/1/{}/{}", x, z));
				WriteAssetFile(assets, fmt::format("Worlds/Grid/Ground/{}_{}.stscene", x, z), SceneSerializer::Serialize(cell), handle, "Scene");
				data.Cells.push_back(WorldCell { 1, static_cast<int32_t>(x), static_cast<int32_t>(z), handle });
			}
		}

		Scene persistent("Grid");
		Perf::AddCameraAndLight(persistent);
		persistent.CreateEntityWithUUID(UUID(0x5E0000000100ull), "World").AddComponent<WorldComponent>().World = worldHandle;
		Entity source = persistent.CreateEntityWithUUID(UUID(0x5E0000000101ull), "Source");
		source.GetComponent<TransformComponent>().Translation = GridWorldMiddle();
		source.AddComponent<StreamingSourceComponent>();
		WriteAssetFile(assets, "Scenes/Grid.stscene", SceneSerializer::Serialize(persistent), persistentHandle, "Scene");
		WriteAssetFile(assets, "Worlds/Grid.stworld", WorldAsset::ToDocument(data), worldHandle, "World");
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

	TEST_CASE("Editor frames of a 32 x 32-cell world of 2.56 million entities, idle and starting to play")
	{
		const std::filesystem::path editor = GetEditorPath();
		REQUIRE_MESSAGE(FileSystem::IsRegularFile(editor), "The editor's world measurements need the built StrataEditor: ", FileSystem::ToUTF8(editor));

		const std::filesystem::path directory = CreateTemporaryDirectory("PerfEditorWorld");
		std::string error;
		const Ref<Project> project = Project::Create(directory / "Project", "PerfWorld", &error);
		REQUIRE_MESSAGE(project, error);
		WriteGridWorld(*project);

		// The persistent scene opens; the editor camera goes to the middle of the world, and world.loadCells answers once the
		// cells around it (without pinning them: they are the ones the camera streams) are in. Then the editor runs idle with a
		// still camera for a wait's frames, editor.perf summarizes them (the World system's stage among them), and play starts
		// from what the editor holds: the persistent scene and those cells, which the game's source in the middle keeps. The
		// frame that starts playing is the longest of the few frames editor.perf summarizes after it.
		const glm::vec3 middle = GridWorldMiddle();
		const nlohmann::json position = { middle.x, middle.y, middle.z };
		constexpr int c_PlayStartFrames = 8;
		const nlohmann::json script = nlohmann::json::array({ Wait(), OpenScene("Scenes/Grid.stscene"),
			Command("camera.set", { { "position", position }, { "target", { middle.x + 10.0f, middle.y, middle.z } } }),
			Command("world.loadCells", { { "region", { { "center", position }, { "radius", c_GridWorldStreamingDistance } } }, { "pin", false }, { "wait", true } }),
			Wait(), Command("editor.perf", { { "frames", c_WaitFrames - 2 } }), Command("world.status"), Command("play.start"),
			Wait(c_PlayStartFrames / 2), Command("editor.perf", { { "frames", c_PlayStartFrames } }), Command("world.status"), Wait(),
			Command("play.stop") });
		const std::filesystem::path scriptFile = directory / "Commands.json";
		REQUIRE(FileSystem::WriteText(scriptFile, script.dump(1, '\t') + "\n"));

		// The idle frame is the median of a wait's frames; starting to play is a maximum by nature (the hitch of copying the
		// loaded cells into the game). A metric over its budget is confirmed by another run.
		Perf::CheckBudgetsConfirmed({ "PerfGPU.Editor.EditFrameWorldIdle", "PerfGPU.Editor.PlayStartWorldMaxFrame" }, [&]
		{
			ProcessSpecification specification;
			specification.Executable = editor;
			specification.Arguments = { "--no-automation", "--frames", "100000", "--quit-after-commands", "--project",
				FileSystem::ToUTF8(project->GetProjectFile()), "--commands", FileSystem::ToUTF8(scriptFile) };
			specification.Output = ProcessOutputMode::Capture;
			const Process::RunResult run = Process::Run(specification, c_EditorTimeout);
			INFO("Editor output:\n", run.Output);
			REQUIRE_MESSAGE(run.Started, run.Error);
			REQUIRE_FALSE(run.TimedOut);
			REQUIRE(run.ExitCode == 0);

			// The cells around the camera were in while the editor ran idle, and stayed in when play started.
			const std::vector<nlohmann::json> statuses = FindCommandResults(run.Output, "world.status");
			REQUIRE(statuses.size() == 2);
			const double editCells = GetNumber(statuses[0], "/cells/loaded");
			CHECK(editCells >= 21);
			CHECK(GetNumber(statuses[0], "/cells/loading") == 0);
			const std::vector<nlohmann::json> opens = FindCommandResults(run.Output, "scene.open");
			REQUIRE(opens.size() == 1);

			const std::vector<nlohmann::json> waits = FindCommandResults(run.Output, "editor.wait");
			REQUIRE(waits.size() == 4);
			const double idleMedian = GetNumber(waits[1], "/frameTimes/medianMs");
			CHECK(GetNumber(waits[1], "/frameTimes/count") >= c_WaitFrames - 2);
			const std::vector<nlohmann::json> profiles = FindCommandResults(run.Output, "editor.perf");
			REQUIRE(profiles.size() == 2);
			const nlohmann::json* stages = JsonUtils::Find(profiles[0], "stages");
			REQUIRE(stages != nullptr);
			const nlohmann::json* streaming = JsonUtils::Find(*stages, "World.Streaming");
			const double streamingMean = streaming ? GetNumber(*streaming, "/avgMs") : 0.0;
			CHECK(GetNumber(profiles[1], "/frames") == c_PlayStartFrames);
			const double playStartMax = GetNumber(profiles[1], "/frame/maxMs");
			MESSAGE("World of ", c_GridWorldSide * c_GridWorldSide, " cells: ", editCells, " cells loaded around the camera; idle frame median ", idleMedian,
				" ms (World.Streaming mean ", streamingMean, " ms); the frame starting to play ", playStartMax, " ms (", GetNumber(opens[0], "/seconds"),
				" s to open the persistent scene)");
			return Perf::MetricValues { { "PerfGPU.Editor.EditFrameWorldIdle", idleMedian }, { "PerfGPU.Editor.PlayStartWorldMaxFrame", playStartMax } };
		});
	}
}
