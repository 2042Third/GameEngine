#include <doctest/doctest.h>

#include "Editor/HarnessEditor.h"
#include "Editor/ImGuiHarness.h"
#include "EditorLayer.h"
#include "Panels/WelcomePanel.h"
#include "TestHelpers.h"
#include "UI/AboutDialog.h"
#include "UI/EditorFonts.h"
#include "UI/EditorPanelRegistry.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/ProjectDialogs.h"
#include "UI/TextFormat.h"
#include "UI/Theme.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Version.h>
#include <Strata/Events/ApplicationEvent.h>
#include <Strata/Project/Project.h>
#include <Strata/Scene/Scene.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	bool IsDrawn(std::string_view probeKey)
	{
		return UI::ItemProbe::Find(probeKey).has_value();
	}

	// A panel for registry tests.
	class CountingPanel : public EditorPanel
	{
	public:
		void OnImGuiRender(EditorPanelContext&) override { Draws++; }
		void OnHidden(EditorPanelContext&) override { Hides++; }

		int Draws = 0;
		int Hides = 0;
	};

	EditorPanelDescriptor MakePanel(std::string id, EditorPanelPlacement placement)
	{
		EditorPanelDescriptor descriptor;
		descriptor.Id = std::move(id);
		descriptor.Title = "Panel " + descriptor.Id;
		descriptor.Icon = Icons::Box;
		descriptor.Placement = placement;
		descriptor.Create = []() { return CreateScope<CountingPanel>(); };
		return descriptor;
	}

	std::string MakeIniLocation(const std::filesystem::path& location)
	{
		return std::string("[") + ProjectDialogs::c_SettingsName + "][Settings]\nProjectLocation=" + FileSystem::ToUTF8(location) + "\n\n";
	}

}

