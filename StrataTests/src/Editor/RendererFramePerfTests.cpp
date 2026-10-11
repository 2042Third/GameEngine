#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Process.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Edit frames of the real editor on the renderer's scaling scenes (CTest StrataTests.PerfGPU: Release and Dist, with a GPU
// and a display): StrataEditor opens generated scenes in a maximized window with its default overlays (grid, selection
// outline, scene gizmos) and runs a command script whose editor.wait steps report the CPU time of their frames
// (Application::GetLastFrameWorkTime: the wait for the GPU is not part of it). Runs the StrataEditor next to the test
// executable, or the one STRATA_TEST_EDITOR_PATH names.
//
// The generated grids hang below one root ("Grid"), collapsed in the Hierarchy panel: these frames measure the scene's
// rendering and overlays. The same grids as roots also measure the Hierarchy panel, which draws every root each frame
// (100,000 roots add about 14 ms) until its rows are clipped to the visible ones.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr std::chrono::milliseconds c_EditorTimeout = std::chrono::minutes(15);
	constexpr int c_WaitFrames = 120;
	// Frames before a measurement: the first frames of a scene build its render proxies and load what it shows.
	constexpr int c_SettleFrames = 90;
	constexpr size_t c_UniqueMaterials = 10'000;
	// Handles of the generated material assets (fixed, so the scene can name them before the editor imports them).
	constexpr uint64_t c_MaterialHandleBase = 0x7E00000000100000ull;

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

	// Written without indentation, which would make the large files much larger.
	void WriteScene(const std::filesystem::path& file, const Scene& scene)
	{
		REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(SceneSerializer::Serialize(scene)) + "\n"));
	}

	// A large ground box under the generated grids, so the editor frames the whole grid when it opens the scene.
	void AddGround(Scene& scene, float size)
	{
		Entity ground = scene.CreateEntity("Ground");
		TransformComponent& transform = ground.GetComponent<TransformComponent>();
		transform.Translation = glm::vec3(0.0f, -0.05f, 0.0f);
		transform.Scale = glm::vec3(size, 0.1f, size);
		ground.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
	}

	// `count` materials of distinct colors, each a .stmat asset with its .meta (the importer's default settings), so the
	// editor imports them with the handles the scene uses.
	std::vector<AssetHandle> WriteMaterials(const std::filesystem::path& directory, size_t count)
	{
		REQUIRE(FileSystem::CreateDirectories(directory));
		const AssetImporter* importer = AssetImporterRegistry::FindByExtension(".stmat");
		REQUIRE(importer);
		std::vector<AssetHandle> handles;
		handles.reserve(count);
		for (size_t index = 0; index < count; index++)
		{
			MaterialProperties properties;
			properties.BaseColor = glm::vec4(static_cast<float>(index % 17) / 16.0f, static_cast<float>(index % 13) / 12.0f, static_cast<float>(index % 7) / 6.0f, 1.0f);
			properties.Roughness = static_cast<float>(index % 5) / 4.0f;
			const std::filesystem::path file = directory / FileSystem::FromUTF8(fmt::format("M{:05}.stmat", index));
			REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(Material::Create(properties)->Serialize()) + "\n"));

			const AssetHandle handle(c_MaterialHandleBase + index);
			nlohmann::json meta = nlohmann::json::object();
			meta["Strata"] = { { "Format", "AssetMeta" }, { "Version", 1 } };
			meta["Handle"] = UUIDToJson(handle);
			meta["Type"] = AssetTypeToString(AssetType::Material);
			meta["ImportSettings"] = importer->GetDefaultSettings(file);
			std::filesystem::path metaFile = file;
			metaFile += ".meta";
			REQUIRE(FileSystem::WriteText(metaFile, JsonUtils::Dump(meta) + "\n"));
			handles.push_back(handle);
		}
		return handles;
	}

	// The results the command script logged for its editor.wait steps ("editor.wait -> {...}"), in order.
	std::vector<nlohmann::json> FindWaitResults(const std::string& output)
	{
		constexpr std::string_view c_Marker = "editor.wait -> ";
		std::vector<nlohmann::json> results;
		size_t position = 0;
		while ((position = output.find(c_Marker, position)) != std::string::npos)
		{
			position += c_Marker.size();
			const size_t end = output.find('\n', position);
			std::string line = output.substr(position, end == std::string::npos ? std::string::npos : end - position);
			while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
				line.pop_back();
			const std::optional<nlohmann::json> result = JsonUtils::Parse(line);
			REQUIRE_MESSAGE(result.has_value(), "Unreadable editor.wait result: ", line);
			results.push_back(*result);
		}
		return results;
	}

	double MedianFrameMs(const nlohmann::json& waitResult)
	{
		const nlohmann::json* frameTimes = JsonUtils::Find(waitResult, "frameTimes");
		REQUIRE(frameTimes != nullptr);
		const nlohmann::json* count = JsonUtils::Find(*frameTimes, "count");
		const nlohmann::json* median = JsonUtils::Find(*frameTimes, "medianMs");
		REQUIRE((count && count->is_number_integer() && median && median->is_number()));
		// Every frame but the first of the wait (which also ran the step before it) is measured.
		CHECK(count->get<int>() >= c_WaitFrames - 2);
		return median->get<double>();
	}

	nlohmann::json Command(std::string_view command, nlohmann::json parameters = nlohmann::json::object())
	{
		return { { "command", command }, { "parameters", std::move(parameters) } };
	}

	nlohmann::json Wait(int frames)
	{
		return Command("editor.wait", { { "frames", frames } });
	}

}

