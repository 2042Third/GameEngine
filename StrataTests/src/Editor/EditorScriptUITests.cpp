#include <doctest/doctest.h>

#include "Editor/HarnessEditor.h"
#include "Editor/ImGuiHarness.h"
#include "EditorLayer.h"
#include "Panels/ProblemsPanel.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"

#include <Strata/Core/FileSystem.h>

#include <imgui.h>

#include <chrono>
#include <filesystem>
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
}
