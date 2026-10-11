#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/PropertyEdit.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Project/GameManifest.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/SceneSerializer.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A project with two saved scenes, "Scenes/One.stscene" (an entity "Hero", the edited scene) and "Scenes/Two.stscene"
	// (an entity "Boss").
	struct SceneLoadProject
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		UUID One = UUID::Null();
		UUID Two = UUID::Null();

		explicit SceneLoadProject(const std::filesystem::path& directory)
		{
			Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Loads" } });
			Run("entity.create", { { "name", "Boss" } });
			Two = ToUUID(Run("scene.saveAs", { { "path", "Scenes/Two.stscene" } })["scene"]);
			Run("scene.new");
			Run("entity.create", { { "name", "Hero" } });
			One = ToUUID(Run("scene.saveAs", { { "path", "Scenes/One.stscene" } })["scene"]);
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		static UUID ToUUID(const nlohmann::json& json)
		{
			const std::optional<UUID> uuid = UUIDFromJson(json);
			REQUIRE(uuid.has_value());
			return *uuid;
		}
	};

}

TEST_SUITE("Editor.SceneLoad")
{
	TEST_CASE("scene.open loads the scene while the edited one stays shown and read-only, and answers once it replaced it")
	{
		SceneLoadProject project(CreateTemporaryDirectory("EditorSceneOpen") / "Loads");
		EditorContext& context = project.Context;
		const Ref<Scene> edited = context.GetEditScene();

		EditorCommandResult result = project.Commands.Execute(context, "scene.open", { { "scene", UUIDToJson(project.Two) } });
		REQUIRE(result.Success);
		REQUIRE(result.IsPending());
		CHECK(context.IsSceneLoading());
		CHECK(context.GetSceneLoadStatus().Scene == project.Two);

		// Until it is in, the edited scene is what the editor shows, and it cannot change: edits and play mode are refused.
		CHECK(context.GetEditScene() == edited);
		const EditorCommandResult create = project.Commands.Execute(context, "entity.create", { { "name", "Lost" } });
		CHECK_FALSE(create.Success);
		CHECK(create.Error.find("read-only") != std::string::npos);
		CHECK_FALSE(edited->FindEntityByName("Lost").IsValid());
		const EditorCommandResult play = project.Commands.Execute(context, "play.start");
		CHECK_FALSE(play.Success);
		CHECK(play.Error.find("being opened") != std::string::npos);
		CHECK_FALSE(context.IsPlaying());
		// Settings, undo and property edits (the Inspector's) are refused the same way, before anything changes.
		const glm::vec3 gravity = edited->GetSettings().Gravity;
		const EditorCommandResult settings = project.Commands.Execute(context, "scene.setSettings", { { "gravity", { 0.0f, -1.0f, 0.0f } } });
		CHECK(settings.ErrorKind == EditorCommandError::Failed);
		CHECK(settings.Error.find("read-only") != std::string::npos);
		CHECK(edited->GetSettings().Gravity == gravity);
		const EditorCommandResult undo = project.Commands.Execute(context, "edit.undo");
		CHECK(undo.ErrorKind == EditorCommandError::Failed);
		CHECK(undo.Error.find("read-only") != std::string::npos);
		const Entity hero = edited->FindEntityByName("Hero");
		REQUIRE(hero);
		const ComponentInfo* transform = ComponentRegistry::Find("Transform");
		REQUIRE(transform);
		const PropertyInfo* translation = transform->FindProperty("Translation");
		REQUIRE(translation);
		std::string error;
		CHECK_FALSE(SetPropertyWithUndo(context, hero, *transform, *translation, PropertyValue(glm::vec3(5.0f)), &error));
		CHECK(error.find("read-only") != std::string::npos);
		CHECK(hero.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
		const nlohmann::json status = project.Run("editor.status");
		REQUIRE(status["sceneLoad"].is_object());
		CHECK(status["sceneLoad"]["scene"] == UUIDToJson(project.Two));
		CHECK(status["sceneLoad"]["progress"].get<double>() >= 0.0);
		CHECK(status["memory"]["workingSetMB"].get<double>() > 0.0);
		const nlohmann::json loading = project.Run("scene.loadStatus");
		REQUIRE(loading["open"].is_object());
		CHECK(loading["open"]["scene"] == UUIDToJson(project.Two));
		CHECK(loading["open"]["state"] == "LoadingScene");
		CHECK(loading["play"].is_null());

		result = CompleteCommand(context, std::move(result));
		REQUIRE_MESSAGE(result.Success, result.Error);
		CHECK(result.Value["scene"] == UUIDToJson(project.Two));
		CHECK(result.Value["name"] == "Two");
		CHECK(result.Value["entities"] == 1);
		CHECK(result.Value["seconds"].get<double>() >= 0.0);
		CHECK(result.Value["frames"].get<int>() >= 1);
		CHECK_FALSE(context.IsSceneLoading());
		CHECK(context.GetEditScene() != edited);
		CHECK(context.GetSceneHandle() == project.Two);
		CHECK(context.GetEditScene()->FindEntityByName("Boss").IsValid());
		CHECK(context.GetSceneLoadStatus().State == SceneLoadState::Ready);
		CHECK(project.Run("editor.status")["sceneLoad"].is_null());
		const nlohmann::json loaded = project.Run("scene.loadStatus");
		CHECK(loaded["open"].is_null());
		CHECK(loaded["lastOpen"]["state"] == "Ready");
		CHECK(loaded["lastOpen"]["error"].is_null());
		// Editable again.
		project.Run("entity.create", { { "name", "Added" } });
		CHECK(context.GetUndoStack().CanUndo());
	}

	TEST_CASE("Opening another scene, a new scene or closing the project cancels a scene load")
	{
		SceneLoadProject project(CreateTemporaryDirectory("EditorSceneOpenCancel") / "Loads");
		EditorContext& context = project.Context;

		// A later scene.open replaces an earlier one, which fails as cancelled.
		EditorCommandResult first = project.Commands.Execute(context, "scene.open", { { "scene", UUIDToJson(project.Two) } });
		EditorCommandResult second = project.Commands.Execute(context, "scene.open", { { "scene", UUIDToJson(project.One) } });
		first = CompleteCommand(context, std::move(first));
		CHECK_FALSE(first.Success);
		CHECK(first.ErrorKind == EditorCommandError::Cancelled);
		second = CompleteCommand(context, std::move(second));
		REQUIRE_MESSAGE(second.Success, second.Error);
		CHECK(context.GetSceneHandle() == project.One);

		// scene.new cancels it; the scene it was loading never replaces the new one.
		EditorCommandResult opening = project.Commands.Execute(context, "scene.open", { { "scene", UUIDToJson(project.Two) } });
		project.Run("scene.new", { { "name", "Fresh" } });
		CHECK_FALSE(context.IsSceneLoading());
		CHECK(context.GetSceneLoadStatus().State == SceneLoadState::Cancelled);
		opening = CompleteCommand(context, std::move(opening));
		CHECK(opening.ErrorKind == EditorCommandError::Cancelled);
		for (int frame = 0; frame < 5; frame++)
			context.Update(Timestep(1.0f / 60.0f));
		CHECK(context.GetEditScene()->GetName() == "Fresh");

		// Every pin of the cancelled loads is released.
		CHECK(context.GetAssetManager()->GetPinCount(project.Two) == 0);

		// Closing the project ends a load too.
		EditorCommandResult closing = project.Commands.Execute(context, "scene.open", { { "scene", UUIDToJson(project.Two) } });
		project.Run("project.close");
		CHECK_FALSE(context.IsSceneLoading());
		closing = CompleteCommand(context, std::move(closing));
		CHECK(closing.ErrorKind == EditorCommandError::Cancelled);

		// What cannot be opened fails before anything starts.
		CHECK(project.Commands.Execute(context, "scene.open", { { "scene", UUIDToJson(project.Two) } }).ErrorKind != EditorCommandError::None);
	}

	TEST_CASE("The viewport shows a scene load's progress over the scene still shown, and the assets pill counts it")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorSceneLoadOverlay"));
		editor.Frames(3);
		EditorContext& context = editor.Context();
		const AssetHandle scene = context.GetSceneHandle();
		REQUIRE(scene.IsValid());

		// The frame in which the load starts draws the card; the next frame's update brings the scene in.
		editor.FrameWith([&]()
		{
			REQUIRE(context.BeginOpenScene(scene));
		});
		REQUIRE(context.IsSceneLoading());
		const std::optional<UI::ItemProbe::Item> bar = UI::ItemProbe::Find("Viewport.SceneLoad");
		REQUIRE(bar.has_value());
		CHECK(bar->Frame == ImGui::GetFrameCount());
		CHECK(bar->Text.find("Reading the scene") != std::string::npos);
		const std::optional<UI::ItemProbe::Item> pill = UI::ItemProbe::Find("Status.Assets");
		REQUIRE(pill.has_value());
		CHECK(pill->Text.find("Opening scene") == 0);
		CHECK(ImGuiHarness::CountFallbackGlyphs(editor.FindPanelWindow(EditorPanels::c_Viewport)) == 0);
		// The Hierarchy and the Inspector say that the scene shown is read-only (their contents are disabled).
		for (const char* banner : { "Hierarchy.SceneLoadBanner", "Inspector.SceneLoadBanner" })
		{
			const std::optional<UI::ItemProbe::Item> drawn = UI::ItemProbe::Find(banner);
			REQUIRE_MESSAGE(drawn.has_value(), banner);
			CHECK(drawn->Frame == ImGui::GetFrameCount());
			CHECK(drawn->Text.find("read-only") != std::string::npos);
		}
		CHECK(ImGuiHarness::CountFallbackGlyphs(editor.FindPanelWindow(EditorPanels::c_Hierarchy)) == 0);

		// Once the scene is in, neither shows it any more.
		REQUIRE(editor.FramesUntil([&]() { return !context.IsSceneLoading(); }));
		editor.Frames(1);
		const std::optional<UI::ItemProbe::Item> barAfter = UI::ItemProbe::Find("Viewport.SceneLoad");
		CHECK((!barAfter || barAfter->Frame < ImGui::GetFrameCount()));
		const std::optional<UI::ItemProbe::Item> pillAfter = UI::ItemProbe::Find("Status.Assets");
		REQUIRE(pillAfter.has_value());
		CHECK(pillAfter->Text.find("Opening scene") == std::string::npos);
		const std::optional<UI::ItemProbe::Item> bannerAfter = UI::ItemProbe::Find("Hierarchy.SceneLoadBanner");
		CHECK((!bannerAfter || bannerAfter->Frame < ImGui::GetFrameCount()));
	}

	TEST_CASE("project.open opens the project at once and answers once its start scene replaced the empty scene")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("EditorProjectOpen") / "Loads";
		UUID one = UUID::Null();
		{
			SceneLoadProject project(directory);
			project.Run("project.setStartScene", { { "scene", UUIDToJson(project.One) } });
			one = project.One;
		}

		EditorContext context(EditorContextSpecification { false });
		EditorCommandRegistry commands;
		EditorCommandResult result = commands.Execute(context, "project.open", { { "path", FileSystem::ToUTF8(directory) } });
		REQUIRE_MESSAGE(result.Success, result.Error);
		REQUIRE(result.IsPending());

		// The project is open; its start scene loads while the empty scene shows, read-only.
		REQUIRE(context.HasProject());
		CHECK(context.IsSceneLoading());
		CHECK(context.GetSceneLoadStatus().StartScene);
		CHECK(context.GetSceneLoadStatus().Scene == one);
		CHECK_FALSE(context.GetSceneHandle().IsValid());
		const EditorCommandResult create = commands.Execute(context, "entity.create", { { "name", "Lost" } });
		CHECK_FALSE(create.Success);
		CHECK(create.Error.find("read-only") != std::string::npos);
		const nlohmann::json loading = RunCommandToCompletion(context, commands, "scene.loadStatus");
		REQUIRE(loading["open"].is_object());
		CHECK(loading["open"]["scene"] == UUIDToJson(one));
		CHECK(loading["open"]["startScene"] == true);
		CHECK(RunCommandToCompletion(context, commands, "editor.status")["sceneLoad"]["startScene"] == true);
		// scene.loadStatus with wait answers once no scene is being opened.
		EditorCommandResult waiting = commands.Execute(context, "scene.loadStatus", { { "wait", true } });
		REQUIRE(waiting.Success);
		REQUIRE(waiting.IsPending());

		result = CompleteCommand(context, std::move(result));
		REQUIRE_MESSAGE(result.Success, result.Error);
		CHECK(result.Value["startScene"] == UUIDToJson(one));
		CHECK(result.Value["startSceneOpened"] == true);
		CHECK_FALSE(result.Value.contains("startSceneError"));
		CHECK(result.Value["seconds"].get<double>() >= 0.0);
		CHECK(FileSystem::FromUTF8(result.Value["projectFile"].get<std::string>()) == context.GetProject()->GetProjectFile());
		CHECK_FALSE(context.IsSceneLoading());
		CHECK(context.GetSceneHandle() == one);
		CHECK(context.GetEditScene()->FindEntityByName("Hero").IsValid());
		waiting = CompleteCommand(context, std::move(waiting));
		REQUIRE_MESSAGE(waiting.Success, waiting.Error);
		CHECK(waiting.Value["open"].is_null());
		CHECK(waiting.Value["lastOpen"]["startScene"] == true);
		CHECK(waiting.Value["lastOpen"]["state"] == "Ready");
		// Without a load under way it answers at once.
		CHECK_FALSE(commands.Execute(context, "scene.loadStatus", { { "wait", true } }).IsPending());
		CHECK(commands.Execute(context, "scene.loadStatus", { { "wait", "yes" } }).ErrorKind == EditorCommandError::InvalidParameters);

		// A project without a start scene answers at once.
		const std::filesystem::path empty = CreateTemporaryDirectory("EditorProjectOpenEmpty") / "Empty";
		RunCommandToCompletion(context, commands, "project.create", { { "directory", FileSystem::ToUTF8(empty) }, { "name", "Empty" } });
		result = commands.Execute(context, "project.open", { { "path", FileSystem::ToUTF8(empty) } });
		REQUIRE_MESSAGE(result.Success, result.Error);
		CHECK_FALSE(result.IsPending());
		CHECK(result.Value["startScene"].is_null());
		CHECK(result.Value["startSceneOpened"] == false);

		// Opening another project while the start scene loads: the first command ends as cancelled, the second opens its own.
		EditorCommandResult first = commands.Execute(context, "project.open", { { "path", FileSystem::ToUTF8(directory) } });
		REQUIRE(first.IsPending());
		EditorCommandResult second = commands.Execute(context, "project.open", { { "path", FileSystem::ToUTF8(directory) } });
		first = CompleteCommand(context, std::move(first));
		CHECK_FALSE(first.Success);
		CHECK(first.ErrorKind == EditorCommandError::Cancelled);
		second = CompleteCommand(context, std::move(second));
		REQUIRE_MESSAGE(second.Success, second.Error);
		CHECK(second.Value["startSceneOpened"] == true);
		CHECK(context.GetSceneHandle() == one);

		// Opening another scene instead: the project stays open, and project.open says its start scene was not opened.
		EditorCommandResult replaced = commands.Execute(context, "project.open", { { "path", FileSystem::ToUTF8(directory) } });
		REQUIRE(replaced.IsPending());
		EditorCommandResult other = commands.Execute(context, "scene.open", { { "scene", "Scenes/Two.stscene" } });
		replaced = CompleteCommand(context, std::move(replaced));
		REQUIRE_MESSAGE(replaced.Success, replaced.Error);
		CHECK(replaced.Value["startSceneOpened"] == false);
		CHECK(replaced.Value["startSceneError"].get<std::string>().find("Another scene") != std::string::npos);
		other = CompleteCommand(context, std::move(other));
		REQUIRE_MESSAGE(other.Success, other.Error);
		CHECK(context.GetEditScene()->FindEntityByName("Boss").IsValid());
	}

	TEST_CASE("A project whose start scene cannot be opened opens with an empty scene, and project.open says why")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("EditorProjectOpenBroken") / "Loads";
		{
			SceneLoadProject project(directory);
			project.Run("project.setStartScene", { { "scene", UUIDToJson(project.One) } });
		}
		// The start scene's document is damaged since.
		REQUIRE(FileSystem::WriteText(directory / "Assets" / "Scenes" / "One.stscene", "{ \"Strata\": "));

		EditorContext context(EditorContextSpecification { false });
		EditorCommandRegistry commands;
		const EditorCommandResult result = CompleteCommand(context, commands.Execute(context, "project.open", { { "path", FileSystem::ToUTF8(directory) } }));
		REQUIRE_MESSAGE(result.Success, result.Error);
		CHECK(result.Value["startSceneOpened"] == false);
		REQUIRE(result.Value["startSceneError"].is_string());
		CHECK_FALSE(result.Value["startSceneError"].get<std::string>().empty());
		CHECK(context.HasProject());
		CHECK_FALSE(context.IsSceneLoading());
		CHECK_FALSE(context.GetSceneHandle().IsValid());
		CHECK(context.GetSceneLoadStatus().State == SceneLoadState::Failed);
		const nlohmann::json status = RunCommandToCompletion(context, commands, "scene.loadStatus");
		CHECK(status["lastOpen"]["startScene"] == true);
		CHECK(status["lastOpen"]["state"] == "Failed");
		// The empty scene is editable.
		RunCommandToCompletion(context, commands, "entity.create", { { "name", "Fresh" } });

		// Waiting for it (OpenProject, for tools and tests) gives the same.
		EditorContext waited(EditorContextSpecification { false });
		std::string error;
		REQUIRE_MESSAGE(waited.OpenProject(directory, &error), error);
		CHECK_FALSE(waited.GetSceneHandle().IsValid());
		CHECK(waited.GetSceneLoadStatus().State == SceneLoadState::Failed);
	}

	TEST_CASE("A window opens the project's start scene in the background")
	{
		EditorOptions options = WithFeatureProject("EditorStartSceneInBackground");
		options.OpenStartSceneInBackground = true;
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, options);
		EditorContext& context = editor.Context();
		REQUIRE(context.HasProject());
		const AssetHandle startScene = context.GetProject()->GetConfig().StartScene;
		REQUIRE(startScene.IsValid());
		// The editor started without waiting for it: its frames go on over the empty scene (the viewport shows the load, as
		// for scene.open) until the start scene is in.
		REQUIRE(context.IsSceneLoading());
		CHECK(context.GetSceneLoadStatus().StartScene);
		CHECK(context.GetSceneLoadStatus().Scene == startScene);
		CHECK_FALSE(context.GetSceneHandle().IsValid());

		REQUIRE(editor.FramesUntil([&]() { return !context.IsSceneLoading(); }));
		CHECK(context.GetSceneHandle() == startScene);
		CHECK(context.GetSceneLoadStatus().State == SceneLoadState::Ready);
		CHECK(context.GetEditScene()->FindEntityByName("Ball").IsValid());
	}

	TEST_CASE("Exports name the project's loading scene in the manifest and keep it in pruned packs")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("EditorExportLoadingScene");
		SceneLoadProject project(directory / "Loads");
		EditorContext& context = project.Context;
		project.Run("project.setStartScene", { { "scene", UUIDToJson(project.One) } });

		// Not a scene: refused, and nothing changes.
		const EditorCommandResult notScene = project.Commands.Execute(context, "project.setExportSettings", { { "loadingScene", "Scenes" } });
		CHECK(notScene.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK_FALSE(context.GetProject()->GetConfig().Export.LoadingScene.IsValid());
		CHECK(project.Commands.Execute(context, "project.setExportSettings", nlohmann::json::object()).ErrorKind == EditorCommandError::InvalidParameters);

		const nlohmann::json settings = project.Run("project.setExportSettings", { { "loadingScene", "Scenes/Two.stscene" } });
		CHECK(settings["loadingScene"] == UUIDToJson(project.Two));
		CHECK(settings["alwaysInclude"].empty());
		// Saved with the project.
		std::string error;
		const Ref<Project> saved = Project::Load(context.GetProject()->GetProjectFile(), &error);
		REQUIRE_MESSAGE(saved, error);
		CHECK(saved->GetConfig().Export.LoadingScene == project.Two);

		// A pruned export: the start scene uses nothing of Two, yet Two ships as the loading scene.
		const nlohmann::json exported = project.Run("project.export", { { "directory", FileSystem::ToUTF8(directory / "Build") }, { "includeRuntime", false },
			{ "prune", true } });
		const std::optional<GameManifest> manifest = GameManifest::Load(FileSystem::FromUTF8(exported["manifest"].get<std::string>()), &error);
		REQUIRE_MESSAGE(manifest, error);
		CHECK(manifest->StartScene == project.One);
		CHECK(manifest->LoadingScene == project.Two);
		for (const nlohmann::json& excluded : exported["excludedAssets"])
			CHECK(excluded["path"] != "Scenes/Two.stscene");

		// Null takes it off again.
		CHECK(project.Run("project.setExportSettings", { { "loadingScene", nullptr } })["loadingScene"].is_null());
		CHECK_FALSE(context.GetProject()->GetConfig().Export.LoadingScene.IsValid());
	}

	TEST_CASE("Sub-scenes load into the running game and unload through commands, and play.stop discards them")
	{
		SceneLoadProject project(CreateTemporaryDirectory("EditorSubScenes") / "Loads");
		EditorContext& context = project.Context;
		const auto failure = [&](std::string_view name, const nlohmann::json& parameters)
		{
			return CompleteCommand(context, project.Commands.Execute(context, name, parameters));
		};

		// Edit mode: sub-scenes are runtime only.
		const EditorCommandResult inEditMode = failure("scene.loadAdditive", { { "scene", UUIDToJson(project.Two) } });
		CHECK_FALSE(inEditMode.Success);
		CHECK(inEditMode.Error.find("play.start") != std::string::npos);
		CHECK(project.Run("scene.loadStatus")["subScenes"].empty());

		project.Run("play.start");
		const Ref<Scene> running = context.GetActiveScene();
		const size_t entities = running->GetEntityCount();
		const nlohmann::json loaded = project.Run("scene.loadAdditive", { { "scene", "Scenes/Two.stscene" } });
		CHECK(loaded["state"] == "Loaded");
		CHECK(loaded["scene"] == UUIDToJson(project.Two));
		CHECK(loaded["totalEntities"] == 1);
		CHECK(loaded["slices"] == 1);
		CHECK(loaded["warning"].is_string());
		REQUIRE(loaded["roots"].size() == 1);
		const std::optional<UUID> boss = UUIDFromJson(loaded["roots"][0]);
		REQUIRE(boss.has_value());
		CHECK(running->GetEntityByUUID(*boss).GetName() == "Boss");
		CHECK(running->GetEntityCount() == entities + 1);
		const int64_t subScene = loaded["subScene"].get<int64_t>();
		const nlohmann::json status = project.Run("scene.loadStatus");
		REQUIRE(status["subScenes"].size() == 1);
		CHECK(status["subScenes"][0]["subScene"] == subScene);
		CHECK(status["subScenes"][0]["state"] == "Loaded");
		CHECK(status["subScenes"][0]["progress"] == 1.0);

		// The same scene again: its IDs are there already.
		const EditorCommandResult twice = failure("scene.loadAdditive", { { "scene", UUIDToJson(project.Two) } });
		CHECK_FALSE(twice.Success);
		CHECK(twice.Error.find(boss->ToString()) != std::string::npos);
		CHECK(running->GetEntityCount() == entities + 1);
		CHECK(project.Run("scene.loadStatus")["subScenes"].size() == 2); // The failed one stays, with its error, until unloaded
		const int64_t failed = project.Run("scene.loadStatus")["subScenes"][1]["subScene"].get<int64_t>();
		CHECK(project.Run("scene.unloadAdditive", { { "subScene", failed } })["entities"] == entities + 1);

		const nlohmann::json unloaded = project.Run("scene.unloadAdditive", { { "subScene", subScene } });
		CHECK(unloaded["entities"] == entities);
		CHECK_FALSE(running->GetEntityByUUID(*boss).IsValid());
		CHECK(project.Run("scene.loadStatus")["subScenes"].empty());
		const EditorCommandResult unknown = failure("scene.unloadAdditive", { { "subScene", subScene } });
		CHECK(unknown.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(failure("scene.unloadAdditive", nlohmann::json::object()).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(failure("scene.loadAdditive", { { "scene", "Scenes/Missing.stscene" } }).ErrorKind == EditorCommandError::InvalidParameters);

		// Loaded again and left there: play.stop discards it with the running copy.
		project.Run("scene.loadAdditive", { { "scene", UUIDToJson(project.Two) } });
		project.Run("play.stop");
		CHECK_FALSE(context.GetEditScene()->FindEntityByName("Boss").IsValid());
		CHECK(context.GetEditScene()->GetSubScenes().empty());
		CHECK_FALSE(context.IsSceneModified());
	}
}