TEST_SUITE("PerfGPU.Renderer")
{
	TEST_CASE("Edit frames of the renderer's scaling scenes: cubes, the streaming view, a large selection, a narrow view, point lights, unique materials")
	{
		const std::filesystem::path editor = GetEditorPath();
		REQUIRE_MESSAGE(FileSystem::IsRegularFile(editor), "The editor frame measurements need the built StrataEditor: ", FileSystem::ToUTF8(editor));

		const std::filesystem::path directory = CreateTemporaryDirectory("PerfRendererFrames");
		std::string error;
		const Ref<Project> project = Project::Create(directory / "Project", "PerfRenderer", &error);
		REQUIRE_MESSAGE(project, error);
		const std::filesystem::path scenes = project->GetAssetDirectory() / "Scenes";
		REQUIRE(FileSystem::CreateDirectories(scenes));

		// Grids of unit cubes, seen whole: the editor frames what a scene renders when it opens it.
		UUID grid100k = UUID::Null();
		for (size_t count : { size_t(100'000), size_t(300'000), size_t(50'000) })
		{
			Scene scene("Cubes");
			Perf::AddCameraAndLight(scene);
			Entity grid = scene.CreateEntity("Grid");
			if (count == 100'000)
				grid100k = grid.GetUUID();
			Perf::CreateCubeGrid(scene, count, {}, grid);
			WriteScene(scenes / FileSystem::FromUTF8(fmt::format("Cubes{}k.stscene", count / 1000)), scene);
		}
		{
			// 10,000 point lights over a ground of 200 x 200 units (lit by them, shown with their gizmos).
			Scene scene("Lights");
			Perf::AddCameraAndLight(scene);
			AddGround(scene, 220.0f);
			Perf::CreatePointLightGrid(scene, 10'000, 3.0f, 2.0f, scene.CreateEntity("Grid"));
			WriteScene(scenes / "Lights10k.stscene", scene);
		}
		{
			Scene scene("Materials");
			Perf::AddCameraAndLight(scene);
			const std::vector<AssetHandle> materials = WriteMaterials(project->GetAssetDirectory() / "Materials", c_UniqueMaterials);
			Perf::CreateCubeGrid(scene, c_UniqueMaterials, materials, scene.CreateEntity("Grid"));
			WriteScene(scenes / "Materials10k.stscene", scene);
		}

		// The narrow view looks straight down from 30 units with a 20 degree field of view: about 10 x 10 * aspect units
		// of the grid, under 200 of its 50,000 cubes in any window shape up to 4:1.
		const nlohmann::json narrowCamera = { { "position", { 0.0, 30.0, 0.01 } }, { "target", { 0.0, 0.0, 0.0 } }, { "fov", 20.0 } };
		const nlohmann::json script = nlohmann::json::array({
			Command("scene.open", { { "scene", "Scenes/Cubes100k.stscene" } }), Wait(c_SettleFrames), Wait(c_WaitFrames),
			// The streaming debug view over all 100,000 cubes in view (it looks at what the renderer culls, every frame).
			Command("viewport.setSettings", { { "debugView", "streaming" } }), Wait(c_SettleFrames), Wait(c_WaitFrames),
			Command("viewport.setSettings", { { "debugView", "none" } }),
			// Selecting the grid's root outlines all 100,000 cubes.
			Command("selection.set", { { "entities", nlohmann::json::array({ UUIDToJson(grid100k) }) } }), Wait(c_SettleFrames), Wait(c_WaitFrames),
			Command("scene.open", { { "scene", "Scenes/Cubes300k.stscene" } }), Wait(c_SettleFrames), Wait(c_WaitFrames),
			Command("scene.open", { { "scene", "Scenes/Cubes50k.stscene" } }), Command("camera.set", narrowCamera), Wait(c_SettleFrames), Wait(c_WaitFrames),
			Command("scene.open", { { "scene", "Scenes/Lights10k.stscene" } }), Wait(c_SettleFrames), Wait(c_WaitFrames),
			Command("scene.open", { { "scene", "Scenes/Materials10k.stscene" } }), Wait(c_SettleFrames * 2), Wait(c_WaitFrames) });
		const std::filesystem::path scriptFile = directory / "Commands.json";
		REQUIRE(FileSystem::WriteText(scriptFile, script.dump(1, '\t') + "\n"));

		// One editor run measures every metric (the median CPU time of a wait's frames, which ignores the frames the machine's
		// other work slowed down); a metric over its budget is confirmed by another run.
		Perf::CheckBudgetsConfirmed({ "PerfGPU.Renderer.EditFrameCubes100k", "PerfGPU.Renderer.EditFrameCubes100kStreamingView",
			"PerfGPU.Renderer.EditFrameCubes100kSelected", "PerfGPU.Renderer.EditFrameCubes300k", "PerfGPU.Renderer.EditFrameCubes50kNarrow",
			"PerfGPU.Renderer.EditFramePointLights10k", "PerfGPU.Renderer.EditFrameMaterials10k" }, [&]
		{
			// Maximized (the default): the frames draw the editor at the size people use it. --frames keeps the run from saving
			// its panel layout over the user's; the script ends the run long before.
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

			// Every scene's first wait settles it; the second one is measured.
			const std::vector<nlohmann::json> waits = FindWaitResults(run.Output);
			REQUIRE(waits.size() == 14);
			const double unselected = MedianFrameMs(waits[1]);
			const double streamingView = MedianFrameMs(waits[3]);
			const double selected = MedianFrameMs(waits[5]);
			// What the selection adds is the difference of two medians, too small to budget on its own (it measures zero or
			// less in some runs): the budget is on the frame with the selection, the difference is reported.
			MESSAGE("100,000 cubes: ", unselected, " ms, ", streamingView, " ms in the streaming view, ", selected, " ms with all of them selected (",
				selected - unselected, " ms added)");
			return Perf::MetricValues {
				{ "PerfGPU.Renderer.EditFrameCubes100k", unselected },
				{ "PerfGPU.Renderer.EditFrameCubes100kStreamingView", streamingView },
				{ "PerfGPU.Renderer.EditFrameCubes100kSelected", selected },
				{ "PerfGPU.Renderer.EditFrameCubes300k", MedianFrameMs(waits[7]) },
				{ "PerfGPU.Renderer.EditFrameCubes50kNarrow", MedianFrameMs(waits[9]) },
				{ "PerfGPU.Renderer.EditFramePointLights10k", MedianFrameMs(waits[11]) },
				{ "PerfGPU.Renderer.EditFrameMaterials10k", MedianFrameMs(waits[13]) } };
		});
	}
}
