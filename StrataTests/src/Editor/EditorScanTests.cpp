#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/SaveTestUtils.h"
#include "Perf/WorldFixture.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

// A project's assets scanned in the background as the editor opens it (EditorContextSpecification::ImportInBackground):
// the project is open at once, commands that need the whole registry wait for the scan, and the start scene opens after it.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_Materials = 200;

	ManyAssetsProject WriteProject(const std::string& name)
	{
		std::string error;
		std::optional<ManyAssetsProject> project = WriteManyAssetsProject(CreateTemporaryDirectory(name) / "Many", c_Materials, error);
		REQUIRE_MESSAGE(project.has_value(), error);
		return std::move(*project);
	}

	EditorContextSpecification InBackground()
	{
		EditorContextSpecification specification { false };
		specification.ImportInBackground = true;
		return specification;
	}

}

TEST_SUITE("Editor.Scan")
{
	TEST_CASE("While a project's assets are scanned, edits and asset queries wait, session queries answer, and the start scene opens after the scan")
	{
		GatedJobSystem jobs;
		const ManyAssetsProject project = WriteProject("EditorScanWaits");
		EditorContext context { InBackground() };
		EditorCommandRegistry commands;
		EditorCommandResult created;
		EditorCommandResult listed;
		{
			// The scan's job waits behind the held background thread.
			BackgroundGate gate;
			std::string error;
			REQUIRE_MESSAGE(context.BeginOpenProject(project.ProjectFile, &error), error);
			CHECK(context.HasProject());
			CHECK(context.IsScanningAssets());
			CHECK(context.IsWaitingForAssets());
			// The start scene waits for the scan: it counts as being opened.
			CHECK(context.IsSceneLoading());
			CHECK(context.GetSceneLoadStatus().StartScene);

			const EditorCommandResult status = commands.Execute(context, "editor.status", nlohmann::json::object());
			REQUIRE(status.Success);
			REQUIRE(status.Value["assets"]["scanning"].is_object());
			CHECK(status.Value["assets"]["scanning"]["phase"] == "Walking");

			created = commands.Execute(context, "entity.create", { { "name", "Waited" } });
			listed = commands.Execute(context, "asset.list", { { "type", "Material" }, { "includeBuiltin", false } });
			CHECK(created.IsPending());
			CHECK(listed.IsPending());
			// Session queries answer at once.
			CHECK_FALSE(commands.Execute(context, "scene.loadStatus", nlohmann::json::object()).IsPending());
			CHECK_FALSE(commands.Execute(context, "log.read", nlohmann::json::object()).IsPending());
			// The UI's direct commands are refused, saying why.
			const std::optional<std::string> waiting = commands.DescribeAssetScanWait(context, "entity.create");
			REQUIRE(waiting.has_value());
			CHECK(waiting->find("still being scanned") != std::string::npos);
			CHECK_FALSE(commands.DescribeAssetScanWait(context, "editor.status").has_value());
			for (int frame = 0; frame < 5; frame++)
			{
				context.Update(Timestep(1.0f / 60.0f));
				CHECK_FALSE(PollCommandResult(created, context));
				CHECK_FALSE(PollCommandResult(listed, context));
			}
		}

		// The scan ends, the start scene opens, and then the commands that waited run.
		REQUIRE(WaitUntil([&]()
		{
			context.Update(Timestep(1.0f / 60.0f));
			PollCommandResult(created, context);
			PollCommandResult(listed, context);
			return !created.IsPending() && !listed.IsPending();
		}, std::chrono::seconds(60)));
		CHECK_MESSAGE(created.Success, created.Error);
		REQUIRE_MESSAGE(listed.Success, listed.Error);
		CHECK(listed.Value["assets"].size() == c_Materials);
		CHECK_FALSE(context.IsWaitingForAssets());
		CHECK(context.GetSceneHandle() == project.Scene);
		// The edit went into the start scene, which was open by then: it is kept.
		CHECK(context.GetEditScene()->FindEntityByName("Waited"));
		CHECK(context.GetEditScene()->FindEntityByName("Cube 0"));
		const EditorCommandResult status = commands.Execute(context, "editor.status", nlohmann::json::object());
		CHECK(status.Value["assets"]["scanning"].is_null());
	}

	TEST_CASE("project.open of a project scanned in the background answers once its start scene is open")
	{
		GatedJobSystem jobs;
		const ManyAssetsProject project = WriteProject("EditorScanProjectOpen");
		EditorContext context { InBackground() };
		EditorCommandRegistry commands;
		const nlohmann::json opened = RunCommandToCompletion(context, commands, "project.open", { { "path", FileSystem::ToUTF8(project.ProjectFile) } });
		CHECK(opened["startSceneOpened"] == true);
		CHECK_FALSE(context.IsWaitingForAssets());
		CHECK(context.GetSceneHandle() == project.Scene);
		CHECK(context.GetAssetManager()->FindAssetByPath("Materials/Folder_00/Material_00000.stmat") == project.Materials.front());
	}

	TEST_CASE("Closing a project while its assets are scanned ends the scan, and nothing it waited for runs")
	{
		GatedJobSystem jobs;
		const ManyAssetsProject project = WriteProject("EditorScanClose");
		EditorContext context { InBackground() };
		EditorCommandRegistry commands;
		EditorCommandResult created;
		{
			BackgroundGate gate;
			std::string error;
			REQUIRE_MESSAGE(context.BeginOpenProject(project.ProjectFile, &error), error);
			created = commands.Execute(context, "entity.create", { { "name", "Never" } });
			REQUIRE(created.IsPending());
			context.CloseProject();
			CHECK_FALSE(context.IsWaitingForAssets());
			CHECK_FALSE(context.IsSceneLoading());
		}
		// Without a project the waiting edit runs on the editor's scene; the start scene never opens.
		created = CompleteCommand(context, std::move(created));
		CHECK_FALSE(context.HasProject());
		CHECK(context.GetSceneLoadStatus().State == SceneLoadState::Cancelled);
	}
}

TEST_SUITE("Editor.UI.Scan")
{
	TEST_CASE("While the assets are scanned, the assets pill and the Content Browser say how far the scan is")
	{
		GatedJobSystem jobs;
		const ManyAssetsProject project = WriteProject("EditorScanUI");
		EditorOptions options;
		options.ProjectPath = project.Directory;
		std::optional<HarnessEditor> editor;
		{
			BackgroundGate gate;
			editor.emplace(ImGuiHarness::Specification {}, options, std::string(), false, true);
			editor->Frames(3);
			REQUIRE(editor->Context().IsScanningAssets());
			CHECK(editor->GetPillText("Status.Assets").find("Scanning") != std::string::npos);
			CHECK(UI::ItemProbe::Find("ContentBrowser.Scanning").has_value());
		}
		// The scan and the start scene take more than the usual ten seconds in CI's Windows Debug build.
		REQUIRE(editor->FramesUntil([&]() { return !editor->Context().IsWaitingForAssets(); }, std::chrono::minutes(2)));
		editor->Frames(2);
		CHECK(editor->GetPillText("Status.Assets").find("Scanning") == std::string::npos);
		CHECK_FALSE(UI::ItemProbe::Find("ContentBrowser.Scanning").has_value());
		CHECK(editor->Context().GetSceneHandle() == project.Scene);
	}
}
