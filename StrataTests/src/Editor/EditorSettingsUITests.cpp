#include <doctest/doctest.h>

#include "Core/SettingsTestUtils.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorPreferences.h"
#include "Editor/ExternalEditor.h"
#include "Editor/HarnessEditor.h"
#include "Editor/UndoStack.h"
#include "Panels/ProjectSettingsPanel.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"
#include "UI/SettingsPages.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Project/Project.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	bool IsDrawn(std::string_view key)
	{
		return UI::ItemProbe::Find(key).has_value();
	}

	// A project of the basic3d template (Scenes/Main.stscene, its start scene) with a second saved scene,
	// Scenes/Second.stscene, which is the edited one.
	void CreateProject(HarnessEditor& editor, const std::string& name)
	{
		const std::filesystem::path directory = CreateTemporaryDirectory(name) / "Game";
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" }, { "template", "basic3d" } });
		editor.Run("scene.saveAs", { { "path", "Scenes/Second.stscene" } });
		editor.Frames(2);
	}

	void Press(HarnessEditor& editor, ImGuiKey key)
	{
		editor.Harness.SetKey(key, true);
		editor.Frames(1);
		editor.Harness.SetKey(key, false);
		editor.Frames(2);
	}

	// Opens the Project Settings panel from the File menu.
	void OpenProjectSettings(HarnessEditor& editor)
	{
		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		REQUIRE(editor.Click("Menu.File.ProjectSettings"));
		REQUIRE(editor.FramesUntil([]() { return IsDrawn("ProjectSettings.Page.Game"); }));
	}

	// Opens the Preferences dialog from the File menu; it shows once it has been laid out (a dialog's first frame measures it).
	void OpenPreferences(HarnessEditor& editor)
	{
		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		REQUIRE(editor.Click("Menu.File.Preferences"));
		REQUIRE(editor.FramesUntil([&editor]() { return editor.Layer->GetPreferencesDialog().IsOpen(); }));
		editor.Frames(2);
	}

	// Picks an asset with a picker: click, search, Enter.
	void PickAsset(HarnessEditor& editor, std::string_view picker, std::string_view search)
	{
		REQUIRE(editor.Click(picker));
		editor.Frames(1);
		editor.Harness.TypeText(search);
		editor.Frames(2);
		Press(editor, ImGuiKey_Enter);
	}

	// Moves the mouse over the whole window in half-frame steps; fails at the first item whose id another item shares.
	void CheckIdConflicts(HarnessEditor& editor)
	{
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
	}

	// A registry of the editor's sections and the test sections: Test.Simple and Test.Complex (project), Test.Preferences (user).
	struct UIRegistry
	{
		SettingsRegistry Registry;

		UIRegistry()
		{
			EditorPreferences::RegisterSections(Registry);
			RegisterTestSections(Registry);
			REQUIRE(Registry.Register(MakeUserTestSection()));
			Registry.Freeze();
		}
	};

}

