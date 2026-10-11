#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Project/GameManifest.h>
#include <Strata/Reflection/PropertyJson.h>
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

		// Once the scene is in, neither shows it any more.
		REQUIRE(editor.FramesUntil([&]() { return !context.IsSceneLoading(); }));
		editor.Frames(1);
		const std::optional<UI::ItemProbe::Item> barAfter = UI::ItemProbe::Find("Viewport.SceneLoad");
		CHECK((!barAfter || barAfter->Frame < ImGui::GetFrameCount()));
		const std::optional<UI::ItemProbe::Item> pillAfter = UI::ItemProbe::Find("Status.Assets");
		REQUIRE(pillAfter.has_value());
		CHECK(pillAfter->Text.find("Opening scene") == std::string::npos);
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
}
