#include <doctest/doctest.h>

#include "Core/SettingsTestUtils.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorInterface.h"
#include "Editor/EditorPreferences.h"
#include "Editor/ExternalEditor.h"
#include "Editor/HarnessEditor.h"
#include "Editor/ImGuiHarness.h"
#include "Editor/UndoStack.h"
#include "TestHelpers.h"
#include "UI/BindingCapture.h"
#include "UI/InputActionsPage.h"
#include "UI/ItemProbe.h"
#include "UI/SettingsPages.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Input/Input.h>
#include <Strata/Input/InputActions.h>
#include <Strata/Input/InputActionSet.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/PropertyJson.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

	// A frame as the editor application runs one: an input frame, the input actions (and a binding capture), then the editor.
	void InputFrame(HarnessEditor& editor, float seconds = ImGuiHarness::c_DeltaTime)
	{
		Input::BeginFrame();
		InputActions::Update();
		editor.Frames(1, seconds);
	}

	// A project with the map Gameplay (Jump on Space), shown on the Input Actions page.
	struct CapturePage
	{
		HarnessEditor Editor;

		explicit CapturePage(const std::string& name)
		{
			Input::Reset();
			Editor.Frames(1);
			CreateProject(Editor, name);
			Editor.Run("inputActions.setMap", { { "map", "Gameplay" } });
			Editor.Run("inputActions.setAction", { { "action", "Gameplay/Jump" }, { "bindings", { "Key/Space" } } });
			OpenProjectSettings(Editor);
			REQUIRE(Editor.Click("ProjectSettings.Page.Input.Actions"));
			REQUIRE(Editor.FramesUntil([]() { return IsDrawn(Key("Map.1")); }));
			REQUIRE(Editor.Click(Key("Map.1")));
			REQUIRE(Editor.FramesUntil([]() { return IsDrawn(Key("Action.0.CaptureBinding")); }));
		}

		~CapturePage()
		{
			Input::Reset();
		}

		static std::string Key(std::string_view item)
		{
			return std::string(InputActionsPage::c_ProbePrefix) + std::string(item);
		}

		std::vector<std::string> Bindings(std::string_view action)
		{
			const nlohmann::json* value = Editor.Context().GetProject()->GetSettings().FindSection(InputActionSet::c_SectionId);
			REQUIRE(value);
			const std::optional<InputActionSet> set = InputActionSet::FromJson(*value);
			REQUIRE(set);
			const std::optional<InputActionLocation> location = set->FindAction(action);
			REQUIRE(location);
			std::vector<std::string> texts;
			for (const InputBinding& binding : set->GetAction(*location).Bindings)
				texts.push_back(binding.ToString());
			return texts;
		}

		void StartCapture()
		{
			REQUIRE(Editor.Click(Key("Action.0.CaptureBinding")));
			REQUIRE(InputActions::IsCapturing());
			Editor.Frames(1);
			REQUIRE(IsDrawn(Key("Capture")));
		}

		// A key going down or up on the keyboard: the devices (what the capture reads) and ImGui (what shortcuts read).
		void SetKey(KeyCode key, ImGuiKey imguiKey, bool down)
		{
			Input::ProcessKey(key, down);
			Editor.Harness.SetKey(imguiKey, down);
		}
	};

	// A registry of the editor's sections, the input actions and the test sections: Test.Simple and Test.Complex (project),
	// Test.Preferences (user).
	struct UIRegistry
	{
		SettingsRegistry Registry;

		UIRegistry()
		{
			EditorPreferences::RegisterSections(Registry);
			RegisterTestSections(Registry);
			REQUIRE(Registry.Register(MakeUserTestSection()));
			REQUIRE(Registry.Register(InputActionSet::MakeSection()));
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

	TEST_CASE("The Game page sets the icon through the asset picker")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "ProjectSettingsGameIcon");
		const std::filesystem::path source = CreateTemporaryDirectory("ProjectSettingsGameIconSource") / "Icon.png";
		REQUIRE(FileSystem::WriteBytes(source, CreateSolidPNG(16, 16, 40, 120, 220)));
		const std::string icon = editor.Run("asset.import", { { "file", FileSystem::ToUTF8(source) }, { "directory", "Textures" } })["asset"].get<std::string>();
		OpenProjectSettings(editor);
		const size_t steps = editor.Context().GetUndoStack().GetCount();
		const ProjectGameInfo& game = editor.Context().GetProject()->GetConfig().Game;

		// The text fields run project.setGameInfo when their edit ends.
		REQUIRE(editor.ReplaceText("ProjectSettings.Game.Title", "Star Adventure"));
		Press(editor, ImGuiKey_Enter);
		CHECK(game.Title == "Star Adventure");
		REQUIRE(editor.ReplaceText("ProjectSettings.Game.Company", "Studio"));
		Press(editor, ImGuiKey_Enter);
		CHECK(game.Company == "Studio");
		// A version the command refuses changes nothing, and the field shows the project's again.
		REQUIRE(editor.ReplaceText("ProjectSettings.Game.Version", "one"));
		Press(editor, ImGuiKey_Enter);
		CHECK(game.Version == ProjectGameInfo::c_DefaultVersion);
		CHECK(UI::ItemProbe::Find("ProjectSettings.Game.Version")->Text == ProjectGameInfo::c_DefaultVersion);

		CHECK_FALSE(IsDrawn("ProjectSettings.Game.IconPreview"));
		PickAsset(editor, "ProjectSettings.Game.Icon", "Icon");
		CHECK(UUIDToJson(game.Icon) == icon);
		CHECK(IsDrawn("ProjectSettings.Game.IconPreview"));
		// One undo step each, as the command records them; the scene stays saved.
		CHECK(editor.Context().GetUndoStack().GetCount() == steps + 3);
		CHECK_FALSE(editor.Context().IsSceneModified());

		// The Export dialog sums the identity up and leads to this page.
		REQUIRE(editor.Click("ProjectSettings.Page.Export"));
		editor.Frames(2);
		CHECK_FALSE(IsDrawn("ProjectSettings.Game.Title"));
		REQUIRE(editor.Click("Toolbar.Export"));
		editor.Frames(2);
		REQUIRE(editor.Layer->GetExportDialog().IsOpen());
		REQUIRE(IsDrawn("Export.Identity"));
		CHECK(UI::ItemProbe::Find("Export.Identity")->Text == "Star Adventure 1.0.0 by Studio, with the icon Textures/Icon.png");
		REQUIRE(editor.Click("Export.EditIdentity"));
		editor.Frames(2);
		CHECK_FALSE(editor.Layer->GetExportDialog().IsOpen());
		CHECK(IsDrawn("ProjectSettings.Game.Title"));
	}

	TEST_CASE("The Export dialog says when the game has no icon and when its icon is missing")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "ExportIdentityIcon");
		REQUIRE(editor.Click("Toolbar.Export"));
		editor.Frames(2);
		REQUIRE(IsDrawn("Export.Identity"));
		CHECK(UI::ItemProbe::Find("Export.Identity")->Text == "Game 1.0.0, without an icon of its own");

		// An icon the project names but no longer has fails the export: the summary says so rather than name a fallback.
		const std::filesystem::path source = CreateTemporaryDirectory("ExportIdentityIconSource") / "Icon.png";
		REQUIRE(FileSystem::WriteBytes(source, CreateSolidPNG(16, 16, 40, 120, 220)));
		const std::string icon = editor.Run("asset.import", { { "file", FileSystem::ToUTF8(source) }, { "directory", "Textures" } })["asset"].get<std::string>();
		editor.Run("project.setGameInfo", { { "icon", icon } });
		editor.Run("asset.delete", { { "asset", icon }, { "force", true } });
		editor.Frames(2);
		REQUIRE(editor.Layer->GetExportDialog().IsOpen());
		CHECK(UI::ItemProbe::Find("Export.Identity")->Text == "Game 1.0.0, with the missing icon " + icon + ": choose another or none");
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

	TEST_CASE("The Input Actions page edits maps, actions and bindings through the commands")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "ProjectSettingsInputActions");
		OpenProjectSettings(editor);
		const auto key = [](std::string_view item) { return std::string(InputActionsPage::c_ProbePrefix) + std::string(item); };
		const auto text = [](const std::string& probe)
		{
			const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(probe);
			REQUIRE_MESSAGE(item.has_value(), "No ", probe, " in the last frame");
			return item->Text;
		};
		const auto actions = [&editor]()
		{
			const nlohmann::json* value = editor.Context().GetProject()->GetSettings().FindSection(InputActionSet::c_SectionId);
			const std::optional<InputActionSet> set = value ? InputActionSet::FromJson(*value) : std::optional<InputActionSet>(InputActionSet::MakeDefault());
			REQUIRE(set);
			return *set;
		};
		const auto bindings = [&actions](std::string_view name)
		{
			const InputActionSet set = actions();
			const std::optional<InputActionLocation> location = set.FindAction(name);
			REQUIRE(location);
			std::vector<std::string> texts;
			for (const InputBinding& binding : set.GetAction(*location).Bindings)
				texts.push_back(binding.ToString());
			return texts;
		};
		const auto click = [&editor](const std::string& probe)
		{
			INFO(probe);
			REQUIRE(editor.Click(probe));
			editor.Frames(2);
		};
		REQUIRE(editor.Click("ProjectSettings.Page.Input.Actions"));
		REQUIRE(editor.FramesUntil([&key]() { return IsDrawn(key("Summary")); }));
		CHECK(text(key("Summary")) == "1 map \xC2\xB7 6 actions");
		// Without a game map the UI map shows: its actions are the game UI's and keep their names.
		CHECK(text(key("Action.0.Reserved")) == "Used by game UI navigation");
		CHECK_FALSE(UI::ItemProbe::Find(key("Action.0.Remove"))->Enabled);
		CHECK_FALSE(UI::ItemProbe::Find(key("RemoveMap"))->Enabled);
		const size_t steps = editor.Context().GetUndoStack().GetCount();

		// A map, named in place; an action, named in place.
		click(key("AddMap"));
		CHECK(actions().FindMap("New Map"));
		REQUIRE(editor.ReplaceText(key("MapName"), "Gameplay"));
		Press(editor, ImGuiKey_Enter);
		CHECK(actions().FindMap("Gameplay"));
		click(key("AddAction"));
		REQUIRE(actions().FindAction("Gameplay/New Action"));
		REQUIRE(editor.ReplaceText(key("Action.0.Name"), "Jump"));
		Press(editor, ImGuiKey_Enter);
		REQUIRE(actions().FindAction("Gameplay/Jump"));

		// A binding chosen from the list of controls.
		click(key("Action.0.AddBinding"));
		click(key("AddBinding.Part.0"));
		click(key("AddBinding.Part.0.Key/Comma"));
		click(key("AddBinding.Add"));
		CHECK(bindings("Gameplay/Jump") == std::vector<std::string> { "Key/Comma" });
		CHECK(text(key("Action.0.Binding.0")) == "Key/Comma");

		// Another kind keeps the bindings that fit it; a 2D composite is put together part by part.
		click(key("Action.0.Kind"));
		click(key("Action.0.Kind.Vector2"));
		CHECK(actions().GetAction(*actions().FindAction("Jump")).Kind == InputActionKind::Vector2);
		CHECK(bindings("Gameplay/Jump").empty());
		click(key("Action.0.AddBinding"));
		click(key("AddBinding.Shape"));
		click(key("AddBinding.Shape.2D"));
		const char* parts[] = { "Key/Apostrophe", "Key/Comma", "Key/Minus", "Key/Period" };
		for (size_t part = 0; part < 4; part++)
		{
			click(key(fmt::format("AddBinding.Part.{}", part)));
			click(key(fmt::format("AddBinding.Part.{}.{}", part, parts[part])));
		}
		click(key("AddBinding.Add"));
		CHECK(bindings("Gameplay/Jump") == std::vector<std::string> { "2D:Key/Apostrophe,Key/Comma,Key/Minus,Key/Period" });

		// Removing the binding, the action, the map; the map's enable at start.
		click(key("Action.0.RemoveBinding.0"));
		CHECK(bindings("Gameplay/Jump").empty());
		click(key("Action.0.Remove"));
		CHECK_FALSE(actions().FindAction("Gameplay/Jump"));
		click(key("MapEnabled"));
		CHECK_FALSE(actions().Maps[*actions().FindMap("Gameplay")].Enabled);
		click(key("RemoveMap"));
		CHECK_FALSE(actions().FindMap("Gameplay"));
		// One step per edit: the map, its name, the action, its name, two bindings, the kind, three removals and the enable.
		CHECK(editor.Context().GetUndoStack().GetCount() == steps + 11);
		CHECK(text(key("Summary")) == "1 map \xC2\xB7 6 actions");
		CHECK_FALSE(IsDrawn(key("Error")));
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
			editor.Run("inputActions.setAction", { { "action", "UI/Pause" }, { "kind", "Axis" }, { "bindings", { "Key/P", "1D:Key/A,Key/D" } } });
			OpenProjectSettings(editor);
			for (const char* page : { "ProjectSettings.Page.Game", "ProjectSettings.Page.Export", "ProjectSettings.Page.Test.Simple",
				"ProjectSettings.Page.Input.Actions" })
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

	TEST_CASE("The Interface page's scale runs preferences.set")
	{
		HarnessEditor editor;
		editor.Frames(1);
		OpenPreferences(editor);
		REQUIRE(editor.Click("Preferences.Page.Editor.Interface"));
		editor.Frames(2);
		const std::string field = std::string("Preferences.") + EditorInterface::c_SettingsSection + ".Scale";
		REQUIRE(IsDrawn(field));
		CHECK(UI::ItemProbe::Find(field)->Text == "Automatic");

		const size_t steps = editor.Context().GetUndoStack().GetCount();
		REQUIRE(editor.Click(field));
		editor.Frames(1);
		REQUIRE(editor.Click(field + ".150%"));
		editor.Frames(2);
		const nlohmann::json* value = editor.Context().GetPreferences().FindSection(EditorInterface::c_SettingsSection);
		REQUIRE(value);
		CHECK((*value)["Scale"] == "150%");
		CHECK(editor.Host->UIScaleFactor == doctest::Approx(1.5f));
		CHECK(UI::ItemProbe::Find(field)->Text == "150%");
		// A preference, not a step of the project's history.
		CHECK(editor.Context().GetUndoStack().GetCount() == steps);
	}

	TEST_CASE("Preferences open again from what is set: a command typed but not applied is gone")
	{
		HarnessEditor editor;
		editor.Frames(1);
		editor.Layer->ShowPreferencesDialog(ExternalEditor::c_SettingsSection);
		REQUIRE(editor.FramesUntil([&editor]() { return editor.Layer->GetPreferencesDialog().IsOpen(); }));
		editor.Frames(2);
		REQUIRE(editor.ReplaceText("ExternalEditor.Command", "noeditor {file}"));
		editor.Frames(2);
		REQUIRE(UI::ItemProbe::Find("ExternalEditor.Program")->Text.starts_with("Cannot start: there is no program 'noeditor'"));
		REQUIRE(editor.Click("Preferences.Close"));
		REQUIRE(editor.FramesUntil([&editor]() { return !editor.Layer->GetPreferencesDialog().IsOpen(); }));
		CHECK(editor.Context().GetExternalEditor().GetUserCommand().empty());

		editor.Layer->ShowPreferencesDialog(ExternalEditor::c_SettingsSection);
		REQUIRE(editor.FramesUntil([&editor]() { return editor.Layer->GetPreferencesDialog().IsOpen(); }));
		editor.Frames(2);
		CHECK(UI::ItemProbe::Find("ExternalEditor.Program")->Text == "Files open in the system's default application.");
		CHECK_FALSE(UI::ItemProbe::Find("ExternalEditor.Apply")->Enabled);
	}
}