TEST_SUITE("Editor.UI.ProjectSettings")
{
	TEST_CASE("File > Project Settings opens the panel")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "ProjectSettingsPanel");
		CHECK_FALSE(editor.Layer->GetPanels().IsOpen(EditorPanels::c_ProjectSettings));
		OpenProjectSettings(editor);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_ProjectSettings));
		// The project's own pages come first.
		CHECK(UI::ItemProbe::Find("ProjectSettings.Title")->Text == "Game");
		CHECK(IsDrawn("ProjectSettings.Page.Export"));
		REQUIRE(editor.Click("ProjectSettings.Page.Export"));
		editor.Frames(2);
		CHECK(UI::ItemProbe::Find("ProjectSettings.Title")->Text == "Export");
		// editor.showPanel opens it too.
		editor.Layer->GetPanels().SetOpen(EditorPanels::c_ProjectSettings, false);
		editor.Frames(2);
		CHECK(editor.Run("editor.showPanel", { { "panel", "ProjectSettings" } })["panel"] == "ProjectSettings");
		CHECK(editor.FramesUntil([]() { return IsDrawn("ProjectSettings.Page.Game"); }));
	}

	TEST_CASE("The Game page sets the start and loading scenes through their commands")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "ProjectSettingsGame");
		const AssetHandle second = editor.Context().GetSceneHandle();
		OpenProjectSettings(editor);
		const size_t steps = editor.Context().GetUndoStack().GetCount();

		PickAsset(editor, "ProjectSettings.Game.LoadingScene", "Second");
		CHECK(editor.Context().GetProject()->GetConfig().Export.LoadingScene == second);
		PickAsset(editor, "ProjectSettings.Game.StartScene", "Second");
		CHECK(editor.Context().GetProject()->GetConfig().StartScene == second);
		// project.setStartScene cleared the loading scene, which was the new start scene.
		CHECK_FALSE(editor.Context().GetProject()->GetConfig().Export.LoadingScene.IsValid());
		// One undo step each, as the commands record them; the scene stays saved.
		CHECK(editor.Context().GetUndoStack().GetCount() == steps + 2);
		CHECK_FALSE(editor.Context().IsSceneModified());
	}

	TEST_CASE("The Export page lists what pruned exports keep and changes it through project.setExportSettings")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "ProjectSettingsExport");
		OpenProjectSettings(editor);
		REQUIRE(editor.Click("ProjectSettings.Page.Export"));
		editor.Frames(2);
		REQUIRE(editor.ReplaceText("ProjectSettings.Export.Path", "Scenes"));
		REQUIRE(editor.Click("ProjectSettings.Export.Add"));
		editor.Frames(2);
		CHECK(editor.Context().GetProject()->GetConfig().Export.AlwaysInclude == std::vector<std::string> { "Scenes" });
		CHECK(UI::ItemProbe::Find("ProjectSettings.Export.AlwaysInclude.0")->Text == "Scenes");
		// A path that names nothing is refused by the command and changes nothing.
		REQUIRE(editor.ReplaceText("ProjectSettings.Export.Path", "Missing"));
		REQUIRE(editor.Click("ProjectSettings.Export.Add"));
		editor.Frames(2);
		CHECK(editor.Context().GetProject()->GetConfig().Export.AlwaysInclude == std::vector<std::string> { "Scenes" });
		REQUIRE(editor.Click("ProjectSettings.Export.Remove.0"));
		editor.Frames(2);
		CHECK(editor.Context().GetProject()->GetConfig().Export.AlwaysInclude.empty());
	}

	TEST_CASE("Editing a generic field runs its section's command")
	{
		UIRegistry settings;
		EditorOptions options;
		options.Settings = &settings.Registry;
		HarnessEditor editor({}, options);
		editor.Frames(1);
		CreateProject(editor, "ProjectSettingsGeneric");
		OpenProjectSettings(editor);
		REQUIRE(editor.Click("ProjectSettings.Page.Test.Simple"));
		editor.Frames(2);
		const size_t steps = editor.Context().GetUndoStack().GetCount();

		// A check box runs project.setSettings with the section's whole value.
		REQUIRE(editor.Click("ProjectSettings.Test.Simple.Enabled"));
		editor.Frames(2);
		const nlohmann::json* value = editor.Context().GetProject()->GetSettings().FindSection("Test.Simple");
		REQUIRE(value);
		CHECK((*value)["Enabled"] == false);
		CHECK((*value)["Mode"] == "Fast");
		CHECK(editor.Context().GetUndoStack().GetCount() == steps + 1);
		CHECK(editor.Context().GetUndoStack().GetUndoName() == "Set Simple");

		// Text runs it once, when the edit ends; a value the section refuses changes nothing.
		REQUIRE(editor.ReplaceText("ProjectSettings.Test.Simple.Label", "Racing"));
		Press(editor, ImGuiKey_Enter);
		CHECK((*editor.Context().GetProject()->GetSettings().FindSection("Test.Simple"))["Label"] == "Racing");
		CHECK(editor.Context().GetUndoStack().GetCount() == steps + 2);
		REQUIRE(editor.ReplaceText("ProjectSettings.Test.Simple.Label", "A label longer than sixteen bytes"));
		Press(editor, ImGuiKey_Enter);
		CHECK((*editor.Context().GetProject()->GetSettings().FindSection("Test.Simple"))["Label"] == "Racing");
		CHECK(editor.Context().GetUndoStack().GetCount() == steps + 2);

		// Reset to Defaults removes the stored section.
		REQUIRE(editor.Click("ProjectSettings.Test.Simple.Reset"));
		editor.Frames(2);
		CHECK_FALSE(editor.Context().GetProject()->GetSettings().IsStored("Test.Simple"));

		// The same page draws a preference, through preferences.set and without an undo step.
		OpenPreferences(editor);
		REQUIRE(editor.Click("Preferences.Page.Test.Preferences"));
		editor.Frames(2);
		const size_t before = editor.Context().GetUndoStack().GetCount();
		REQUIRE(editor.ReplaceText("Preferences.Test.Preferences.Note", "Hello"));
		Press(editor, ImGuiKey_Enter);
		REQUIRE(editor.Context().GetPreferences().FindSection("Test.Preferences"));
		CHECK((*editor.Context().GetPreferences().FindSection("Test.Preferences"))["Note"] == "Hello");
		CHECK(editor.Context().GetUndoStack().GetCount() == before);
	}

	TEST_CASE("Every registered section has a page")
	{
		HarnessEditor editor;
		const EditorSettingsPageRegistry& pages = editor.Layer->GetSettingsPages();
		size_t complex = 0;
		for (const SettingsSection& section : SettingsRegistry::GetProcessRegistry().GetAll())
		{
			CAPTURE(section.Id);
			// A complex section's value has no fields to draw from: the editor UI registers a page for it.
			if (!section.IsSimple())
			{
				complex++;
				CHECK(pages.Has(section.Id));
			}
			const SettingsWindowKind window = section.Scope == SettingsScope::Project ? SettingsWindowKind::Project : SettingsWindowKind::Preferences;
			CHECK(pages.CreatePage(editor.Context(), section, window) != nullptr);
		}
		CHECK(complex > 0);
		// A simple section needs none.
		const SettingsSection simple = MakeSimpleTestSection();
		CHECK(pages.CreatePage(editor.Context(), simple, SettingsWindowKind::Project) != nullptr);
		CHECK(pages.CreatePage(editor.Context(), MakeComplexTestSection(), SettingsWindowKind::Project) == nullptr);
	}

	TEST_CASE("No id conflicts at 100% and 150%")
	{
		UIRegistry settings;
		for (const float scale : { 1.0f, 1.5f })
		{
			CAPTURE(scale);
			EditorOptions options;
			options.Settings = &settings.Registry;
			HarnessEditor editor({ ImVec2(1600.0f, 900.0f), scale }, options);
			editor.Frames(1);
			CreateProject(editor, "ProjectSettingsIds");
			editor.Run("project.setExportSettings", { { "alwaysInclude", { "Scenes" } } });
			OpenProjectSettings(editor);
			for (const char* page : { "ProjectSettings.Page.Game", "ProjectSettings.Page.Export", "ProjectSettings.Page.Test.Simple" })
			{
				CAPTURE(page);
				REQUIRE(editor.Click(page));
				editor.Frames(2);
				CheckIdConflicts(editor);
			}
			OpenPreferences(editor);
			for (const char* page : { "Preferences.Page.Editor.ExternalEditor", "Preferences.Page.Test.Preferences" })
			{
				CAPTURE(page);
				REQUIRE(editor.Click(page));
				editor.Frames(2);
				CheckIdConflicts(editor);
				// The dialog's items stay inside the window.
				const std::optional<UI::ItemProbe::Item> close = UI::ItemProbe::Find("Preferences.Close");
				REQUIRE(close);
				CHECK(close->Max.x <= ImGui::GetIO().DisplaySize.x + 0.5f);
				CHECK(close->Max.y <= ImGui::GetIO().DisplaySize.y + 0.5f);
			}
		}
	}
}

