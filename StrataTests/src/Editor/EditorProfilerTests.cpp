#include <doctest/doctest.h>

#include "Audio/AudioTestUtils.h"
#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/FrameProfiler.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <set>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_ProfiledFrames = 30;

	nlohmann::json Run(EditorContext& context, EditorCommandRegistry& commands, std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
	{
		const EditorCommandResult result = commands.Execute(context, name, parameters);
		INFO(std::string(name), ": ", result.Error);
		REQUIRE(result.Success);
		REQUIRE_FALSE(result.IsPending());
		return result.Value;
	}

	std::set<std::string> GetKeys(const nlohmann::json& object)
	{
		REQUIRE(object.is_object());
		std::set<std::string> keys;
		for (const auto& [key, value] : object.items())
			keys.insert(key);
		return keys;
	}

	// The keys of a statistics object of editor.perf.
	const std::set<std::string> c_TimeKeys = { "avgMs", "p50Ms", "p95Ms", "maxMs" };

	// The result's shape, which tools and command scripts rely on.
	void CheckProfileSchema(const nlohmann::json& profile)
	{
		CHECK(GetKeys(profile) == std::set<std::string> { "frames", "frame", "framesPerSecond", "stageSum", "untimed", "jobs", "stages", "gpu", "renderStats" });
		CHECK(profile["frames"].is_number_unsigned());
		CHECK(profile["framesPerSecond"].is_number());
		CHECK(GetKeys(profile["frame"]) == c_TimeKeys);
		CHECK(GetKeys(profile["stageSum"]) == c_TimeKeys);
		CHECK(GetKeys(profile["untimed"]) == c_TimeKeys);
		CHECK(GetKeys(profile["jobs"]) == std::set<std::string> { "workerThreads", "ioThreads", "backgroundThreads", "workerBusyPercent", "ioBusyPercent",
			"backgroundBusyPercent", "running", "queued" });
		// Every built-in stage, whether or not it ran, with the same keys.
		const nlohmann::json& stages = profile["stages"];
		REQUIRE(stages.is_object());
		for (const BuiltinFrameStage& builtin : c_BuiltinFrameStages)
		{
			CAPTURE(builtin.Name);
			const std::string name(builtin.Name);
			REQUIRE(stages.contains(name));
			std::set<std::string> keys = c_TimeKeys;
			keys.insert("inFrameTime");
			CHECK(GetKeys(stages[name]) == keys);
			CHECK(stages[name]["inFrameTime"] == builtin.InWorkTime);
			for (const std::string& key : c_TimeKeys)
				CHECK(stages[name][key].is_number());
		}
		// The main thread's stages only: the GPU's are in gpu.
		uint32_t cpuStages = 0;
		for (FrameStageID stage = 0; stage < FrameProfiler::Get().GetStageCount(); stage++)
			cpuStages += FrameProfiler::Get().GetStageKind(stage) == FrameStageKind::CPU ? 1 : 0;
		CHECK(stages.size() == cpuStages);
		CHECK_FALSE(stages.contains("GPU.Opaque"));

		const nlohmann::json& gpu = profile["gpu"];
		CHECK(GetKeys(gpu) == std::set<std::string> { "frames", "latencyFrames", "busy", "stages", "boundBy" });
		CHECK(gpu["frames"].is_number_unsigned());
		CHECK(gpu["latencyFrames"].is_number());
		CHECK(GetKeys(gpu["busy"]) == c_TimeKeys);
		CHECK(std::set<std::string> { "gpu", "cpu", "capped", "unknown" }.count(gpu["boundBy"].get<std::string>()) == 1);
		// Every GPU stage the renderer registered.
		for (const char* name : { "GPU.Uploads", "GPU.Setup", "GPU.Cull", "GPU.Shadows", "GPU.Prepass", "GPU.AO", "GPU.Opaque", "GPU.Transparent", "GPU.Post",
			"GPU.Overlays" })
		{
			CAPTURE(name);
			REQUIRE(gpu["stages"].contains(name));
			CHECK(GetKeys(gpu["stages"][name]) == c_TimeKeys);
		}
	}

}

