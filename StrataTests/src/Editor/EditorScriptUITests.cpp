#include <doctest/doctest.h>

#include "Editor/HarnessEditor.h"
#include "Editor/ScriptToolchain.h"
#include "Editor/ImGuiHarness.h"
#include "EditorLayer.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/ProblemsPanel.h"
#include "Scripting/ScriptTestUtils.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"
#include "UI/ScriptBrowser.h"
#include "UI/ScriptDialogs.h"
#include "UI/Theme.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scripting/ScriptingComponents.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A machine with the script SDK and no C++ toolchain: no compiler, CMake, vswhere or Command Line Tools.
	class MachineWithoutToolchain final : public ScriptToolchainEnvironment
	{
	public:
		bool IsFile(const std::filesystem::path& path) const override
		{
			return path.filename() == "StrataScriptCoreConfig.cmake" && FileSystem::IsRegularFile(path);
		}
		bool IsDirectory(const std::filesystem::path&) const override { return false; }
		std::optional<std::filesystem::path> FindProgram(std::string_view) const override { return std::nullopt; }
		std::optional<std::string> GetEnvVar(std::string_view) const override { return std::nullopt; }
		std::optional<std::string> Run(const std::filesystem::path&, const std::vector<std::string>&) const override { return std::nullopt; }
	};

	// Script builds that run the test executable as CMake (STRATA_TEST_FAKE_CMAKE, see TestMain.cpp).
	EditorOptions WithFakeScriptBuild()
	{
		EditorOptions options;
		ScriptBuildSettings settings = ScriptBuildSettings::GetEngineDefaults();
		settings.CMake = GetTestExecutablePath();
		options.ScriptBuild = settings;
		return options;
	}

	void CreateProject(HarnessEditor& editor, const std::string& name, const char* projectTemplate = "basic3d")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("EditorUIScripts") / name;
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", name }, { "template", projectTemplate } });
		editor.Frames(2);
	}

	std::vector<std::string> WaitForLines(const std::filesystem::path& file)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!FileSystem::IsRegularFile(file) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		const std::optional<std::string> text = FileSystem::ReadText(file);
		REQUIRE_MESSAGE(text, "The external editor did not start: no ", FileSystem::ToUTF8(file));
		std::vector<std::string> lines;
		size_t start = 0;
		for (size_t end = text->find('\n'); end != std::string::npos; end = text->find('\n', start))
		{
			lines.push_back(text->substr(start, end - start));
			start = end + 1;
		}
		return lines;
	}

	// A ScriptBrowser as a panel hosts it: its actions and files in a window of their own over the editor, drawn in every
	// frame of the editor while Shown.
	struct HostedScriptBrowser
	{
		explicit HostedScriptBrowser(HarnessEditor& editor)
			: Editor(editor)
		{
		}

		HarnessEditor& Editor;
		ScriptBrowser Browser;
		std::string Filter;
		bool Shown = true;
		float ActionsWidth = 0.0f; // ScriptBrowser::CalcActionsWidth() in the last frame drawn

		void Frames(int count)
		{
			for (int frame = 0; frame < count; frame++)
			{
				Editor.Host->Time += ImGuiHarness::c_DeltaTime;
				Editor.Layer->OnUpdate(Timestep(ImGuiHarness::c_DeltaTime));
				Editor.Harness.Frame([this]()
				{
					Editor.Layer->OnImGuiRender();
					if (!Shown)
						return;
					EditorPanelContext context { Editor.Context(), Editor.Layer->GetCommands(), Editor.Layer->GetCommandRunner(), {}, Editor.Layer.get() };
					ImGui::SetNextWindowPos(ImVec2(100.0f, 100.0f));
					ImGui::SetNextWindowSize(ImVec2(900.0f, 500.0f));
					ImGui::Begin("Hosted Scripts", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
					ActionsWidth = ScriptBrowser::CalcActionsWidth();
					Browser.DrawActions(context);
					Browser.Draw(context, Filter);
					ImGui::End();
				});
				Editor.Host->FrameCount++;
			}
		}

		// Frames (with a little sleep, for the listing's I/O thread) until the condition holds or ten seconds passed.
		bool FramesUntil(const std::function<bool()>& condition)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!condition())
			{
				if (std::chrono::steady_clock::now() > deadline)
					return false;
				Frames(1);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			return true;
		}

		// Moves the mouse to a point and clicks there twice, well after any click before (a double click).
		void DoubleClick(const ImVec2& position)
		{
			Frames(30);
			Editor.Harness.MoveMouse(position);
			Frames(1);
			for (int click = 0; click < 2; click++)
			{
				Editor.Harness.SetMouseButton(ImGuiMouseButton_Left, true);
				Frames(1);
				Editor.Harness.SetMouseButton(ImGuiMouseButton_Left, false);
				Frames(1);
			}
		}

		void Click(const ImVec2& position)
		{
			Frames(30);
			Editor.Harness.MoveMouse(position);
			Frames(1);
			Editor.Harness.SetMouseButton(ImGuiMouseButton_Left, true);
			Frames(1);
			Editor.Harness.SetMouseButton(ImGuiMouseButton_Left, false);
			Frames(1);
		}
	};

	ImVec2 GetCenter(const ImVec2& min, const ImVec2& max)
	{
		return ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
	}

	// The titles of the toasts that show, top to bottom (their probes, UI/Notifications).
	std::vector<std::string> GetToastTitles()
	{
		std::vector<std::string> titles;
		for (size_t index = 0;; index++)
		{
			const std::optional<UI::ItemProbe::Item> title = UI::ItemProbe::Find(fmt::format("Toast.{}.Title", index));
			if (!title)
				return titles;
			titles.push_back(title->Text);
		}
	}

	// The probe key of the toast with this title ("Toast.<n>"), empty when none shows.
	std::string FindToast(const std::string& title)
	{
		const std::vector<std::string> titles = GetToastTitles();
		const auto found = std::find(titles.begin(), titles.end(), title);
		return found == titles.end() ? std::string() : fmt::format("Toast.{}", found - titles.begin());
	}

}