TEST_SUITE("Editor.UI.InputActions")
{
	TEST_CASE("The capture binds the next key and runs setAction")
	{
		CapturePage page("InputActionsCaptureKey");
		const size_t steps = page.Editor.Context().GetUndoStack().GetCount();
		page.StartCapture();
		CHECK(UI::ItemProbe::Find(CapturePage::Key("Capture"))->Text.starts_with("Press a key, mouse button or gamepad control to bind (10 s)"));
		// While it listens every action reads as up; the key it takes is bound with one undo step.
		page.SetKey(Key::K, ImGuiKey_K, true);
		InputFrame(page.Editor);
		CHECK(page.Bindings("Gameplay/Jump") == std::vector<std::string> { "Key/Space", "Key/K" });
		CHECK(page.Editor.Context().GetUndoStack().GetCount() == steps + 1);
		CHECK_FALSE(InputActions::IsCapturing());
		page.SetKey(Key::K, ImGuiKey_K, false);
		InputFrame(page.Editor);
		CHECK_FALSE(IsDrawn(CapturePage::Key("Capture")));
		CHECK(IsDrawn(CapturePage::Key("Action.0.Binding.1")));

		// A part of a composite: the binding popup's part takes the key, and Add adds the composite.
		page.Editor.Run("inputActions.setAction", { { "action", "Gameplay/Steer" }, { "kind", "Axis" } });
		REQUIRE(page.Editor.FramesUntil([]() { return IsDrawn(CapturePage::Key("Action.1.AddBinding")); }));
		// A Vector2 takes no single pressed control.
		page.Editor.Run("inputActions.setAction", { { "action", "Gameplay/Move" }, { "kind", "Vector2" } });
		REQUIRE(page.Editor.FramesUntil([]() { return IsDrawn(CapturePage::Key("Action.2.CaptureBinding")); }));
		CHECK_FALSE(UI::ItemProbe::Find(CapturePage::Key("Action.2.CaptureBinding"))->Enabled);
		REQUIRE(page.Editor.Click(CapturePage::Key("Action.1.AddBinding")));
		page.Editor.Frames(2);
		REQUIRE(page.Editor.Click(CapturePage::Key("AddBinding.Shape")));
		page.Editor.Frames(1);
		REQUIRE(page.Editor.Click(CapturePage::Key("AddBinding.Shape.1D")));
		page.Editor.Frames(1);
		REQUIRE(page.Editor.Click(CapturePage::Key("AddBinding.Capture.1")));
		page.Editor.Frames(1);
		CHECK(IsDrawn(CapturePage::Key("Capture")));
		page.SetKey(Key::L, ImGuiKey_L, true);
		InputFrame(page.Editor);
		page.SetKey(Key::L, ImGuiKey_L, false);
		InputFrame(page.Editor);
		CHECK(UI::ItemProbe::Find(CapturePage::Key("AddBinding.Part.1"))->Text == "Key/L");
		REQUIRE(page.Editor.Click(CapturePage::Key("AddBinding.Add")));
		page.Editor.Frames(2);
		CHECK(page.Bindings("Gameplay/Steer") == std::vector<std::string> { "1D:Key/Space,Key/L" });
		CHECK_FALSE(IsDrawn(CapturePage::Key("Error")));
	}

	TEST_CASE("Keys pressed while capturing run no shortcut")
	{
		CapturePage page("InputActionsCaptureShortcut");
		const size_t position = page.Editor.Context().GetUndoStack().GetPosition();
		page.StartCapture();
		// Ctrl is the key pressed first: it is captured. Z pressed with it is no undo, as long as a key captured with it is held.
		page.SetKey(Key::LeftControl, ImGuiMod_Ctrl, true);
		InputFrame(page.Editor);
		CHECK(page.Bindings("Gameplay/Jump") == std::vector<std::string> { "Key/Space", "Key/LeftControl" });
		page.SetKey(Key::Z, ImGuiKey_Z, true);
		InputFrame(page.Editor);
		page.SetKey(Key::Z, ImGuiKey_Z, false);
		InputFrame(page.Editor);
		CHECK(page.Editor.Context().GetUndoStack().GetPosition() == position + 1);
		CHECK(page.Bindings("Gameplay/Jump") == std::vector<std::string> { "Key/Space", "Key/LeftControl" });
		page.SetKey(Key::LeftControl, ImGuiMod_Ctrl, false);
		InputFrame(page.Editor);

		// Keys pressed together: the first in code order is captured.
		page.StartCapture();
		page.SetKey(Key::Escape, ImGuiKey_Escape, true);
		page.SetKey(Key::Tab, ImGuiKey_Tab, true);
		InputFrame(page.Editor);
		CHECK(page.Bindings("Gameplay/Jump") == std::vector<std::string> { "Key/Space", "Key/LeftControl", "Key/Escape" });
		page.SetKey(Key::Escape, ImGuiKey_Escape, false);
		page.SetKey(Key::Tab, ImGuiKey_Tab, false);
		InputFrame(page.Editor);

		// Once every key is up the keyboard is the editor's again: Ctrl+Z undoes the capture's step.
		page.Editor.Harness.SetKey(ImGuiMod_Ctrl, true);
		page.Editor.Frames(1);
		page.Editor.Harness.SetKey(ImGuiKey_Z, true);
		page.Editor.Frames(1);
		page.Editor.Harness.SetKey(ImGuiKey_Z, false);
		page.Editor.Harness.SetKey(ImGuiMod_Ctrl, false);
		page.Editor.Frames(2);
		CHECK(page.Bindings("Gameplay/Jump") == std::vector<std::string> { "Key/Space", "Key/LeftControl" });
	}

	TEST_CASE("Cancel and the timeout end a capture")
	{
		CapturePage page("InputActionsCaptureCancel");
		page.StartCapture();
		REQUIRE(page.Editor.Click(CapturePage::Key("Capture.Cancel")));
		page.Editor.Frames(1);
		CHECK_FALSE(InputActions::IsCapturing());
		CHECK_FALSE(IsDrawn(CapturePage::Key("Capture")));

		// The capture hears the mouse too: the press of a click on Cancel cancels, it binds nothing.
		page.StartCapture();
		page.Editor.Harness.MoveMouse(UI::ItemProbe::Find(CapturePage::Key("Capture.Cancel"))->GetCenter());
		page.Editor.Frames(1);
		Input::ProcessMouseButton(Mouse::ButtonLeft, true);
		page.Editor.Harness.SetMouseButton(ImGuiMouseButton_Left, true);
		InputFrame(page.Editor);
		Input::ProcessMouseButton(Mouse::ButtonLeft, false);
		page.Editor.Harness.SetMouseButton(ImGuiMouseButton_Left, false);
		InputFrame(page.Editor);
		CHECK_FALSE(InputActions::IsCapturing());
		CHECK_FALSE(IsDrawn(CapturePage::Key("Capture")));
		CHECK(page.Bindings("Gameplay/Jump") == std::vector<std::string> { "Key/Space" });

		// Ten seconds without a control end it.
		page.StartCapture();
		page.Editor.Frames(1, 5.0f);
		CHECK(UI::ItemProbe::Find(CapturePage::Key("Capture"))->Text.find("(5 s)") != std::string::npos);
		page.Editor.Frames(2, 5.0f);
		CHECK_FALSE(InputActions::IsCapturing());
		CHECK_FALSE(IsDrawn(CapturePage::Key("Capture")));

		// So does showing another map: nobody sees it listen.
		page.StartCapture();
		REQUIRE(page.Editor.Click(CapturePage::Key("Map.0")));
		CHECK_FALSE(InputActions::IsCapturing());
		page.SetKey(Key::K, ImGuiKey_K, true);
		InputFrame(page.Editor);
		page.SetKey(Key::K, ImGuiKey_K, false);
		InputFrame(page.Editor);
		CHECK(page.Bindings("Gameplay/Jump") == std::vector<std::string> { "Key/Space" });
	}

	TEST_CASE("A captured mouse press clicks nothing, also in windows drawn before the page")
	{
		Input::Reset();
		ImGuiHarness harness;
		// Frames as the editor runs them: the input frame and the capture, then the editor's UI, which starts with the capture's
		// frame, draws a window that acts on a press (as the viewport picks) and then the page, which asks after its capture.
		bool clicked = false;
		std::optional<std::string> taken;
		const auto frame = [&]()
		{
			Input::BeginFrame();
			InputActions::Update();
			harness.Frame([&]()
			{
				BindingCapture::BeginFrame();
				clicked = clicked || ImGui::IsMouseClicked(ImGuiMouseButton_Left);
				if (!BindingCapture::IsListening("Test"))
				{
					if (std::optional<std::string> binding = BindingCapture::Take("Test"))
						taken = std::move(binding);
				}
			});
		};
		const auto setMouse = [&](bool down)
		{
			Input::ProcessMouseButton(Mouse::ButtonLeft, down);
			harness.SetMouseButton(ImGuiMouseButton_Left, down);
		};
		harness.MoveMouse(ImVec2(100.0f, 100.0f));
		frame();
		bool begun = false;
		harness.Frame([&]() { begun = BindingCapture::Begin("Test"); });
		REQUIRE(begun);
		frame();
		CHECK(InputActions::IsCapturing());

		// The press the capture takes: no window sees it, not even one drawn before the page collects the binding.
		setMouse(true);
		frame();
		CHECK(taken == std::optional<std::string>("Mouse/Left"));
		CHECK_FALSE(clicked);
		setMouse(false);
		frame();
		CHECK_FALSE(clicked);

		// The next press is the editor's again.
		setMouse(true);
		frame();
		CHECK(clicked);
		setMouse(false);
		frame();
		Input::Reset();
	}

	TEST_CASE("A capture while play is paused binds the next key")
	{
		CapturePage page("InputActionsCapturePaused");
		page.Editor.Run("play.start", { { "build", false } });
		page.Editor.Run("play.pause", { { "paused", true } });
		page.Editor.Frames(2);
		REQUIRE(Input::IsSuspended());
		page.StartCapture();
		page.SetKey(Key::K, ImGuiKey_K, true);
		InputFrame(page.Editor);
		CHECK(page.Bindings("Gameplay/Jump") == std::vector<std::string> { "Key/Space", "Key/K" });
		CHECK_FALSE(InputActions::IsCapturing());
		page.SetKey(Key::K, ImGuiKey_K, false);
		InputFrame(page.Editor);

		// Play resumes, and the running game jumps with K (simulated input reaches it without the Game view's focus).
		page.Editor.Run("play.pause", { { "paused", false } });
		InputFrame(page.Editor);
		Input::SimulateKey(Key::K, true);
		InputFrame(page.Editor);
		CHECK(InputActions::IsPressed("Jump"));
		Input::SimulateKey(Key::K, false);
		page.Editor.Run("play.stop");
	}
}
