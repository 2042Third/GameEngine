#include <doctest/doctest.h>

#include "Editor/HarnessEditor.h"
#include "Editor/ImGuiHarness.h"
#include "EditorLayer.h"
#include "Panels/ProblemsPanel.h"
#include "Scripting/ScriptTestUtils.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"
#include "UI/ScriptBrowser.h"
#include "UI/Theme.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Timestep.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

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
		CHECK(editor.GetPillText("Status.Scripts") == "Build failed");

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
#endif
	}

	TEST_CASE("New Script writes a script class and opens it")
	{
		const std::filesystem::path output = CreateTemporaryDirectory("NewScriptEditor") / "Arguments.txt";
		ScopedEnvironmentVariable externalEditor("STRATA_EXTERNAL_EDITOR", fmt::format("\"{}\" --strata-test-helper=record-arguments \"{}\" {{file}} {{line}}",
			FileSystem::ToUTF8(GetTestExecutablePath()), FileSystem::ToUTF8(output)));
		HarnessEditor editor;
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
		REQUIRE(editor.Click("NewScript.Create"));
		editor.Frames(2);
		CHECK_FALSE(editor.Layer->GetNewScriptDialog().IsOpen());
		const std::filesystem::path file = editor.Context().GetProject()->GetScriptSourceDirectory() / "DoorOpener.cpp";
		CHECK(FileSystem::ReadText(file).value_or("").find("class DoorOpener : public Script") != std::string::npos);
		const std::vector<std::string> lines = WaitForLines(output);
		REQUIRE(lines.size() == 2);
		std::error_code error;
		CHECK(std::filesystem::equivalent(FileSystem::FromUTF8(lines[0]), file, error));
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
}