TEST_SUITE("Editor.UI.Scripts")
{
	TEST_CASE("A new project's example script, which no scene uses, asks for no build")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "Fresh");
		REQUIRE(editor.Context().HasScriptBuild());
		CHECK_FALSE(editor.Context().DoScenesUseScripts());
		CHECK(editor.GetPillText("Status.Scripts") == "Scripts unused");
		CHECK(editor.GetPillColor("Status.Scripts") == UI::ToColorU32(UI::GetThemeColors().TextSecondary));
	}

	TEST_CASE("The Problems panel lists a failed build's diagnostics, and a double click opens one in the external editor")
	{
		ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "fail");
		const std::filesystem::path output = CreateTemporaryDirectory("ProblemsEditor") / "Arguments.txt";
		ScopedEnvironmentVariable externalEditor("STRATA_EXTERNAL_EDITOR", fmt::format("\"{}\" --strata-test-helper=record-arguments \"{}\" {{file}} {{line}} {{column}}",
			FileSystem::ToUTF8(GetTestExecutablePath()), FileSystem::ToUTF8(output)));
		HarnessEditor editor({}, WithFakeScriptBuild());
		editor.Frames(1);
		CreateProject(editor, "Broken");
		const std::filesystem::path broken = editor.Context().GetProject()->GetScriptSourceDirectory() / "Broken.cpp";
		REQUIRE(FileSystem::WriteText(broken, "// The stand-in for CMake reports errors here\n"));

		const ScriptBuilder& builder = editor.Context().GetScriptBuilder();
		REQUIRE(editor.Click("Toolbar.BuildScripts"));
		REQUIRE(editor.FramesUntil([&builder]() { return !builder.IsRunning() && builder.GetLastResult().ID != 0; }));
		editor.Frames(2);
		// Its errors, as the Problems panel counts them (the warning is no error).
		CHECK(editor.GetPillText("Status.Scripts") == "Build failed \xC2\xB7 1 error");

		// The pill leads to the Problems panel, which lists the error and the warning with their places.
		REQUIRE(editor.Click("Status.Scripts"));
		editor.Frames(3);
		ProblemsPanel* problems = editor.Layer->GetPanels().Get<ProblemsPanel>(EditorPanels::c_Problems);
		REQUIRE(problems);
		REQUIRE(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Problems));
		const std::vector<ProblemsPanel::Row>& rows = problems->GetRows();
		REQUIRE(rows.size() == 2);
		CHECK(rows[0].Kind == ProblemsPanel::RowKind::Error);
		CHECK(rows[0].Text.find("'Undeclared': undeclared identifier") != std::string::npos);
		CHECK(rows[0].File == "Scripts/Broken.cpp");
		CHECK(rows[0].ShownFile == "Scripts/Broken.cpp"); // The project's files show relative to it
		CHECK(UI::ItemProbe::Find("Problems.File.0")->Text == "Scripts/Broken.cpp");
		CHECK(rows[0].Line == 7);
		CHECK(rows[0].Column == 5);
		std::error_code error;
		CHECK(std::filesystem::equivalent(rows[0].Source, broken, error));
		CHECK(rows[1].Kind == ProblemsPanel::RowKind::Warning);
		// The warning shows whole, also after the "##" of the token it names (where a label would end).
		CHECK(rows[1].Text.find("'TOKEN##QUIRK'") != std::string::npos);
		CHECK(ImGuiHarness::CountGlyphs(editor.FindPanelWindow(EditorPanels::c_Problems), 'Q') >= 1);
		CHECK(problems->GetErrorCount() == 1);
		CHECK(UI::ItemProbe::Find("Problems.Summary")->Text.find("1 error, 1 warning") != std::string::npos);

		// A double click on the error opens its file at its line and column.
		const std::optional<UI::ItemProbe::Item> row = UI::ItemProbe::Find("Problems.Row.0");
		REQUIRE(row);
		CHECK(row->Enabled);
		editor.Frames(30); // Past the double-click time of the clicks before
		editor.ClickAt(row->GetCenter());
		editor.ClickAt(row->GetCenter());
		const std::vector<std::string> lines = WaitForLines(output);
		REQUIRE(lines.size() == 3);
		CHECK(std::filesystem::equivalent(FileSystem::FromUTF8(lines[0]), broken, error));
		CHECK(lines[1] == "7");
		CHECK(lines[2] == "5");
		CHECK(problems->GetLastOpenError().empty());

		// A file that is gone since says so next to the summary, until the rows are built again (the next build's).
		REQUIRE(FileSystem::Remove(broken));
		editor.Frames(30);
		editor.ClickAt(row->GetCenter());
		editor.ClickAt(row->GetCenter());
		editor.Frames(1);
		CHECK(problems->GetLastOpenError().find("There is no file") != std::string::npos);
		CHECK(UI::ItemProbe::Find("Problems.OpenError")->Text == problems->GetLastOpenError());
		REQUIRE(FileSystem::WriteText(broken, "// Back again\n"));
		const uint64_t built = builder.GetLastResult().ID;
		REQUIRE(editor.Click("Problems.Build"));
		REQUIRE(editor.FramesUntil([&builder, built]() { return !builder.IsRunning() && builder.GetLastResult().ID == built + 1; }));
		editor.Frames(2);
		CHECK(problems->GetLastOpenError().empty());
		CHECK(UI::ItemProbe::Find("Problems.OpenError")->Text.empty());
	}

	TEST_CASE("Cancelling the build Play waits for is no failure: no error, no Problems panel, and the build reads as cancelled")
	{
		ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "succeed");
		HarnessEditor editor({}, WithFakeScriptBuild());
		editor.Frames(1);
		CreateProject(editor, "Cancelling");
		// A scene that uses scripts which are not built: Play builds first.
		editor.Context().GetEditScene()->CreateEntity("Scripted").AddComponent<ScriptComponent>();
		editor.Frames(2);
		const uint64_t requests = editor.Context().GetProblemsPanelRequests();
		LogCapture log;

		// Ctrl+P starts the build in this frame; the builder polls it from the next one, so it is still running here.
		editor.Harness.SetKey(ImGuiMod_Ctrl, true);
		editor.Harness.SetKey(ImGuiKey_P, true);
		editor.Frames(1);
		editor.Harness.SetKey(ImGuiKey_P, false);
		editor.Harness.SetKey(ImGuiMod_Ctrl, false);
		const ScriptBuilder& builder = editor.Context().GetScriptBuilder();
		REQUIRE(builder.IsRunning());
		REQUIRE(editor.Context().GetPlayBuildID() == builder.GetCurrentID());
		editor.Context().CancelScriptBuild(); // What the "Building to play" pill does when clicked
		editor.Frames(3);

		CHECK_FALSE(editor.Context().IsPlaying());
		CHECK(editor.Context().GetProblemsPanelRequests() == requests);
		CHECK(builder.GetLastResult().Cancelled);
		CHECK(editor.GetPillText("Status.Scripts") != "Build failed");
		ProblemsPanel* problems = editor.Layer->GetPanels().Get<ProblemsPanel>(EditorPanels::c_Problems);
		REQUIRE(problems);
		CHECK(problems->GetSummary() == fmt::format("Build {} cancelled.", builder.GetLastResult().ID));
		// Its toast says it was cancelled, in no error's color, and play.start's cancellation posts none of its own.
		const std::vector<std::string> titles = GetToastTitles();
		CHECK(std::count(titles.begin(), titles.end(), "Script build cancelled") == 1);
		CHECK(std::count(titles.begin(), titles.end(), "Script build failed") == 0);
		CHECK(std::count(titles.begin(), titles.end(), "play.start failed") == 0);
		for (const LogEntry& entry : log.GetEntries())
			CHECK_MESSAGE(entry.Level < LogLevel::Error, entry.Message);
	}

	TEST_CASE("The Problems panel shows a crash where it happened, its file cut at the start where the column is narrow")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "Crashing");
		editor.Run("script.load", { { "path", FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS)) } });
		const nlohmann::json created = editor.Run("entity.create", { { "name", "Faulty" } });
		editor.Run("script.add", { { "entity", created["id"] }, { "class", "Faulty" }, { "fields", { { "Fault", "NullDereference" }, { "FaultIn", "OnUpdate" } } } });
		editor.Run("play.start", { { "build", false } });
		REQUIRE(editor.FramesUntil([&editor]()
		{
			const std::optional<ScriptFault>& fault = editor.Context().GetLastScriptFault();
			return fault && fault->Symbolized;
		}));
		editor.Run("editor.showPanel", { { "panel", "Problems" } });
		editor.Frames(3);

		ProblemsPanel* problems = editor.Layer->GetPanels().Get<ProblemsPanel>(EditorPanels::c_Problems);
		REQUIRE(problems);
		const std::vector<ProblemsPanel::Row>& rows = problems->GetRows();
		REQUIRE_FALSE(rows.empty());
		CHECK(rows[0].Kind == ProblemsPanel::RowKind::Crash);
		CHECK(rows[0].Text.find("Crash in Faulty::OnUpdate on 'Faulty'") != std::string::npos);