TEST_SUITE("Editor.Perf")
{
	TEST_CASE("editor.perf shows where the frames of the playing feature scene go")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("EditorPerf");
		const std::filesystem::path projectFile = CopyFeatureProject(directory / "Project");
		ScopedAudioEngine audio;
		REQUIRE(audio.Initialized);

		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		Tests::RunCommandToCompletion(context, commands, "project.open", { { "path", FileSystem::ToUTF8(projectFile) } });
		Run(context, commands, "script.load", { { "path", FileSystem::ToUTF8(GetFeatureScriptModule()) } });
		Tests::RunCommandToCompletion(context, commands, "scene.open", { { "scene", "Scenes/Feature.stscene" } });
		LoadAllAssets(*context.GetAssetManager());
		Run(context, commands, "play.start");

		// Frames as the application runs them: the editor's update between the profiler's frame markers, ended with the
		// frame's work time.
		FrameProfiler& profiler = FrameProfiler::Get();
		profiler.Reset();
		for (uint32_t frame = 0; frame < c_ProfiledFrames; frame++)
		{
			profiler.BeginFrame();
			const auto start = std::chrono::steady_clock::now();
			context.Update(Timestep(c_FeatureFrameTime));
			profiler.EndFrame(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
		}
		REQUIRE(profiler.GetRecordedFrameCount() == c_ProfiledFrames);

		const nlohmann::json profile = Run(context, commands, "editor.perf", { { "frames", c_ProfiledFrames } });
		INFO(profile.dump(1));
		CheckProfileSchema(profile);
		CHECK(profile["frames"] == c_ProfiledFrames);
		// The scripts, physics and transforms of the playing scene took time.
		const nlohmann::json& stages = profile["stages"];
		CHECK(stages["Scene.Scripts"]["avgMs"].get<float>() > 0.0f);
		CHECK(stages["Scene.Physics"]["avgMs"].get<float>() > 0.0f);
		CHECK(stages["Scene.Transforms"]["avgMs"].get<float>() > 0.0f);
		CHECK(stages["Assets.Update"]["avgMs"].get<float>() > 0.0f);
		// No renderer and no UI ran.
		CHECK(stages["Render.Submit"]["maxMs"] == 0.0f);
		CHECK(stages["Editor.UI"]["maxMs"] == 0.0f);
		CHECK(profile["frame"]["avgMs"].get<float>() > 0.0f);
		CHECK(profile["framesPerSecond"].get<float>() > 0.0f);
		// Without a GPU: no GPU times, nothing bounds the frames known, and no viewport frame.
		CHECK(profile["gpu"]["frames"] == 0);
		CHECK(profile["gpu"]["boundBy"] == "unknown");
		CHECK(profile["renderStats"].is_null());

		// In every frame the stages add up to at most the frame's time, and so do their statistics.
		for (uint32_t age = 0; age < c_ProfiledFrames; age++)
		{
			const FrameSample frame = profiler.GetFrame(age);
			double sum = 0.0;
			for (FrameStageID stage = 0; stage < frame.StageMs.size(); stage++)
			{
				if (profiler.IsStageInWorkTime(stage))
					sum += frame.StageMs[stage];
			}
			CAPTURE(age);
			CHECK(sum <= frame.WorkMs + 1.0e-3);
		}
		CHECK(profile["stageSum"]["avgMs"].get<float>() <= profile["frame"]["avgMs"].get<float>() + 1.0e-3f);
		CHECK(profile["stageSum"]["maxMs"].get<float>() <= profile["frame"]["maxMs"].get<float>() + 1.0e-3f);

		// Fewer frames than asked for: what there is. Invalid counts are refused.
		CHECK(Run(context, commands, "editor.perf", { { "frames", 600 } })["frames"] == c_ProfiledFrames);
		CHECK(commands.Execute(context, "editor.perf", { { "frames", 0 } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(commands.Execute(context, "editor.perf", { { "frames", 601 } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(commands.Execute(context, "editor.perf", { { "frame", 10 } }).ErrorKind == EditorCommandError::InvalidParameters);
		Run(context, commands, "play.stop");

		// Without recorded frames the shape stays the same.
		profiler.Reset();
		const nlohmann::json empty = Run(context, commands, "editor.perf");
		CheckProfileSchema(empty);
		CHECK(empty["frames"] == 0);
		CHECK(empty["frame"]["maxMs"] == 0.0f);
	}
}
