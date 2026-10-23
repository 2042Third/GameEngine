#include <doctest/doctest.h>

#include "Audio/AudioTestUtils.h"
#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetPack.h>
#include <Strata/Asset/RuntimeAssetManager.h>
#include <Strata/Core/EngineServices.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Runtime/GameRuntime.h>
#include <Strata/Runtime/GameWindowService.h>
#include <Strata/Scene/CookedScene.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <map>
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
		// Audio without an output device (the editor's and the game's applications would initialize it), so that the scripts'
		// sounds play.
		ScopedAudioEngine audio;
		REQUIRE(audio.Initialized);
		ScopedScriptLogLevel scriptLogLevel;
		LogCapture log;

		// The editor runs the feature scripts, built in-tree, as the project's script module.
		FeatureEditor editor;
		Tests::RunCommandToCompletion(editor.Context, editor.Commands, "project.open", { { "path", FileSystem::ToUTF8(projectFile) } });
		CHECK(editor.Run("project.info")["startScene"] == "F7A0000000000001");
		editor.Run("script.load", { { "path", FileSystem::ToUTF8(GetFeatureScriptModule()) } });
		const Ref<ScriptEngine> engine = editor.Context.GetScriptEngine();
		REQUIRE(engine);
		Tests::RunCommandToCompletion(editor.Context, editor.Commands, "scene.open", { { "scene", "Scenes/Feature.stscene" } });
		const std::string ball = editor.FindEntity("Ball");
		CHECK(editor.GetHeight(ball) == doctest::Approx(4.0f));
		// The crate is an instance of its prefab with two overrides: its script's value and its lid's shadows, as saved.
		const nlohmann::json crate = editor.Run("prefab.status", { { "entity", "F7E0000000000031" } });
		CHECK(crate["overrides"] == nlohmann::json::array({
			{ { "entity", "F7E0000000000030" }, { "prefabEntity", "F7C0000000000001" }, { "component", "Script" }, { "property", "CrateScript.Value" },
				{ "kind", "Property" } },
			{ { "entity", "F7E0000000000031" }, { "prefabEntity", "F7C0000000000002" }, { "component", "MeshRenderer" }, { "property", "CastShadows" },
				{ "kind", "Property" } } }));
		CHECK(editor.Context.GetEditScene()->GetEntityByUUID(*UUIDFromJson(nlohmann::json("F7E0000000000030"))).GetComponent<PrefabInstanceComponent>().Overrides.size() == 2);
		CHECK(crate["outdated"] == false);

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

		// The game quits after the scenario: play mode stops.
		PlayFeatureQuitFrame(*played, *engine, [&]() { editor.Context.Update(Timestep(c_FeatureFrameTime)); });
		CHECK_FALSE(editor.Context.IsPlaying());
		CHECK_FALSE(played->IsRunning());
		CheckFeatureJournal(*played, *engine);
		// The edited scene is untouched by play mode.
		CHECK(editor.GetHeight(ball) == doctest::Approx(4.0f));
		CHECK(editor.Run("component.get", { { "entity", sign }, { "component", "Text" } })["values"]["Text"] == "Hello, Strata");
		CHECK_FALSE(editor.Context.IsSceneModified());

		// The exported game (asset pack, script module and manifest) plays the same scenario in the game runtime, with its own
		// copy of the module.
		const nlohmann::json exported = Tests::RunCommandToCompletion(editor.Context, editor.Commands, "project.export", { { "directory", FileSystem::ToUTF8(directory / "Build") }, { "includeRuntime", false } });
		REQUIRE(exported["scriptModule"].is_string());
		CHECK(FileSystem::FromUTF8(exported["scriptModule"].get<std::string>()).filename() == GetFeatureScriptModule().filename());
		// The project's identity goes with the game: its icon (Textures/Checker.png, 64 x 64 pixels: enlarged, with a warning)
		// lies next to the manifest, which names the title and version.
		REQUIRE(exported["icon"].is_string());
		CHECK(FileSystem::FromUTF8(exported["icon"].get<std::string>()).filename() == "FeatureTest.png");
		CHECK(exported["warnings"].size() == 1);
		const std::filesystem::path manifest = FileSystem::FromUTF8(exported["manifest"].get<std::string>());
		std::string error;
		// The pack holds the scenes, prefabs and models in their cooked form (project.export's default), which the game reads.
		{
			const Ref<RuntimeAssetManager> pack = RuntimeAssetManager::Create(FileSystem::FromUTF8(exported["assetPack"].get<std::string>()), &error);
			REQUIRE_MESSAGE(pack, error);
			std::map<AssetType, uint32_t> documents;
			for (const AssetPackEntry& entry : pack->GetPack().GetEntries())
			{
				const AssetType type = entry.Metadata.Type;
				if (type != AssetType::Scene && type != AssetType::Prefab && type != AssetType::Model)
					continue;
				std::vector<uint8_t> bytes;
				REQUIRE_MESSAGE(pack->GetPack().ReadData(entry, bytes, &error), error);
				CHECK_MESSAGE(CookedScene::IsCooked(bytes), entry.Metadata.Path);
				documents[type]++;
			}
			CHECK(documents[AssetType::Scene] >= 1);
			CHECK(documents[AssetType::Prefab] >= 1);
		}
		// The game's window as StrataRuntime provides it headless; saves in the test's directory instead of the user's data.
		GameWindowService gameWindow(nullptr);
		EngineServices::ScopedProvider<IGameWindow> gameWindowProvider(gameWindow);
		GameRuntimeOptions gameOptions;
		gameOptions.SaveFilesDirectory = directory / "Saves";
		const uint64_t cookedReads = CookedSceneReader::GetReadCount();
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error, gameOptions);
		REQUIRE_MESSAGE(game, error);
		CHECK(game->GetManifest().GetDisplayTitle() == "Strata Feature Test");
		CHECK(game->GetManifest().Version == "6.1.2.3");
		REQUIRE(game->WaitForStart());
		CHECK(CookedSceneReader::GetReadCount() > cookedReads); // The start scene, read cooked
		const Ref<ScriptEngine> gameEngine = game->GetScriptEngine();
		REQUIRE(gameEngine);
		CHECK(gameEngine != engine);
		CHECK(ScriptEngine::GetActive() == gameEngine);
		const Ref<Scene> gameScene = game->GetScene();
		const auto advanceGame = [&]() { game->Update(Timestep(c_FeatureFrameTime)); };
		PlayFeatureScene(*gameScene, *gameEngine, advanceGame);
		CheckFeatureResults(*gameScene, *gameEngine);
		CHECK_FALSE(game->GetScriptFault());
		// The game quits after the scenario: the runtime reports the exit code (StrataRuntime would exit with it).
		const int32_t exitCode = PlayFeatureQuitFrame(*gameScene, *gameEngine, advanceGame);
		CHECK(game->GetQuitRequest() == exitCode);
		game.reset(); // Stops the scene
		CheckFeatureJournal(*gameScene, *gameEngine);

		// The game runtime starts its scene right away and streams assets in (it has no loading screen yet), so the
		// platform's mesh collider waits for its mesh at first; the export enlarges the project's small icon.
		constexpr std::string_view c_Tolerated[] = { "the mesh collider of 'Platform' waits for mesh", "The game's icon is 64 x 64 pixels" };
		CheckFeatureLog(log.GetEntries(), 2, c_Tolerated);
	}
}