#if defined(ST_PLATFORM_WINDOWS)
		// The test module's source lies outside the project: its path as the symbolizer named it, cut at the start so that
		// the file's name stays readable.
		CHECK(FileSystem::FromUTF8(rows[0].File).filename() == "FaultScripts.cpp");
		CHECK(rows[0].ShownFile == rows[0].File);
		CHECK(rows[0].Line > 0);
		const std::string shown = UI::ItemProbe::Find("Problems.File.0")->Text;
		CHECK(shown.ends_with("FaultScripts.cpp"));
		CHECK((shown == rows[0].File || shown.starts_with("\xE2\x80\xA6")));

		// The stack runs through the SDK's thunks, which script.status reports and the panel leaves out: the frames shown are
		// the other frames with a source (the script's own callers, as far as the optimizer kept them).
		const ScriptFault& fault = *editor.Context().GetLastScriptFault();
		REQUIRE(fault.Location);
		CHECK(std::any_of(fault.Stack.begin(), fault.Stack.end(), [](const ScriptCodeLocation& frame) { return frame.Function.starts_with("Strata::Detail::"); }));
		CHECK(std::none_of(rows.begin(), rows.end(), [](const ProblemsPanel::Row& row) { return row.Text.starts_with("Strata::Detail::"); }));
		const auto shownFrames = std::count_if(fault.Stack.begin(), fault.Stack.end(), [&fault](const ScriptCodeLocation& frame)
		{
			const bool isLocation = frame.Address == fault.Location->Address && frame.Line == fault.Location->Line && frame.File == fault.Location->File;
			return !frame.File.empty() && !isLocation && !frame.Function.starts_with("Strata::Detail::");
		});
		CHECK(std::count_if(rows.begin(), rows.end(), [](const ProblemsPanel::Row& row) { return row.Kind == ProblemsPanel::RowKind::Frame; }) == shownFrames);

		// The function that crashed is named on the crash row (its frame is not a row of its own) and leads the summary, in
		// the error color.
		REQUIRE_FALSE(fault.Location->Function.empty());
		// The null write is in a function OnUpdate calls (inlined into it in optimized builds, which still name it).
		CHECK(fault.Location->Function != "Faulty::OnUpdate");
		if (fault.Location->Function != "Faulty::OnUpdate")
			CHECK(rows[0].Text.find("(in " + fault.Location->Function + ")") != std::string::npos);
		CHECK(problems->GetSummary() == "The scripts crashed in " + fault.Location->Function + ".");
		const std::optional<UI::ItemProbe::Item> summary = UI::ItemProbe::Find("Problems.Summary");
		REQUIRE(summary);
		CHECK(summary->Color == UI::ToColorU32(UI::GetThemeColors().Error));

		// Stack rows sit under the crash row, their icon indented with them and their text clear of the icon.
		if (rows.size() > 1)
		{
			REQUIRE(rows[1].Kind == ProblemsPanel::RowKind::Frame);
			const std::optional<UI::ItemProbe::Item> crashIcon = UI::ItemProbe::Find("Problems.Icon.0");
			const std::optional<UI::ItemProbe::Item> frameIcon = UI::ItemProbe::Find("Problems.Icon.1");
			const std::optional<UI::ItemProbe::Item> crashText = UI::ItemProbe::Find("Problems.Text.0");
			const std::optional<UI::ItemProbe::Item> frameText = UI::ItemProbe::Find("Problems.Text.1");
			REQUIRE(crashIcon);
			REQUIRE(frameIcon);
			REQUIRE(crashText);
			REQUIRE(frameText);
			CHECK(frameIcon->Min.x > crashIcon->Min.x + ImGui::GetFontSize() * 0.5f);
			const float crashGap = crashText->Min.x - crashIcon->Max.x;
			CHECK(crashGap > 0.0f);
			CHECK(frameText->Min.x - frameIcon->Max.x == doctest::Approx(crashGap));
		}