TEST_SUITE("Editor.UI.Preferences")
{
	TEST_CASE("File > Preferences opens from the launcher and from a project")
	{
		HarnessEditor editor;
		editor.Frames(2);
		REQUIRE(editor.Layer->IsLauncherShown());
		OpenPreferences(editor);
		CHECK(IsDrawn("Preferences.Page.Editor.ExternalEditor"));
		// The project's settings are no preferences.
		CHECK_FALSE(IsDrawn("Preferences.Page.Game"));
		REQUIRE(editor.Click("Preferences.Close"));
		editor.Frames(2);
		CHECK_FALSE(editor.Layer->GetPreferencesDialog().IsOpen());

		CreateProject(editor, "PreferencesFromProject");
		REQUIRE_FALSE(editor.Layer->IsLauncherShown());
		OpenPreferences(editor);
		// Escape closes it.
		Press(editor, ImGuiKey_Escape);
		CHECK_FALSE(editor.Layer->GetPreferencesDialog().IsOpen());
		// editor.showDialog opens it at a section, and refuses what is no preference.
		CHECK(editor.Run("editor.showDialog", { { "dialog", "preferences" }, { "section", ExternalEditor::c_SettingsSection } })["section"]
			== ExternalEditor::c_SettingsSection);
		CHECK(editor.FramesUntil([&editor]() { return editor.Layer->GetPreferencesDialog().IsOpen(); }));
		CHECK(editor.Layer->GetCommands().Execute(editor.Context(), "editor.showDialog", { { "dialog", "preferences" }, { "section", "Game.Missing" } })
			.ErrorKind == EditorCommandError::InvalidParameters);
	}

	TEST_CASE("Scripts > External Editor opens Preferences at its section")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "PreferencesExternalEditor");
		REQUIRE(editor.Click("Menu.Scripts"));
		editor.Frames(1);
		REQUIRE(editor.Click("Menu.Scripts.ExternalEditor"));
		REQUIRE(editor.FramesUntil([&editor]() { return editor.Layer->GetPreferencesDialog().IsOpen(); }));
		editor.Frames(2);
		CHECK(editor.Layer->GetPreferencesDialog().GetSelected() == ExternalEditor::c_SettingsSection);
		CHECK(UI::ItemProbe::Find("Preferences.Title")->Text == "External Editor");
		CHECK(IsDrawn("ExternalEditor.Command"));
		// A command set elsewhere meanwhile (an agent's preferences.set) shows in the page.
		CHECK(UI::ItemProbe::Find("ExternalEditor.Program")->Text == "Files open in the system's default application.");
		editor.Run("preferences.set", { { "section", ExternalEditor::c_SettingsSection }, { "value", { { "Command", "noeditor {file}" } } } });
		editor.Frames(2);
		CHECK(UI::ItemProbe::Find("ExternalEditor.Program")->Text.starts_with("Cannot start: there is no program 'noeditor'"));
	}
}