TEST_SUITE("Editor.Launcher")
{
	TEST_CASE("Without a project the editor shows the launcher; with one the dock space")
	{
		HarnessEditor editor;
		editor.Frames(3);
		REQUIRE(editor.Layer->IsLauncherShown());
		for (const std::string_view item : { "Welcome.NewProject", "Welcome.OpenProject", "Welcome.OpenSample", "Welcome.Template.basic3d", "Welcome.Template.empty",
			"Welcome.Sample.Tetris", "Welcome.ContinueWithoutProject", "Welcome.About", "Welcome.AgentStatus" })
		{
			CAPTURE(item);
			CHECK(IsDrawn(item));
		}
		// No toolbar, status bar or panels behind it.
		CHECK_FALSE(IsDrawn("Toolbar.Play"));
		CHECK_FALSE(IsDrawn("Status.PlayState"));
		ImGuiWindow* viewport = editor.FindPanelWindow(EditorPanels::c_Viewport);
		CHECK((!viewport || !viewport->Active));
		CHECK(editor.Run("editor.status")["editor"]["launcher"] == true);

		// A project replaces it with the editor; closing the project brings it back.
		const std::filesystem::path directory = CreateTemporaryDirectory("LauncherProject") / "Game";
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" }, { "template", "basic3d" } });
		editor.Frames(2);
		CHECK_FALSE(editor.Layer->IsLauncherShown());
		CHECK(IsDrawn("Toolbar.Play"));
		CHECK_FALSE(IsDrawn("Welcome.NewProject"));
		CHECK(editor.Run("editor.status")["editor"]["launcher"] == false);
		editor.Run("project.close");
		editor.Frames(2);
		CHECK(editor.Layer->IsLauncherShown());
		CHECK(IsDrawn("Welcome.NewProject"));
	}

	TEST_CASE("New Project creates a lit project from a template card, in a remembered location")
	{
		const std::filesystem::path location = CreateTemporaryDirectory("LauncherNewProject") / "Projects";
		std::string settings;
		{
			HarnessEditor editor;
			editor.Frames(2);
			REQUIRE(editor.Click("Welcome.NewProject"));
			// A dialog that sizes itself to its contents settles over its first frames.
			editor.Frames(3);
			// The lit template comes first and is chosen.
			REQUIRE(IsDrawn("NewProject.Template.basic3d"));
			REQUIRE(IsDrawn("NewProject.Template.empty"));
			CHECK(UI::ItemProbe::Find("NewProject.Template.basic3d")->Min.x < UI::ItemProbe::Find("NewProject.Template.empty")->Min.x);
			CHECK(UI::ItemProbe::Find("NewProject.Create")->Enabled);

			// A name that cannot be a file name keeps Create disabled.
			REQUIRE(editor.ReplaceText("NewProject.Name", "Bad:Name"));
			editor.Frames(1);
			CHECK_FALSE(UI::ItemProbe::Find("NewProject.Create")->Enabled);
			CHECK_FALSE(editor.Click("NewProject.Create"));

			REQUIRE(editor.ReplaceText("NewProject.Name", "Launcher Game"));
			REQUIRE(editor.ReplaceText("ProjectDialogs.Location", FileSystem::ToUTF8(location)));
			editor.Frames(1);
			REQUIRE(UI::ItemProbe::Find("NewProject.Create")->Enabled);
			REQUIRE(editor.Click("NewProject.Create"));
			editor.Frames(2);

			// The project, lit from its template, is open in the editor.
			REQUIRE(editor.Context().HasProject());
			const std::filesystem::path projectFile = location / "Launcher Game" / "Launcher Game.stproj";
			CHECK(editor.Context().GetProject()->GetProjectFile() == projectFile);
			CHECK(FileSystem::IsRegularFile(location / "Launcher Game" / "Assets" / "Scenes" / "Main.stscene"));
			CHECK(editor.Context().GetEditScene()->GetEntityCount() == 5);
			CHECK_FALSE(editor.Layer->IsLauncherShown());
			CHECK_FALSE(editor.Layer->GetProjectDialogs().IsOpen());
			CHECK(editor.Run("editor.recentProjects")["projects"][0]["name"] == "Launcher Game");

			// The location is remembered in the editor's settings.
			CHECK(editor.Layer->GetProjectDialogs().GetLocation() == location);
			size_t size = 0;
			settings = ImGui::SaveIniSettingsToMemory(&size);
			CHECK(settings.find(MakeIniLocation(location)) != std::string::npos);
		}
		{
			// The next session suggests it, with a name that is free there.
			HarnessEditor editor({}, {}, MakeIniLocation(location));
			editor.Frames(2);
			CHECK(editor.Layer->GetProjectDialogs().GetLocation() == location);
		}
		CHECK(ProjectDialogs::GetDefaultLocation().filename() == ProjectDialogs::c_DefaultLocationName);
	}

	TEST_CASE("A template card opens New Project with that template, and Open Sample its sample")
	{
		const std::filesystem::path location = CreateTemporaryDirectory("LauncherTemplateCard");
		HarnessEditor editor;
		editor.Frames(2);
		REQUIRE(editor.Click("Welcome.Template.empty"));
		editor.Frames(3);
		REQUIRE(editor.Layer->GetProjectDialogs().IsOpen());
		REQUIRE(editor.ReplaceText("ProjectDialogs.Location", FileSystem::ToUTF8(location)));
		editor.Frames(1);
		REQUIRE(editor.Click("NewProject.Create"));
		editor.Frames(1);
		REQUIRE(editor.Context().HasProject());
		// The empty template has no start scene.
		CHECK_FALSE(editor.Context().GetProject()->GetConfig().StartScene.IsValid());
		CHECK(editor.Context().GetProject()->GetProjectFile().parent_path() == location / ProjectDialogs::c_DefaultProjectName);

		// The sidebar's Open Sample goes straight to the only sample.
		editor.Run("project.close");
		editor.Frames(2);
		REQUIRE(editor.Click("Welcome.OpenSample"));
		editor.Frames(3);
		CHECK(IsDrawn("OpenSample.Open"));
	}

	TEST_CASE("New Project asks about unsaved changes first, and Cancel creates nothing")
	{
		HarnessEditor editor;
		editor.Layer->DismissLauncher();
		editor.Frames(2);
		editor.Run("entity.create", { { "name", "Unsaved" } });
		REQUIRE(editor.Context().IsSceneModified());

		// File > New Project.
		editor.Layer->ShowNewProjectDialog({});
		editor.Frames(2);
		CHECK_FALSE(editor.Layer->GetProjectDialogs().IsOpen());
		REQUIRE(IsDrawn("UnsavedChanges.DontSave"));
		REQUIRE(editor.Click("UnsavedChanges.DontSave"));
		// A dialog that sizes itself to its contents settles over its first frames.
		editor.Frames(3);
		CHECK(editor.Layer->GetProjectDialogs().IsOpen());
		REQUIRE(IsDrawn("NewProject.Cancel"));

		REQUIRE(editor.Click("NewProject.Cancel"));
		editor.Frames(1);
		CHECK_FALSE(editor.Layer->GetProjectDialogs().IsOpen());
		CHECK_FALSE(editor.Context().HasProject());
		CHECK(editor.Context().GetEditScene()->FindEntityByName("Unsaved"));
	}

	TEST_CASE("Open Sample copies the sample into the chosen location and opens the copy")
	{
		const std::filesystem::path location = CreateTemporaryDirectory("LauncherSample");
		HarnessEditor editor;
		editor.Frames(2);
		REQUIRE(editor.Click("Welcome.Sample.Tetris"));
		editor.Frames(3);
		REQUIRE(IsDrawn("OpenSample.Open"));
		REQUIRE(editor.ReplaceText("ProjectDialogs.Location", FileSystem::ToUTF8(location)));
		editor.Frames(1);
		REQUIRE(editor.Click("OpenSample.Open"));
		editor.Frames(2);

		REQUIRE(editor.Context().HasProject());
		CHECK(editor.Context().GetProject()->GetProjectFile() == location / "Tetris" / "Tetris.stproj");
		CHECK(FileSystem::IsRegularFile(location / "Tetris" / "Scripts" / "TetrisBoard.cpp"));
		CHECK_FALSE(editor.Layer->IsLauncherShown());

		// Again: the copy goes next to the first one.
		editor.Run("project.close");
		editor.Frames(2);
		REQUIRE(editor.Click("Welcome.Sample.Tetris"));
		editor.Frames(3);
		REQUIRE(editor.Click("OpenSample.Open"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		CHECK(editor.Context().GetProject()->GetProjectFile() == location / "Tetris 2" / "Tetris.stproj");
	}

	TEST_CASE("Recent projects are cards that open their project and can leave the list")
	{
		const std::filesystem::path root = CreateTemporaryDirectory("LauncherRecent");
		HarnessEditor editor;
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(root / "First") }, { "name", "First" } });
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(root / "Second") }, { "name", "Second" } });
		editor.Run("project.close");
		editor.Frames(2);

		// Most recent first.
		const WelcomePanel* welcome = editor.Layer->GetPanels().Get<WelcomePanel>(EditorPanels::c_Welcome);
		REQUIRE(welcome);
		REQUIRE(welcome->GetRecentProjects().size() == 2);
		CHECK(welcome->GetRecentProjects()[0].Name == "Second");
		CHECK(IsDrawn("Welcome.Recent.0"));
		CHECK(IsDrawn("Welcome.Recent.1"));
		CHECK_FALSE(IsDrawn("Welcome.Recent.2"));

		REQUIRE(editor.Click("Welcome.Recent.1"));
		editor.Frames(1);
		REQUIRE(editor.Context().HasProject());
		CHECK(editor.Context().GetProject()->GetConfig().Name == "First");

		// A project whose files are gone drops out; one that cannot be opened says why on the launcher.
		editor.Run("project.close");
		editor.Frames(2);
		CHECK(welcome->GetRecentProjects()[0].Name == "First");
		REQUIRE(FileSystem::Remove(root / "Second"));
		editor.Frames(static_cast<int>(WelcomePanel::c_RefreshSeconds / ImGuiHarness::c_DeltaTime) + 2);
		REQUIRE(welcome->GetRecentProjects().size() == 1);
		CHECK_FALSE(IsDrawn("Welcome.Recent.1"));

		editor.Run("editor.removeRecentProject", { { "path", FileSystem::ToUTF8(root / "First" / "First.stproj") } });
		editor.Frames(static_cast<int>(WelcomePanel::c_RefreshSeconds / ImGuiHarness::c_DeltaTime) + 2);
		CHECK(welcome->GetRecentProjects().empty());
		CHECK_FALSE(IsDrawn("Welcome.Recent.0"));
	}

	TEST_CASE("The launcher reads the recent projects on an I/O thread and keeps those out of reach on the list")
	{
		// Real I/O threads (the harness otherwise runs jobs inline): the frames never wait for the read.
		struct ScopedJobs
		{
			ScopedJobs()
			{
				JobSystemSpecification specification;
				specification.WorkerThreadCount = 1;
				specification.IOThreadCount = 1;
				JobSystem::Init(specification);
			}
			~ScopedJobs() { JobSystem::Shutdown(); }
		} jobs;

		const std::filesystem::path root = CreateTemporaryDirectory("LauncherRecentRead");
		const std::filesystem::path present = root / "Present" / "Present.stproj";
		REQUIRE(FileSystem::CreateDirectories(present.parent_path()));
		REQUIRE(FileSystem::WriteText(present, "{}"));
		// Deleted: its folder is gone, the folder that held it is there.
		const std::filesystem::path deleted = root / "Deleted" / "Deleted.stproj";
		// Out of reach: the folder that held its folder is not there either (a share or a drive that is not connected).
		const std::filesystem::path offline = root / "Offline" / "Game" / "Game.stproj";
		const std::filesystem::path file = root / "RecentProjects.json";
		REQUIRE(FileSystem::WriteText(file, RecentProjects::ToJson({ { "Present", present, 3, "1.0" }, { "Deleted", deleted, 2, "1.0" },
			{ "Game", offline, 1, "1.0" } }).dump()));

		EditorOptions options;
		options.RecentProjectsFile = file;
		HarnessEditor editor({}, options);
		editor.Frames(1);
		const WelcomePanel* welcome = editor.Layer->GetPanels().Get<WelcomePanel>(EditorPanels::c_Welcome);
		REQUIRE(welcome);
		REQUIRE(editor.FramesUntil([welcome]() { return !welcome->IsRefreshing() && welcome->GetRecentProjects().size() == 1; }));
		CHECK(welcome->GetRecentProjects()[0].Name == "Present");
		CHECK(IsDrawn("Welcome.Recent.0"));
		CHECK_FALSE(IsDrawn("Welcome.Recent.1"));

		// The deleted project left the file; the one out of reach stays, hidden until it is back.
		const std::optional<std::vector<RecentProject>> stored = RecentProjects::ReadFile(file).Projects;
		REQUIRE(stored);
		REQUIRE(stored->size() == 2);
		CHECK((*stored)[0].Name == "Present");
		CHECK((*stored)[1].Name == "Game");
		REQUIRE(FileSystem::CreateDirectories(offline.parent_path()));
		REQUIRE(FileSystem::WriteText(offline, "{}"));
		editor.Frames(static_cast<int>(WelcomePanel::c_RefreshSeconds / ImGuiHarness::c_DeltaTime) + 2);
		REQUIRE(editor.FramesUntil([welcome]() { return !welcome->IsRefreshing() && welcome->GetRecentProjects().size() == 2; }));
		CHECK(welcome->GetRecentProjects()[1].Name == "Game");
	}

	TEST_CASE("A project that cannot be opened is reported on the launcher")
	{
		EditorOptions options;
		options.ProjectPath = CreateTemporaryDirectory("LauncherMissing") / "Missing";
		HarnessEditor editor({}, options);
		editor.Frames(2);
		REQUIRE(editor.Layer->IsLauncherShown());
		const WelcomePanel* welcome = editor.Layer->GetPanels().Get<WelcomePanel>(EditorPanels::c_Welcome);
		REQUIRE(welcome);
		CHECK(welcome->GetError().find("Could not open the project") != std::string::npos);
		REQUIRE(editor.Click("Welcome.DismissError"));
		CHECK(welcome->GetError().empty());
	}

	TEST_CASE("A project file dropped on the launcher opens its project")
	{
		const std::filesystem::path root = CreateTemporaryDirectory("LauncherDrop");
		HarnessEditor editor;
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(root / "Dropped") }, { "name", "Dropped" } });
		editor.Run("project.close");
		editor.Frames(2);

		// Other files cannot be imported without a project: the launcher says so.
		REQUIRE(FileSystem::WriteText(root / "Notes.txt", "text"));
		WindowFileDropEvent notes({ root / "Notes.txt" });
		editor.Harness.DispatchEvent(notes, *editor.Layer);
		const WelcomePanel* welcome = editor.Layer->GetPanels().Get<WelcomePanel>(EditorPanels::c_Welcome);
		REQUIRE(welcome);
		CHECK(welcome->GetError().find(".stproj") != std::string::npos);
		CHECK_FALSE(editor.Context().HasProject());

		WindowFileDropEvent project({ root / "Notes.txt", root / "Dropped" / "Dropped.stproj" });
		editor.Harness.DispatchEvent(project, *editor.Layer);
		REQUIRE(editor.Context().HasProject());
		CHECK(editor.Context().GetProject()->GetConfig().Name == "Dropped");
	}

	TEST_CASE("Continue without a project shows the editor until a project opens and closes")
	{
		HarnessEditor editor;
		editor.Frames(2);
		REQUIRE(editor.Click("Welcome.ContinueWithoutProject"));
		editor.Frames(1);
		CHECK_FALSE(editor.Layer->IsLauncherShown());
		CHECK(IsDrawn("Toolbar.Play"));
		CHECK(IsDrawn("Status.PlayState"));

		const std::filesystem::path directory = CreateTemporaryDirectory("LauncherContinue") / "Game";
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		editor.Frames(1);
		editor.Run("project.close");
		editor.Frames(1);
		CHECK(editor.Layer->IsLauncherShown());
	}

	TEST_CASE("File > Show Launcher and File > Open Recent lead back from Continue without a project")
	{
		const std::filesystem::path root = CreateTemporaryDirectory("LauncherMenu");
		HarnessEditor editor;
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(root / "First") }, { "name", "First" } });
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(root / "Second") }, { "name", "Second" } });
		editor.Run("project.close");
		editor.Frames(2);
		REQUIRE(editor.Click("Welcome.ContinueWithoutProject"));
		editor.Frames(1);
		REQUIRE_FALSE(editor.Layer->IsLauncherShown());

		// Without a project the launcher is one menu item away.
		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		REQUIRE(editor.Click("Menu.File.ShowLauncher"));
		editor.Frames(1);
		CHECK(editor.Layer->IsLauncherShown());
		CHECK(IsDrawn("Welcome.Recent.0"));

		// The recent projects are in the File menu, most recent first.
		REQUIRE(editor.Click("Welcome.ContinueWithoutProject"));
		editor.Frames(1);
		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		REQUIRE(editor.Click("Menu.File.OpenRecent"));
		editor.Frames(1);
		REQUIRE(IsDrawn("Menu.File.Recent.0"));
		REQUIRE(IsDrawn("Menu.File.Recent.1"));
		CHECK_FALSE(IsDrawn("Menu.File.Recent.2"));
		REQUIRE(editor.Click("Menu.File.Recent.1"));
		editor.Frames(1);
		REQUIRE(editor.Context().HasProject());
		CHECK(editor.Context().GetProject()->GetConfig().Name == "First");

		// With a project open, Close Project leads to the launcher; the open project is not offered again.
		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		const std::optional<UI::ItemProbe::Item> showLauncher = UI::ItemProbe::Find("Menu.File.ShowLauncher");
		REQUIRE(showLauncher);
		CHECK_FALSE(showLauncher->Enabled);
		REQUIRE(editor.Click("Menu.File.OpenRecent"));
		editor.Frames(1);
		REQUIRE(IsDrawn("Menu.File.Recent.0"));
		CHECK_FALSE(IsDrawn("Menu.File.Recent.1"));
	}

	TEST_CASE("The agent card copies the exact command line that connects Claude Code")
	{
		CHECK(WelcomePanel::GetAgentCommandLine(Platform::GetExecutableDirectory() / "StrataCLI") ==
			"claude mcp add strata -- \"" + UI::DisplayPath(Platform::GetExecutableDirectory() / "StrataCLI") + "\" mcp");

		// The agent card sizes itself to its contents, which settles over the first frames.
		HarnessEditor editor;
		editor.Frames(4);
		const EditorEnvironment environment = editor.Layer->GetEnvironment();
#if defined(STRATA_TESTS_HAVE_CLI)
		// Built next to the editor (and the tests).
		REQUIRE_FALSE(environment.CLIExecutable.empty());
		CHECK(FileSystem::IsRegularFile(environment.CLIExecutable));
		REQUIRE(editor.Click("Welcome.AgentCommand.Copy"));
		CHECK(editor.Harness.GetClipboard() == WelcomePanel::GetAgentCommandLine(environment.CLIExecutable));
		CHECK(editor.Harness.GetClipboard().find("claude mcp add strata -- \"") == 0);
		CHECK(editor.Harness.GetClipboard().find("StrataCLI") != std::string::npos);
#else
		CHECK(environment.CLIExecutable.empty());
		CHECK_FALSE(IsDrawn("Welcome.AgentCommand.Copy"));
#endif
		// Automation is off in the harness: the card says so instead of a port.
		CHECK_FALSE(editor.Layer->GetAutomationState().Running);
		CHECK(editor.GetPillColor("Welcome.AgentStatus") == UI::ToColorU32(UI::GetThemeColors().Warning));
	}

	TEST_CASE("The agent card says why automation could not start")
	{
		// Asked for, but its session file cannot be written (the session directory would be inside a file).
		const std::filesystem::path blocker = CreateTemporaryDirectory("LauncherAutomationFailure") / "File";
		REQUIRE(FileSystem::WriteText(blocker, "not a directory"));
		ScopedEnvironmentVariable sessions("STRATA_SESSION_DIR", FileSystem::ToUTF8(blocker / "Sessions"));
		HarnessEditor editor({}, {}, {}, true);
		editor.Frames(2);
		const EditorAutomationState automation = editor.Layer->GetAutomationState();
		CHECK(automation.Enabled);
		CHECK_FALSE(automation.Running);
		CHECK_FALSE(automation.Error.empty());
		CHECK(editor.GetPillColor("Welcome.AgentStatus") == UI::ToColorU32(UI::GetThemeColors().Error));
	}

	TEST_CASE("About Strata shows the build, the GPU, the startup time and the notices")
	{
		HarnessEditor editor;
		GraphicsDeviceInfo device;
		device.AdapterName = "Test GPU";
		device.DriverVersion = "1.2.3";
		device.APIVersion = "1.3.280";
		editor.Host->GraphicsDevice = device;
		editor.Host->ProcessUptime = 0.41;
		editor.Frames(2);

		// The startup time is the process's age once the first frame was on screen.
		const nlohmann::json status = editor.Run("editor.status")["editor"];
		CHECK(status["startupSeconds"].get<double>() == doctest::Approx(0.41));
		CHECK(status["gpu"]["name"] == "Test GPU");
		CHECK(status["gpu"]["driver"] == "1.2.3");

		REQUIRE(editor.Click("Welcome.About"));
		editor.Frames(3);
		CHECK(editor.Layer->GetAboutDialog().IsOpen());
		REQUIRE(editor.Click("About.CopyDetails"));
		const std::string& details = editor.Harness.GetClipboard();
		CHECK(details.find(std::string("Version: ") + c_EngineVersion) != std::string::npos);
		CHECK(details.find(std::string("Commit: ") + c_EngineCommit) != std::string::npos);
		CHECK(details.find("GPU: Test GPU") != std::string::npos);
		CHECK(details.find("Driver: 1.2.3") != std::string::npos);
		CHECK(details.find("Graphics API: Vulkan 1.3.280") != std::string::npos);
		CHECK(details.find("Startup: 0.41 s to the first frame") != std::string::npos);
		REQUIRE(editor.Click("About.Close"));
		editor.Frames(1);
		CHECK_FALSE(editor.Layer->GetAboutDialog().IsOpen());

		// The notices are the repository's, compiled in.
		const std::optional<std::string> notices = FileSystem::ReadText(FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "ThirdPartyNotices.md");
		REQUIRE(notices.has_value());
		CHECK(AboutDialog::GetThirdPartyNotices() == *notices);

		// Without a GPU the details say so.
		EditorEnvironment headless;
		const std::string text = AboutDialog::FormatDetails(headless);
		CHECK(text.find("GPU: None") != std::string::npos);
		CHECK(text.find("Startup: Not measured") != std::string::npos);
	}

	TEST_CASE("The viewport's chips switch preview lighting and the game's UI")
	{
		HarnessEditor editor({}, WithFeatureProject("LauncherChips"));
		editor.Frames(3);
		const ViewportSettings& settings = editor.Context().GetViewport().GetSettings();
		REQUIRE(settings.PreviewLighting);
		REQUIRE_FALSE(settings.ShowGameUI);
		REQUIRE(editor.Click("Viewport.PreviewLighting"));
		CHECK_FALSE(settings.PreviewLighting);
		REQUIRE(editor.Click("Viewport.GameUI"));
		CHECK(settings.ShowGameUI);
		// They are the settings viewport.setSettings changes.
		const nlohmann::json described = editor.Run("viewport.getSettings");
		CHECK(described["previewLighting"] == false);
		CHECK(described["gameUI"] == true);
	}

	TEST_CASE("The preview lighting chip shows whether the scene needs it")
	{
		// A new project without a template: an empty scene, nothing lights it.
		HarnessEditor editor;
		const std::filesystem::path directory = CreateTemporaryDirectory("LauncherPreviewChip") / "Game";
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		editor.Frames(2);
		const UI::ThemeColors& colors = UI::GetThemeColors();
		REQUIRE(editor.Context().GetViewport().GetSettings().PreviewLighting);
		const std::optional<UI::ItemProbe::Item> lit = UI::ItemProbe::Find("Viewport.PreviewLighting");
		REQUIRE(lit);
		CHECK(lit->Color == UI::ToColorU32(colors.Accent));

		// A torch is the scene's own light: preview lighting stays on, with nothing to do, and the chip says so.
		editor.Run("entity.create", { { "name", "Torch" }, { "components", { { "PointLight", nlohmann::json::object() } } } });
		editor.Frames(1);
		const std::optional<UI::ItemProbe::Item> waiting = UI::ItemProbe::Find("Viewport.PreviewLighting");
		REQUIRE(waiting);
		CHECK(waiting->Color == UI::ToColorU32(colors.Accent, 0.55f));
		CHECK(editor.Context().GetViewport().GetSettings().PreviewLighting);

		// Off is off, whatever the scene.
		REQUIRE(editor.Click("Viewport.PreviewLighting"));
		editor.Frames(1);
		const std::optional<UI::ItemProbe::Item> off = UI::ItemProbe::Find("Viewport.PreviewLighting");
		REQUIRE(off);
		CHECK(off->Color == UI::ToColorU32(colors.TextSecondary));
	}

	TEST_CASE("The launcher's sections share one grid and one right edge, centered in the main area")
	{
		const std::filesystem::path root = CreateTemporaryDirectory("LauncherGrid");
		struct Display
		{
			ImVec2 Size;
			float Scale;
		};
		for (const Display display : { Display { ImVec2(1280.0f, 720.0f), 1.0f }, Display { ImVec2(1600.0f, 900.0f), 1.0f },
			Display { ImVec2(2400.0f, 1300.0f), 1.5f }, Display { ImVec2(3840.0f, 2054.0f), 1.5f } })
		{
			for (const size_t recentCount : { size_t(0), size_t(3), size_t(4), size_t(12) })
			{
				CAPTURE(display.Size.x);
				CAPTURE(display.Scale);
				CAPTURE(recentCount);
				HarnessEditor editor({ display.Size, display.Scale });
				for (size_t index = 0; index < recentCount; index++)
				{
					const std::string name = fmt::format("Project {}", index);
					const std::filesystem::path file = root / name / (name + ".stproj");
					REQUIRE(FileSystem::CreateDirectories(file.parent_path()));
					REQUIRE(FileSystem::WriteText(file, "{}"));
					editor.Context().GetRecentProjects().Add(name, file);
				}
				editor.Frames(4);
				REQUIRE(editor.Layer->IsLauncherShown());

				// The right edge of every section: the recent cards (or the note in their place), the starter cards, the agent card.
				const auto rightmost = [](const std::vector<std::string>& keys)
				{
					float right = 0.0f;
					for (const std::string& key : keys)
					{
						if (const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(key))
							right = std::max(right, item->Max.x);
					}
					return right;
				};
				std::vector<std::string> recentKeys = { "Welcome.EmptyState" };
				for (size_t index = 0; index < recentCount; index++)
					recentKeys.push_back(fmt::format("Welcome.Recent.{}", index));
				const std::optional<UI::ItemProbe::Item> agent = UI::ItemProbe::Find("Welcome.AgentCard");
				REQUIRE(agent);
				const float recentRight = rightmost(recentKeys);
				const float startersRight = rightmost({ "Welcome.Template.basic3d", "Welcome.Template.empty", "Welcome.Sample.Tetris" });
				CHECK(std::abs(recentRight - agent->Max.x) <= 1.0f);
				CHECK(std::abs(startersRight - agent->Max.x) <= 1.0f);
				const std::optional<UI::ItemProbe::Item> firstStarter = UI::ItemProbe::Find("Welcome.Template.basic3d");
				REQUIRE(firstStarter);
				CHECK(std::abs(firstStarter->Min.x - agent->Min.x) <= 1.0f);

				// The content column sits in the middle of the main area (next to the sidebar).
				ImGuiWindow* main = nullptr;
				for (ImGuiWindow* window : GImGui->Windows)
				{
					// Child windows are named after their parents: the main area's own name ends with "/Main_<id>".
					const std::string_view name(window->Name);
					const size_t slash = name.rfind('/');
					if (window->Active && slash != std::string_view::npos && name.substr(slash + 1).starts_with("Main_"))
						main = window;
				}
				REQUIRE(main);
				const float leftMargin = agent->Min.x - main->Pos.x;
				const float rightMargin = main->Pos.x + main->Size.x - agent->Max.x;
				CHECK(leftMargin > 0.0f);
				// A short window scrolls the content: its scrollbar takes from the right margin.
				CHECK(std::abs(leftMargin - rightMargin) <= ImGui::GetStyle().ScrollbarSize + 2.0f);
			}
		}
	}

	TEST_CASE("Project names in Chinese, Japanese and Korean show on the cards in the system's fonts")
	{
		// "Jeu été 游戏" and the Korean "게임": Inter has the accents, not the Han characters or Hangul.
		const std::string name = "Jeu \xC3\xA9t\xC3\xA9 \xE6\xB8\xB8\xE6\x88\x8F \xEA\xB2\x8C\xEC\x9E\x84";
		const std::filesystem::path file = CreateTemporaryDirectory("LauncherScripts") / FileSystem::FromUTF8(name) / FileSystem::FromUTF8(name + ".stproj");
		REQUIRE(FileSystem::CreateDirectories(file.parent_path()));
		REQUIRE(FileSystem::WriteText(file, "{}"));
		HarnessEditor editor;
		editor.Context().GetRecentProjects().Add(name, file);
		editor.Frames(2);
		REQUIRE(IsDrawn("Welcome.Recent.0"));
		ImFont* regular = UI::EditorFonts::Get(UI::EditorFont::Regular);
		REQUIRE(regular);
		CHECK(regular->IsGlyphInFont(0x00E9));
		CHECK_FALSE(regular->IsGlyphInFont(0x6E38));
		CHECK_FALSE(UI::EditorFonts::HasFallback());
		const auto measureLatin = [&editor]()
		{
			float width = 0.0f;
			editor.Harness.Frame([&width]() { width = ImGui::CalcTextSize("Strata 0.1").x; });
			return width;
		};
		const float latinWidth = measureLatin();

		// The editor asks for the fallback fonts once its own are loaded; they merge between frames once read.
		UI::EditorFonts::BeginLoadingFallback();
		REQUIRE(editor.FramesUntil([]() { return !UI::EditorFonts::IsLoadingFallback(); }));
		editor.Frames(2);
		const std::vector<std::filesystem::path> files = Platform::FindFallbackFontFiles();
		if (files.empty())
		{
			// Nothing to merge on this system: such text keeps ImGui's fallback character, and nothing else changes.
			CHECK_FALSE(UI::EditorFonts::HasFallback());
			MESSAGE("This system has no fonts for Chinese, Japanese and Korean text");
			return;
		}
		REQUIRE(UI::EditorFonts::HasFallback());
		// Every character of the card's title comes from a font, in each of the editor's fonts (Hangul when the system has
		// a font of its own for it: the second one).
		const bool hangul = files.size() > 1;
		for (const UI::EditorFont font : { UI::EditorFont::Regular, UI::EditorFont::SemiBold, UI::EditorFont::Mono })
		{
			ImFont* merged = UI::EditorFonts::Get(font);
			REQUIRE(merged);
			const char* text = name.c_str();
			const char* end = text + name.size();
			while (text < end)
			{
				unsigned int codepoint = 0;
				text += ImTextCharFromUtf8(&codepoint, text, end);
				CAPTURE(codepoint);
				if (codepoint >= 0xAC00 && codepoint <= 0xD7A3 && !hangul)
					continue;
				CHECK(merged->IsGlyphInFont(static_cast<ImWchar>(codepoint)));
			}
		}
		// Latin text still comes from the editor's own fonts.
		CHECK(measureLatin() == doctest::Approx(latinWidth));
		editor.Frames(1);
		CHECK(IsDrawn("Welcome.Recent.0"));
	}

	TEST_CASE("The launcher has no id conflicts, at 100% and at 150%")
	{
		for (const float scale : { 1.0f, 1.5f })
		{
			CAPTURE(scale);
			const std::filesystem::path root = CreateTemporaryDirectory("LauncherIds");
			HarnessEditor editor({ ImVec2(1600.0f, 900.0f), scale });
			editor.Run("project.create", { { "directory", FileSystem::ToUTF8(root / "Game") }, { "name", "Game" } });
			editor.Run("project.close");
			editor.Frames(2);
			REQUIRE(IsDrawn("Welcome.Recent.0"));

			const float step = ImGui::GetFrameHeight() * 0.5f;
			const ImVec2 display = ImGui::GetIO().DisplaySize;
			int conflicts = 0;
			for (float y = step * 0.5f; y < display.y && conflicts == 0; y += step)
			{
				for (float x = step * 0.5f; x < display.x && conflicts == 0; x += step * 2.0f)
				{
					editor.Harness.MoveMouse(ImVec2(x, y));
					editor.Frames(1);
					if (editor.Harness.GetHoveredItemIdCount() > 1)
					{
						conflicts++;
						FAIL_CHECK("Items with the same id (" << editor.Harness.GetPreviouslyHoveredId() << ") near " << x << ", " << y);
					}
				}
			}
			CHECK(conflicts == 0);
			// The sidebar's and the footer's items stay inside the window (the content scrolls when it is taller).
			for (const std::string_view item : { "Welcome.NewProject", "Welcome.About", "Welcome.ContinueWithoutProject", "Welcome.Recent.0" })
			{
				CAPTURE(item);
				const std::optional<UI::ItemProbe::Item> found = UI::ItemProbe::Find(item);
				REQUIRE(found.has_value());
				CHECK(found->Max.x <= display.x + 0.5f);
				CHECK(found->Max.y <= display.y + 0.5f);
			}
		}
	}

	TEST_CASE("A launcher panel draws instead of the docked ones, and has no menu item or saved state")
	{
		ImGuiHarness harness;
		EditorPanelRegistry registry;
		std::string error;
		REQUIRE(registry.Register(MakePanel("Docked", EditorPanelPlacement::Docked), &error));
		CHECK_FALSE(registry.HasLauncher());
		REQUIRE(registry.Register(MakePanel("Launcher", EditorPanelPlacement::Launcher), &error));
		CHECK(registry.HasLauncher());
		// One launcher at most.
		CHECK_FALSE(registry.Register(MakePanel("Other", EditorPanelPlacement::Launcher), &error));
		CHECK(error.find("launcher") != std::string::npos);

		CountingPanel* docked = registry.Get<CountingPanel>("Docked");
		CountingPanel* launcher = registry.Get<CountingPanel>("Launcher");
		REQUIRE(docked);
		REQUIRE(launcher);
		EditorContext context(EditorContextSpecification { false, false });
		EditorCommandRegistry commands;
		EditorCommandRunner runner;
		EditorPanelContext panelContext { context, commands, runner };

		harness.Frame([&]() { registry.OnImGuiRender(panelContext); });
		CHECK(docked->Draws == 1);
		CHECK(launcher->Draws == 0);
		CHECK(launcher->Hides == 1);
		harness.Frame([&]()
		{
			ImGui::Begin("Host");
			registry.DrawLauncher(panelContext);
			ImGui::End();
		});
		CHECK(launcher->Draws == 1);
		CHECK(docked->Draws == 1);
		CHECK(docked->Hides == 1);

		registry.InstallSettingsHandler();
		size_t size = 0;
		const std::string settings = ImGui::SaveIniSettingsToMemory(&size);
		CHECK(settings.find("Docked=1") != std::string::npos);
		CHECK(settings.find("Launcher=") == std::string::npos);
		registry.RemoveSettingsHandler();
	}
}