#endif
	}

	TEST_CASE("A failed script build and a script crash each show one toast, whose Show Problems opens the Problems panel")
	{
		ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "fail");
		HarnessEditor editor({}, WithFakeScriptBuild());
		editor.Frames(1);
		CreateProject(editor, "ToastProblems");
		REQUIRE(FileSystem::WriteText(editor.Context().GetProject()->GetScriptSourceDirectory() / "Broken.cpp", "// The stand-in for CMake reports errors here\n"));
		editor.Context().GetNotifications().Clear();
		editor.Layer->GetPanels().SetOpen(EditorPanels::c_Problems, false);
		editor.Frames(2);

		// The person's Build Scripts fails: the build's toast says so once (the command's failure is the same news), with its
		// first error and the way to all of them.
		const ScriptBuilder& builder = editor.Context().GetScriptBuilder();
		REQUIRE(editor.Click("Toolbar.BuildScripts"));
		REQUIRE(editor.FramesUntil([&builder]() { return !builder.IsRunning() && builder.GetLastResult().ID != 0; }));
		editor.Frames(2);
		std::vector<std::string> titles = GetToastTitles();
		CHECK(std::count(titles.begin(), titles.end(), "Script build failed") == 1);
		CHECK(std::count(titles.begin(), titles.end(), "script.build failed") == 0);
		// The status bar's pill counts the build's errors as the Problems panel does; the error the build logged is its news,
		// not an unread error besides.
		const std::optional<UI::ItemProbe::Item> pill = UI::ItemProbe::Find("Status.Scripts");
		REQUIRE(pill.has_value());
		CHECK(pill->Text.find("Build failed \xC2\xB7 1 error") != std::string::npos);
		CHECK_FALSE(UI::ItemProbe::Find("Status.Errors").has_value());
		std::string toast = FindToast("Script build failed");
		REQUIRE_FALSE(toast.empty());
		CHECK(UI::ItemProbe::Find(toast)->Text.find("Broken.cpp:7: ") != std::string::npos);
		REQUIRE(UI::ItemProbe::Find(toast + ".Console").has_value());
		CHECK_FALSE(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Problems));
		REQUIRE(editor.Click(toast + ".Problems"));
		editor.Frames(2);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Problems));
		ProblemsPanel* problems = editor.Layer->GetPanels().Get<ProblemsPanel>(EditorPanels::c_Problems);
		REQUIRE(problems);
		CHECK(problems->GetErrorCount() == 1);
		CHECK(FindToast("Script build failed").empty());

		// A script crash stops play mode: its toast leads to the panel too, which leads with the crash.
		editor.Layer->GetPanels().SetOpen(EditorPanels::c_Problems, false);
		editor.Run("script.load", { { "path", FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS)) } });
		const nlohmann::json created = editor.Run("entity.create", { { "name", "Faulty" } });
		editor.Run("script.add", { { "entity", created["id"] }, { "class", "Faulty" }, { "fields", { { "Fault", "NullDereference" }, { "FaultIn", "OnUpdate" } } } });
		editor.Context().GetNotifications().Clear();
		editor.Run("play.start", { { "build", false } });
		REQUIRE(editor.FramesUntil([&editor]() { return editor.Context().GetLastScriptFault().has_value() && !editor.Context().IsPlaying(); }));
		editor.Frames(2);
		titles = GetToastTitles();
		CHECK(std::count(titles.begin(), titles.end(), "Script crash: play mode stopped") == 1);
		toast = FindToast("Script crash: play mode stopped");
		REQUIRE_FALSE(toast.empty());
		CHECK(UI::ItemProbe::Find(toast)->Text.find("Faulty::OnUpdate on entity 'Faulty'") != std::string::npos);
		REQUIRE(editor.Click(toast + ".Problems"));
		editor.Frames(3);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Problems));
		REQUIRE_FALSE(problems->GetRows().empty());
		CHECK(problems->GetRows()[0].Kind == ProblemsPanel::RowKind::Crash);
		CHECK(FindToast("Script crash: play mode stopped").empty());
	}

	TEST_CASE("An editor that finds no C++ toolchain says what to install: in its toast, the Problems panel and script.status")
	{
		// An installed editor's toolchain is only preferences: its first build looks for one, here on a machine without any.
		EditorOptions options = WithFakeScriptBuild();
		options.ScriptBuild->FindToolchain = true;
		options.ToolchainEnvironment = []() { return Scope<ScriptToolchainEnvironment>(CreateScope<MachineWithoutToolchain>()); };
		HarnessEditor editor({}, options);
		editor.Frames(1);
		CreateProject(editor, "NoToolchain");
		editor.Context().GetNotifications().Clear();
		editor.Layer->GetPanels().SetOpen(EditorPanels::c_Problems, false);
		editor.Frames(2);
		const nlohmann::json before = editor.Run("script.status");
		CHECK(before["toolchain"]["found"] == false);
		CHECK(before["toolchain"]["searchesThisMachine"] == true);

		const ScriptBuilder& builder = editor.Context().GetScriptBuilder();
		REQUIRE(editor.Click("Toolbar.BuildScripts"));
		REQUIRE(editor.FramesUntil([&builder]() { return !builder.IsRunning() && builder.GetLastResult().ID != 0; }));
		editor.Frames(2);
		const ScriptBuildResult& result = builder.GetLastResult();
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("No C++ toolchain to build the scripts with was found on this machine") != std::string::npos);
		// What to install, for this platform.
		const std::string install = GetScriptToolchainHost() == ScriptToolchainHost::Windows ? "https://visualstudio.microsoft.com/downloads/"
			: GetScriptToolchainHost() == ScriptToolchainHost::MacOS ? "xcode-select --install" : "sudo apt install g++ cmake ninja-build";

		// The toast's message is the diagnostic's (which has no file to name), and leads to the Problems panel.
		const std::string toast = FindToast("Script build failed");
		REQUIRE_FALSE(toast.empty());
		CHECK(UI::ItemProbe::Find(toast)->Text.find("Script build failed\nBuilding scripts needs ") != std::string::npos);
		CHECK(UI::ItemProbe::Find(toast)->Text.find(install) != std::string::npos);
		CHECK(editor.GetPillText("Status.Scripts") == "Build failed \xC2\xB7 1 error");
		REQUIRE(editor.Click(toast + ".Problems"));
		editor.Frames(3);
		ProblemsPanel* problems = editor.Layer->GetPanels().Get<ProblemsPanel>(EditorPanels::c_Problems);
		REQUIRE(problems);
		REQUIRE(problems->GetRows().size() == 1);
		CHECK(problems->GetRows()[0].Kind == ProblemsPanel::RowKind::Error);
		CHECK(problems->GetRows()[0].Text.find(install) != std::string::npos);
		CHECK(problems->GetRows()[0].File.empty());
		CHECK(UI::ItemProbe::Find("Problems.Summary")->Text.find("1 error") != std::string::npos);

		// script.status has the build's error and diagnostic, and still no toolchain.
		const nlohmann::json status = editor.Run("script.status");
		CHECK(status["toolchain"]["found"] == false);
		CHECK(status["build"]["last"]["success"] == false);
		CHECK(status["build"]["last"]["diagnostics"][0]["message"].get<std::string>().find(install) != std::string::npos);
	}

	TEST_CASE("New Script writes a script class, opens it and builds the scripts, so that the class can be attached")
	{
		const std::filesystem::path output = CreateTemporaryDirectory("NewScriptEditor") / "Arguments.txt";
		ScopedEnvironmentVariable externalEditor("STRATA_EXTERNAL_EDITOR", fmt::format("\"{}\" --strata-test-helper=record-arguments \"{}\" {{file}} {{line}}",
			FileSystem::ToUTF8(GetTestExecutablePath()), FileSystem::ToUTF8(output)));
		ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "succeed");
		HarnessEditor editor({}, WithFakeScriptBuild());
		editor.Frames(1);
		CreateProject(editor, "Authoring");
		editor.Layer->GetNewScriptDialog().Open();
		editor.Frames(2);
		REQUIRE(editor.Layer->GetNewScriptDialog().IsOpen());

		// Without a name nothing can be created; the name field has the keyboard as soon as the dialog shows.
		REQUIRE(UI::ItemProbe::Find("NewScript.Create"));
		CHECK_FALSE(UI::ItemProbe::Find("NewScript.Create")->Enabled);
		editor.Harness.TypeText("Spin");
		editor.Frames(2);
		CHECK(UI::ItemProbe::Find("NewScript.Create")->Enabled);

		// A name the project declares already cannot be created.
		REQUIRE(editor.ReplaceText("NewScript.Class", "Spinner"));
		editor.Frames(1);
		REQUIRE(UI::ItemProbe::Find("NewScript.Create"));
		CHECK_FALSE(UI::ItemProbe::Find("NewScript.Create")->Enabled);

		REQUIRE(editor.ReplaceText("NewScript.Class", "DoorOpener"));
		editor.Frames(1);
		REQUIRE(UI::ItemProbe::Find("NewScript.Create")->Enabled);
		// Cancel, then the primary button at the right end, as in the editor's other dialogs.
		REQUIRE(UI::ItemProbe::Find("NewScript.Cancel"));
		CHECK(UI::ItemProbe::Find("NewScript.Cancel")->Max.x < UI::ItemProbe::Find("NewScript.Create")->Min.x);
		const ScriptBuilder& builder = editor.Context().GetScriptBuilder();
		const uint64_t buildsBefore = builder.GetLastResult().ID;
		REQUIRE_FALSE(builder.IsRunning());
		REQUIRE(editor.Click("NewScript.Create"));
		// The scripts build at once (the stand-in for CMake may have finished already): the new class reaches the Inspector's
		// Add Script once the build finished.
		CHECK((builder.IsRunning() ? builder.GetCurrentID() : builder.GetLastResult().ID) == buildsBefore + 1);
		editor.Frames(2);
		CHECK_FALSE(editor.Layer->GetNewScriptDialog().IsOpen());
		const std::filesystem::path file = editor.Context().GetProject()->GetScriptSourceDirectory() / "DoorOpener.cpp";
		CHECK(FileSystem::ReadText(file).value_or("").find("class DoorOpener : public Script") != std::string::npos);
		const std::vector<std::string> lines = WaitForLines(output);
		REQUIRE(lines.size() == 2);
		std::error_code error;
		CHECK(std::filesystem::equivalent(FileSystem::FromUTF8(lines[0]), file, error));
	}

	TEST_CASE("Escape closes the script dialogs, once it has left their fields")
	{
		ScopedEnvironmentVariable noOverride("STRATA_EXTERNAL_EDITOR", "");
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "Escape");
		const auto pressEscape = [&editor]()
		{
			editor.Harness.SetKey(ImGuiKey_Escape, true);
			editor.Frames(1);
			editor.Harness.SetKey(ImGuiKey_Escape, false);
			editor.Frames(2);
		};

		// New Script's name field has the keyboard when it opens: the first Escape leaves it, the second closes the dialog.
		editor.Layer->GetNewScriptDialog().Open();
		editor.Frames(2);
		REQUIRE(editor.Layer->GetNewScriptDialog().IsOpen());
		editor.Harness.TypeText("Door");
		editor.Frames(1);
		pressEscape();
		CHECK(editor.Layer->GetNewScriptDialog().IsOpen());
		pressEscape();
		CHECK_FALSE(editor.Layer->GetNewScriptDialog().IsOpen());
		CHECK_FALSE(FileSystem::Exists(editor.Context().GetProject()->GetScriptSourceDirectory() / "Door.cpp"));

		// External Editor opens with no field active: Escape closes it, saving nothing; Cancel sits left of Save.
		ExternalEditorDialog& dialog = editor.Layer->GetExternalEditorDialog();
		EditorPanelContext context { editor.Context(), editor.Layer->GetCommands(), editor.Layer->GetCommandRunner(), {}, editor.Layer.get() };
		dialog.Open(context);
		editor.Frames(2);
		REQUIRE(dialog.IsOpen());
		REQUIRE(UI::ItemProbe::Find("ExternalEditor.Cancel"));
		REQUIRE(UI::ItemProbe::Find("ExternalEditor.Save"));
		CHECK(UI::ItemProbe::Find("ExternalEditor.Cancel")->Max.x < UI::ItemProbe::Find("ExternalEditor.Save")->Min.x);
		pressEscape();
		CHECK_FALSE(dialog.IsOpen());
		CHECK(editor.Context().GetExternalEditor().GetUserCommand().empty());
	}

	TEST_CASE("The External Editor dialog offers the editors found here and says which program a command starts")
	{
		ScopedEnvironmentVariable noOverride("STRATA_EXTERNAL_EDITOR", "");
		// VS Code's command-line program in PATH (a file named like it is enough to be found).
		const std::filesystem::path tools = CreateTemporaryDirectory("DialogEditors");
#if defined(ST_PLATFORM_WINDOWS)
		const std::filesystem::path code = tools / "code.exe";
#else
		const std::filesystem::path code = tools / "code";
#endif
		REQUIRE(FileSystem::WriteText(code, "Not really a program\n"));
		std::error_code error;
		std::filesystem::permissions(code, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add, error);
		REQUIRE_FALSE(error);
		ScopedEnvironmentVariable path("PATH", FileSystem::ToUTF8(tools));
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "Editors");
		ExternalEditorDialog& dialog = editor.Layer->GetExternalEditorDialog();
		EditorPanelContext context { editor.Context(), editor.Layer->GetCommands(), editor.Layer->GetCommandRunner(), {}, editor.Layer.get() };
		dialog.Open(context);
		editor.Frames(2);
		REQUIRE(dialog.IsOpen());

		// VS Code is offered (other editors found on this machine may be too); a click puts its command in the field, and the
		// dialog names the program it starts.
		const std::vector<ExternalEditorChoice>& choices = dialog.GetChoices();
		const auto vscode = std::find_if(choices.begin(), choices.end(), [](const ExternalEditorChoice& choice) { return choice.Name == "VS Code"; });
		REQUIRE(vscode != choices.end());
		CHECK(vscode->Command == "code -g {file}:{line}:{column}");
		CHECK(UI::ItemProbe::Find("ExternalEditor.Program")->Text == "Files open in the system's default application.");
		REQUIRE(editor.Click(std::string(UI::NumberedProbeKey("ExternalEditor.Choice.", static_cast<size_t>(vscode - choices.begin())))));
		editor.Frames(2);
		const std::optional<UI::ItemProbe::Item> program = UI::ItemProbe::Find("ExternalEditor.Program");
		REQUIRE(program);
		CHECK(program->Enabled);
		CHECK(program->Text.starts_with("Starts "));
		CHECK(std::filesystem::equivalent(FileSystem::FromUTF8(program->Text.substr(7)), code, error));

		// A program that is not there says so before Save, and Save keeps the dialog open with the reason.
		REQUIRE(editor.ReplaceText("ExternalEditor.Command", "noeditor -g {file}"));
		editor.Frames(2);
		CHECK_FALSE(UI::ItemProbe::Find("ExternalEditor.Program")->Enabled);
		CHECK(UI::ItemProbe::Find("ExternalEditor.Program")->Text.starts_with("Cannot start: there is no program 'noeditor'"));
		REQUIRE(editor.Click("ExternalEditor.Save"));
		editor.Frames(2);
		CHECK(dialog.IsOpen());
		CHECK(editor.Context().GetExternalEditor().GetUserCommand().empty());

		// VS Code's command saves.
		REQUIRE(editor.Click(std::string(UI::NumberedProbeKey("ExternalEditor.Choice.", static_cast<size_t>(vscode - choices.begin())))));
		editor.Frames(1);
		REQUIRE(editor.Click("ExternalEditor.Save"));
		editor.Frames(2);
		CHECK_FALSE(dialog.IsOpen());
		CHECK(editor.Context().GetExternalEditor().GetUserCommand() == "code -g {file}:{line}:{column}");
	}

	TEST_CASE("The script browser lists the script sources with their classes, only while it shows, and opens them in the external editor")
	{
		const std::filesystem::path output = CreateTemporaryDirectory("ScriptBrowserEditor") / "Arguments.txt";
		ScopedEnvironmentVariable externalEditor("STRATA_EXTERNAL_EDITOR", fmt::format("\"{}\" --strata-test-helper=record-arguments \"{}\" {{file}} {{line}}",
			FileSystem::ToUTF8(GetTestExecutablePath()), FileSystem::ToUTF8(output)));
		HarnessEditor editor;
		editor.Frames(1);
		// Without a project the editor has no script to write: its shell offers no New Script dialog.
		editor.Layer->ShowNewScriptDialog();
		editor.Frames(1);
		CHECK_FALSE(editor.Layer->GetNewScriptDialog().IsOpen());
		CreateProject(editor, "Browsing");
		const std::filesystem::path directory = editor.Context().GetProject()->GetScriptSourceDirectory();

		// Not drawn: nothing is listed.
		HostedScriptBrowser hosted(editor);
		hosted.Shown = false;
		hosted.Frames(3);
		const ScriptSourceList& list = hosted.Browser.GetList();
		CHECK(list.GetDirectory().empty());
		CHECK_FALSE(list.IsListing());

		// Drawn: the files of the script directory, each with the classes it registers.
		hosted.Shown = true;
		REQUIRE(hosted.FramesUntil([&hosted]() { return hosted.Browser.FindFileRect("Spinner.cpp").has_value(); }));
		CHECK(list.GetDirectory() == directory);
		const std::optional<UI::ItemProbe::Item> first = UI::ItemProbe::Find("ScriptBrowser.Row.0");
		const std::optional<UI::ItemProbe::Item> second = UI::ItemProbe::Find("ScriptBrowser.Row.1");
		REQUIRE(first);
		REQUIRE(second);
		CHECK(first->Text == "CMakeLists.txt");
		CHECK(second->Text == "Spinner.cpp");
		REQUIRE(list.GetListing().Files.size() == 2);
		CHECK(list.GetListing().Files[1].Classes == std::vector<std::string> { "Spinner" });
		CHECK(ImGuiHarness::CountFallbackGlyphs(ImGui::FindWindowByName("Hosted Scripts")) == 0);

		// The actions take the width a toolbar reserves for them.
		const std::optional<UI::ItemProbe::Item> newScript = UI::ItemProbe::Find("ScriptBrowser.NewScript");
		const std::optional<UI::ItemProbe::Item> openInIDE = UI::ItemProbe::Find("ScriptBrowser.OpenInIDE");
		REQUIRE(newScript);
		REQUIRE(openInIDE);
		CHECK(newScript->Enabled);
		CHECK(openInIDE->Enabled);
		CHECK(std::abs(openInIDE->Max.x - newScript->Min.x - hosted.ActionsWidth) < 0.5f);

		// A file written outside the editor (in the code editor) shows within a refresh.
		REQUIRE(FileSystem::CreateDirectories(directory / "Gameplay"));
		REQUIRE(FileSystem::WriteText(directory / "Gameplay" / "Door.cpp", "ST_SCRIPT_CLASS(Door)\n{\n}\n"));
		REQUIRE(hosted.FramesUntil([&hosted]() { return hosted.Browser.FindFileRect("Gameplay/Door.cpp").has_value(); }));
		// A path with "##" shows whole (a label would end there).
		REQUIRE(FileSystem::WriteText(directory / "Gameplay" / "Trick##Quirk.cpp", "// Pasted names\n"));
		REQUIRE(hosted.FramesUntil([&hosted]() { return hosted.Browser.FindFileRect("Gameplay/Trick##Quirk.cpp").has_value(); }));
		CHECK(ImGuiHarness::CountGlyphs(ImGui::FindWindowByName("Hosted Scripts"), 'Q') >= 1);

		// The search finds files by every word of it, in their paths or their classes, ignoring case.
		const auto countRows = [&hosted](const std::string& filter)
		{
			hosted.Filter = filter;
			hosted.Frames(2);
			return hosted.Browser.GetRows().size();
		};
		CHECK(countRows("door") == 1);
		CHECK(hosted.Browser.FindFileRect("Gameplay/Door.cpp").has_value());
		CHECK_FALSE(hosted.Browser.FindFileRect("Spinner.cpp").has_value());
		CHECK(countRows("SPINNER") == 1);
		CHECK(countRows("gameplay  door") == 1);
		CHECK(countRows("gameplay spinner") == 0);
		CHECK(countRows(".cpp") == 3);
		CHECK(countRows("gameplay") == 2);
		CHECK(countRows("") == 4);

		// A double click opens the file in the external editor, at its first line.
		const std::optional<ScriptBrowser::FileRect> spinner = hosted.Browser.FindFileRect("Spinner.cpp");
		REQUIRE(spinner);
		hosted.DoubleClick(GetCenter(spinner->Min, spinner->Max));
		const std::vector<std::string> lines = WaitForLines(output);
		REQUIRE(lines.size() == 2);
		std::error_code error;
		CHECK(std::filesystem::equivalent(FileSystem::FromUTF8(lines[0]), directory / "Spinner.cpp", error));
		CHECK(lines[1] == "1");
		CHECK(hosted.Browser.GetLastOpenError().empty());

		// New Script... asks the editor's shell for its dialog.
		hosted.Click(newScript->GetCenter());
		hosted.Frames(2);
		CHECK(editor.Layer->GetNewScriptDialog().IsOpen());
		const std::optional<UI::ItemProbe::Item> cancel = UI::ItemProbe::Find("NewScript.Cancel");
		REQUIRE(cancel);
		hosted.Click(cancel->GetCenter());
		hosted.Frames(2);
		REQUIRE_FALSE(editor.Layer->GetNewScriptDialog().IsOpen());

		// Not drawn any more (once a listing that ran has arrived): the files are not listed again, whatever changes.
		REQUIRE(hosted.FramesUntil([&list]() { return !list.IsListing(); }));
		hosted.Shown = false;
		const uint64_t version = list.GetVersion();
		REQUIRE(FileSystem::WriteText(directory / "Unseen.cpp", "ST_SCRIPT_CLASS(Unseen)\n"));
		hosted.Frames(150);
		CHECK_FALSE(list.IsListing());
		CHECK(list.GetVersion() == version);

		// Drawn again without a project: the files are forgotten.
		editor.Run("project.close");
		hosted.Shown = true;
		hosted.Frames(2);
		CHECK(list.GetDirectory().empty());
		CHECK_FALSE(list.HasListed());
		CHECK_FALSE(UI::ItemProbe::Find("ScriptBrowser.Row.0"));
		REQUIRE(UI::ItemProbe::Find("ScriptBrowser.NewScript"));
		CHECK_FALSE(UI::ItemProbe::Find("ScriptBrowser.NewScript")->Enabled);
	}

	TEST_CASE("The Content Browser's Scripts root lists the script sources only while it shows, opens them and writes new ones")
	{
		const std::filesystem::path output = CreateTemporaryDirectory("ScriptsRootEditor") / "Arguments.txt";
		ScopedEnvironmentVariable externalEditor("STRATA_EXTERNAL_EDITOR", fmt::format("\"{}\" --strata-test-helper=record-arguments \"{}\" {{file}} {{line}}",
			FileSystem::ToUTF8(GetTestExecutablePath()), FileSystem::ToUTF8(output)));
		// Tall enough that the docked browser shows its tree and a table of files.
		HarnessEditor editor({ ImVec2(1600.0f, 1200.0f), 1.0f });
		editor.Frames(1);
		CreateProject(editor, "Rooted");
		const std::filesystem::path directory = editor.Context().GetProject()->GetScriptSourceDirectory();
		editor.Layer->GetPanels().Focus(EditorPanels::c_ContentBrowser);
		editor.Frames(3);
		ContentBrowserPanel* browser = editor.Layer->GetPanels().Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser);
		REQUIRE(browser);
		const ScriptBrowser& scripts = browser->GetScriptBrowser();
		const ScriptSourceList& list = scripts.GetList();

		// While the assets show, the script sources are not listed.
		editor.Frames(30);
		CHECK_FALSE(browser->IsShowingScripts());
		CHECK(list.GetDirectory().empty());
		CHECK_FALSE(list.IsListing());
		REQUIRE(UI::ItemProbe::Find("ContentBrowser.Filter.Texture"));

		// The Scripts root, below the asset folders: the script directory's files with the classes each registers, in place of
		// the folder's items, with New Script... and Open in IDE where New and Import are, and no type chips.
		REQUIRE(editor.Click("ContentBrowser.ScriptsRoot"));
		CHECK(browser->IsShowingScripts());
		REQUIRE(editor.FramesUntil([&scripts]() { return scripts.FindFileRect("Spinner.cpp").has_value(); }));
		CHECK(list.GetDirectory() == directory);
		REQUIRE(UI::ItemProbe::Find("ScriptBrowser.Row.0"));
		REQUIRE(UI::ItemProbe::Find("ScriptBrowser.Row.1"));
		CHECK(UI::ItemProbe::Find("ScriptBrowser.Row.0")->Text == "CMakeLists.txt");
		CHECK(UI::ItemProbe::Find("ScriptBrowser.Row.1")->Text == "Spinner.cpp");
		REQUIRE(list.GetListing().Files.size() == 2);
		CHECK(list.GetListing().Files[1].Classes == std::vector<std::string> { "Spinner" });
		CHECK(UI::ItemProbe::Find("ContentBrowser.Crumb0")->Text == "Scripts");
		CHECK_FALSE(UI::ItemProbe::Find("ContentBrowser.Filter.Texture"));
		const std::optional<UI::ItemProbe::Item> newScript = UI::ItemProbe::Find("ScriptBrowser.NewScript");
		const std::optional<UI::ItemProbe::Item> openInIDE = UI::ItemProbe::Find("ScriptBrowser.OpenInIDE");
		REQUIRE(newScript);
		REQUIRE(openInIDE);
		CHECK(newScript->Enabled);
		CHECK(openInIDE->Enabled);
		ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_ContentBrowser);
		REQUIRE(window);
		CHECK(openInIDE->Max.x == doctest::Approx(window->WorkRect.Max.x).epsilon(0.002));
		CHECK(ImGuiHarness::CountFallbackGlyphs(window) == 0);

		// Its own search finds files by every word, in their paths or their classes.
		REQUIRE(editor.ReplaceText("ContentBrowser.ScriptSearch", "spinner"));
		editor.Frames(2);
		CHECK(scripts.GetRows().size() == 1);
		CHECK(scripts.FindFileRect("Spinner.cpp").has_value());
		CHECK_FALSE(scripts.FindFileRect("CMakeLists.txt").has_value());
		REQUIRE(editor.Click("ContentBrowser.ScriptSearch.Clear"));
		editor.Frames(2);
		CHECK(scripts.GetRows().size() == 2);

		// A double click opens a file in the external editor, at its first line.
		const std::optional<ScriptBrowser::FileRect> spinner = scripts.FindFileRect("Spinner.cpp");
		REQUIRE(spinner);
		editor.Frames(30); // Past the double-click time of the clicks before
		editor.ClickAt(GetCenter(spinner->Min, spinner->Max));
		editor.ClickAt(GetCenter(spinner->Min, spinner->Max));
		const std::vector<std::string> lines = WaitForLines(output);
		REQUIRE(lines.size() == 2);
		std::error_code error;
		CHECK(std::filesystem::equivalent(FileSystem::FromUTF8(lines[0]), directory / "Spinner.cpp", error));
		CHECK(lines[1] == "1");

		// New Script... asks the editor for its dialog.
		REQUIRE(editor.Click("ScriptBrowser.NewScript"));
		editor.Frames(2);
		CHECK(editor.Layer->GetNewScriptDialog().IsOpen());
		REQUIRE(editor.Click("NewScript.Cancel"));
		editor.Frames(2);
		REQUIRE_FALSE(editor.Layer->GetNewScriptDialog().IsOpen());

		// A folder of the tree goes back to the assets; then nothing is listed again, whatever changes.
		REQUIRE(editor.FramesUntil([&list]() { return !list.IsListing(); }));
		const std::optional<ContentBrowserPanel::ItemRect> root = browser->FindFolderRect("");
		REQUIRE(root);
		editor.ClickAt(GetCenter(root->Min, root->Max));
		CHECK_FALSE(browser->IsShowingScripts());
		CHECK(UI::ItemProbe::Find("ContentBrowser.Filter.Texture"));
		const uint64_t version = list.GetVersion();
		REQUIRE(FileSystem::WriteText(directory / "Unseen.cpp", "ST_SCRIPT_CLASS(Unseen)\n"));
		editor.Frames(150);
		CHECK_FALSE(list.IsListing());
		CHECK(list.GetVersion() == version);

		// Shown again, it lists at once what changed meanwhile; Up and Backspace go back to the assets.
		REQUIRE(editor.Click("ContentBrowser.ScriptsRoot"));
		REQUIRE(editor.FramesUntil([&scripts]() { return scripts.FindFileRect("Unseen.cpp").has_value(); }));
		REQUIRE(editor.Click("ContentBrowser.Up"));
		editor.Frames(1);
		CHECK_FALSE(browser->IsShowingScripts());
		REQUIRE(editor.Click("ContentBrowser.ScriptsRoot"));
		editor.Frames(1);
		editor.Harness.SetKey(ImGuiKey_Backspace, true);
		editor.Frames(1);
		editor.Harness.SetKey(ImGuiKey_Backspace, false);
		editor.Frames(1);
		CHECK_FALSE(browser->IsShowingScripts());

		// An asset inspected elsewhere is revealed among the assets.
		REQUIRE(editor.Click("ContentBrowser.ScriptsRoot"));
		REQUIRE(browser->IsShowingScripts());
		editor.Run("selection.set", { { "asset", "Materials/Ground.stmat" } });
		editor.Frames(3);
		CHECK_FALSE(browser->IsShowingScripts());
		CHECK(browser->GetCurrentDirectory() == "Materials");

		// Agents show the person either view (editor.showPanel); other panels have no Scripts root.
		CHECK(editor.Run("editor.showPanel", { { "panel", "ContentBrowser" }, { "scripts", true } })["scripts"] == true);
		editor.Frames(2);
		CHECK(browser->IsShowingScripts());
		editor.Run("editor.showPanel", { { "panel", "ContentBrowser" } });
		editor.Frames(1);
		CHECK(browser->IsShowingScripts());
		editor.Run("editor.showPanel", { { "panel", "ContentBrowser" }, { "scripts", false } });
		editor.Frames(1);
		CHECK_FALSE(browser->IsShowingScripts());
		const EditorCommandResult refused = editor.Layer->GetCommands().Execute(editor.Context(), "editor.showPanel", { { "panel", "Console" }, { "scripts", true } });
		CHECK(refused.ErrorKind == EditorCommandError::InvalidParameters);

		// The assets' New menu writes scripts too.
		REQUIRE(editor.Click("ContentBrowser.New"));
		editor.Frames(1);
		REQUIRE(editor.Click("ContentBrowser.New.Script"));
		editor.Frames(2);
		CHECK(editor.Layer->GetNewScriptDialog().IsOpen());
		REQUIRE(editor.Click("NewScript.Cancel"));
		editor.Frames(2);

		// Another project opens on its assets.
		REQUIRE(editor.Click("ContentBrowser.ScriptsRoot"));
		REQUIRE(browser->IsShowingScripts());
		CreateProject(editor, "Rerooted");
		CHECK_FALSE(browser->IsShowingScripts());
		CHECK(UI::ItemProbe::Find("ContentBrowser.Filter.Texture"));
	}

	TEST_CASE("editor.showPanel shows the Scripts root also before the Content Browser drew the project, which the next project does not keep")
	{
		// A command script's first step runs before the browser's first frame, whose project is the one the request was for.
		HarnessEditor editor({ ImVec2(1600.0f, 1200.0f), 1.0f }, WithFeatureProject("ScriptsRootFirst"));
		REQUIRE(editor.Context().HasProject());
		editor.Run("editor.showPanel", { { "panel", "ContentBrowser" }, { "scripts", true } });
		editor.Frames(3);
		ContentBrowserPanel* browser = editor.Layer->GetPanels().Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser);
		REQUIRE(browser);
		CHECK(browser->IsShowingScripts());
		CHECK(UI::ItemProbe::Find("ScriptBrowser.NewScript").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("ContentBrowser.Filter.Texture").has_value());

		// Asked for in this project, not in the next one: it opens on its assets.
		CreateProject(editor, "AfterScriptsRoot");
		CHECK_FALSE(browser->IsShowingScripts());
		CHECK(UI::ItemProbe::Find("ContentBrowser.Filter.Texture"));
	}
}
