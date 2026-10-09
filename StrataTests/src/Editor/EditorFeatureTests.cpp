#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Runtime/GameRuntime.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Runs editor commands like automation does and fails the test with the command's error.
	struct FeatureEditor
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(std::string(name), ": ", result.Error);
			REQUIRE(result.Success);
			REQUIRE_FALSE(result.IsPending());
			return result.Value;
		}

		std::string FindEntity(std::string_view name)
		{
			const nlohmann::json found = Run("entity.find", { { "name", name } })["entities"];
			INFO("Entity '", std::string(name), "'");
			REQUIRE(found.size() == 1);
			return found[0].get<std::string>();
		}

		float GetHeight(const std::string& entity)
		{
			return Run("component.get", { { "entity", entity }, { "component", "Transform" } })["values"]["Translation"][1].get<float>();
		}
	};

}

TEST_SUITE("Editor.FeatureTest")
{
	TEST_CASE("The editor plays the feature project through commands and exports it as a game that plays it too")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("EditorFeatureTest");
		const std::filesystem::path projectFile = CopyFeatureProject(directory / "Project");
		// Scenes played while it is active (by the editor and by the exported game) run the feature scripts.
		ScopedScriptEngine engine(GetFeatureScriptModule());
		ScopedScriptLogLevel scriptLogLevel;
		LogCapture log;

		FeatureEditor editor;
		editor.Run("project.open", { { "path", FileSystem::ToUTF8(projectFile) } });
		CHECK(editor.Run("project.info")["startScene"] == "F7A0000000000001");
		editor.Run("scene.open", { { "scene", "Scenes/Feature.stscene" } });
		const std::string ball = editor.FindEntity("Ball");
		CHECK(editor.GetHeight(ball) == doctest::Approx(4.0f));

		// A loading screen (the editor itself streams assets in while playing).
		LoadAllAssets(*editor.Context.GetAssetManager());
		editor.Run("play.start");
		const Ref<Scene> played = editor.Context.GetActiveScene();
		REQUIRE(played != editor.Context.GetEditScene());
		PlayFeatureScene(*played, *engine, [&]() { editor.Context.Update(Timestep(c_FeatureFrameTime)); });
		CheckFeatureResults(*played, *engine);

		// Commands see the running copy: the ball rests on the platform, and edits while playing come with a warning.
		CHECK(std::abs(editor.GetHeight(ball) - 1.65f) < 0.05f);
		const std::string sign = editor.FindEntity("Sign");
		const nlohmann::json edit = editor.Run("component.set", { { "entity", sign }, { "component", "Text" }, { "values", { { "Text", "Edited while playing" } } } });
		CHECK(edit.contains("warning"));

		editor.Run("play.stop");
		CheckFeatureJournal(*played, *engine);
		// The edited scene is untouched by play mode.
		CHECK(editor.GetHeight(ball) == doctest::Approx(4.0f));
		CHECK(editor.Run("component.get", { { "entity", sign }, { "component", "Text" } })["values"]["Text"] == "Hello, Strata");
		CHECK_FALSE(editor.Context.IsSceneModified());

		// The exported game (asset pack and manifest) plays the same scenario in the game runtime.
		const nlohmann::json exported = editor.Run("project.export", { { "directory", FileSystem::ToUTF8(directory / "Build") }, { "includeRuntime", false } });
		const std::filesystem::path manifest = FileSystem::FromUTF8(exported["manifest"].get<std::string>());
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		const Ref<Scene> gameScene = game->GetScene();
		PlayFeatureScene(*gameScene, *engine, [&]() { game->Update(Timestep(c_FeatureFrameTime)); });
		CheckFeatureResults(*gameScene, *engine);
		game.reset(); // Stops the scene
		CheckFeatureJournal(*gameScene, *engine);

		// The game runtime starts its scene right away and streams assets in (it has no loading screen yet), so the
		// platform's mesh collider waits for its mesh at first.
		constexpr std::string_view c_Tolerated[] = { "the mesh collider of 'Platform' waits for mesh" };
		CheckFeatureLog(log.GetEntries(), 2, c_Tolerated);
	}
}
