#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/PropertyEdit.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"
#include "UI/RecoveryDialog.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

// Contracts of the editor's commands that agents rely on: values out of a property's range are refused rather than changed,
// and the editor window's dialogs open on request (editor.showDialog).

using namespace Strata;
using namespace Strata::Tests;

TEST_SUITE("Editor.Contracts")
{
	TEST_CASE("Properties tell whether values are within their range")
	{
		const ComponentInfo& text = *ComponentRegistry::Find("Text");
		const PropertyInfo& fontSize = *text.FindProperty("FontSize");
		std::string error;
		CHECK(fontSize.IsInRange(PropertyValue(24.0f)));
		CHECK(fontSize.IsInRange(PropertyValue(1.0f))); // Bounds are inside
		CHECK_FALSE(fontSize.IsInRange(PropertyValue(0.5f), &error));
		CHECK(error == "must be between 1 and 1024, not 0.5");
		// Every component of a vector; properties without a range take anything.
		const ComponentInfo& transform = *ComponentRegistry::Find("Transform");
		CHECK(transform.FindProperty("Translation")->IsInRange(PropertyValue(glm::vec3(-1.0e6f, 0.0f, 1.0e6f))));
		const ComponentInfo& light = *ComponentRegistry::Find("PointLight");
		CHECK_FALSE(light.FindProperty("Range")->IsInRange(PropertyValue(0.0f)));
	}

	TEST_CASE("Commands refuse values out of a property's range and change nothing; the inspector still clamps")
	{
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		const std::string id = RunCommandToCompletion(context, commands, "entity.create",
			{ { "name", "Title" }, { "components", { { "Text", { { "Text", "Hi" }, { "FontSize", 24 } } } } } })["id"].get<std::string>();
		const size_t steps = context.GetUndoStack().GetPosition();

		// component.set: the range in the message, the value and the history unchanged.
		const EditorCommandResult set = commands.Execute(context, "component.set", { { "entity", id }, { "component", "Text" }, { "values", { { "FontSize", 0.5 } } } });
		CHECK(set.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(set.Error.find("Text.FontSize must be between 1 and 1024, not 0.5") != std::string::npos);
		const Entity entity = context.GetEditScene()->GetEntityByUUID(*UUID::FromString(id));
		CHECK(entity.GetComponent<TextComponent>().FontSize == 24.0f);
		CHECK(context.GetUndoStack().GetPosition() == steps);

		// entity.create and component.add with components: nothing is created.
		const size_t entities = context.GetEditScene()->GetEntityCount();
		CHECK(commands.Execute(context, "entity.create", { { "name", "Lamp" }, { "components", { { "PointLight", { { "Range", -2 } } } } } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(commands.Execute(context, "component.add", { { "entity", id }, { "component", "PointLight" }, { "values", { { "Range", 0 } } } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(context.GetEditScene()->GetEntityCount() == entities);
		CHECK_FALSE(entity.HasComponent<PointLightComponent>());

		// scene.setSettings names its ranges too.
		const EditorCommandResult settings = commands.Execute(context, "scene.setSettings", { { "fixedTimestep", 1.0 } });
		CHECK(settings.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(settings.Error.find("between 0.001 and 0.1") != std::string::npos);
		CHECK(context.GetUndoStack().GetPosition() == steps);

		// The inspector clamps what it is given, as a drag past the end does.
		const ComponentInfo& text = *ComponentRegistry::Find("Text");
		std::string error;
		REQUIRE_MESSAGE(SetPropertyWithUndo(context, entity, text, *text.FindProperty("FontSize"), PropertyValue(0.5f), &error), error);
		CHECK(entity.GetComponent<TextComponent>().FontSize == 1.0f);
	}

	TEST_CASE("material.set refuses values out of a material property's range")
	{
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		RunCommandToCompletion(context, commands, "project.create", { { "directory", FileSystem::ToUTF8(CreateTemporaryDirectory("ContractMaterial") / "Game") }, { "name", "Game" } });
		RunCommandToCompletion(context, commands, "material.create", { { "path", "Materials/Rough.stmat" }, { "properties", { { "Roughness", 0.5 } } } });
		const std::filesystem::path file = context.GetProject()->GetAssetDirectory() / "Materials/Rough.stmat";
		const std::optional<std::string> before = FileSystem::ReadText(file);
		REQUIRE(before.has_value());

		const EditorCommandResult result = commands.Execute(context, "material.set", { { "material", "Materials/Rough.stmat" }, { "properties", { { "Roughness", 2.0 } } } });
		CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(result.Error.find("Roughness must be between 0 and 1, not 2") != std::string::npos);
		CHECK(FileSystem::ReadText(file) == before);
		CHECK(commands.Execute(context, "material.create", { { "path", "Materials/Bad.stmat" }, { "properties", { { "Roughness", -1 } } } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK_FALSE(FileSystem::Exists(context.GetProject()->GetAssetDirectory() / "Materials/Bad.stmat"));
	}
}

TEST_SUITE("Editor.UI.Dialogs")
{
	TEST_CASE("editor.showDialog opens the window's dialogs")
	{
		HarnessEditor editor;
		editor.Layer->DismissLauncher();
		editor.Frames(2);
		// Without a project there is nothing to export.
		const EditorCommandResult exportWithoutProject = editor.Layer->GetCommands().Execute(editor.Context(), "editor.showDialog", { { "dialog", "export" } });
		CHECK(exportWithoutProject.ErrorKind == EditorCommandError::Failed);
		CHECK(editor.Layer->GetCommands().Execute(editor.Context(), "editor.showDialog", { { "dialog", "settings" } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(editor.Layer->GetCommands().Execute(editor.Context(), "editor.showDialog", { { "dialog", "newProject" }, { "template", "nope" } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		// Recovery data belongs to a project.
		CHECK(editor.Layer->GetCommands().Execute(editor.Context(), "editor.showDialog", { { "dialog", "recovery" } }).ErrorKind
			== EditorCommandError::Failed);

		editor.Run("editor.showDialog", { { "dialog", "about" } });
		editor.Frames(2);
		CHECK(editor.Layer->GetAboutDialog().IsOpen());
	}

	TEST_CASE("editor.showDialog opens the project dialogs and the export dialog of an open project")
	{
		HarnessEditor editor({}, WithFeatureProject("ShowDialog"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		editor.Run("editor.showDialog", { { "dialog", "export" } });
		editor.Frames(2);
		CHECK(editor.Layer->GetExportDialog().IsOpen());
	}

	TEST_CASE("editor.showDialog shows the person what can be recovered")
	{
		HarnessEditor editor({}, WithFeatureProject("ShowRecovery"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		// An agent that discarded the person's changes shows them where they are.
		editor.Run("entity.create", { { "name", "Kept" } });
		editor.Run("scene.new", { { "force", true } });
		editor.Run("editor.showDialog", { { "dialog", "recovery" } });
		CHECK(editor.FramesUntil([&]() { return UI::ItemProbe::Find("Recovery.Recover.0").has_value(); }));
		CHECK(editor.Layer->GetRecoveryDialog().IsOpen());
	}

	TEST_CASE("editor.showDialog opens New Project with its template")
	{
		HarnessEditor editor;
		editor.Layer->DismissLauncher();
		editor.Frames(2);
		editor.Run("editor.showDialog", { { "dialog", "newProject" }, { "template", "empty" } });
		editor.Frames(2);
		CHECK(editor.Layer->GetProjectDialogs().IsOpen());
	}
}
