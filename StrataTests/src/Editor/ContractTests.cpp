#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/PropertyEdit.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"
#include "UI/RecoveryDialog.h"

#include <Strata/Core/Base64.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/ImageReader.h>
#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Scene/Components.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <memory>
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

	TEST_CASE("Material texture maps take textures by path or handle and refuse references to nothing or to other assets")
	{
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		const std::filesystem::path directory = CreateTemporaryDirectory("ContractMaterialMaps") / "Game";
		RunCommandToCompletion(context, commands, "project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		const std::filesystem::path external = directory.parent_path() / "Checker.png";
		REQUIRE(FileSystem::WriteBytes(external, CreateSolidPNG(8, 8, 200, 100, 50)));
		const std::string texture = RunCommandToCompletion(context, commands, "asset.import", { { "file", FileSystem::ToUTF8(external) },
			{ "directory", "Textures" } })["asset"].get<std::string>();

		// By path, as agents name assets: stored as the texture's handle.
		const nlohmann::json created = RunCommandToCompletion(context, commands, "material.create", { { "path", "Materials/Checker.stmat" },
			{ "properties", { { "BaseColorMap", "Textures/Checker.png" } } } });
		CHECK(RunCommandToCompletion(context, commands, "material.get", { { "material", "Materials/Checker.stmat" } })["values"]["BaseColorMap"] == texture);
		const std::filesystem::path file = context.GetProject()->GetAssetDirectory() / "Materials/Checker.stmat";
		const std::optional<std::string> before = FileSystem::ReadText(file);
		REQUIRE(before.has_value());

		// A handle of nothing, an asset of another type, a path of nothing: refused, the file unchanged.
		for (const char* reference : { "0000000000000ABC", "0000000000000001", "Textures/Missing.png" })
		{
			CAPTURE(reference);
			const EditorCommandResult result = commands.Execute(context, "material.set", { { "material", "Materials/Checker.stmat" },
				{ "properties", { { "NormalMap", reference } } } });
			CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
			CHECK(result.Error.find("NormalMap") != std::string::npos);
		}
		const EditorCommandResult mesh = commands.Execute(context, "material.set", { { "material", "Materials/Checker.stmat" },
			{ "properties", { { "NormalMap", "Builtin/Cube" } } } });
		CHECK(mesh.Error.find("is a Mesh, not a Texture") != std::string::npos);
		CHECK(FileSystem::ReadText(file) == before);
		CHECK(commands.Execute(context, "material.create", { { "path", "Materials/Broken.stmat" }, { "properties", { { "BaseColorMap", "0000000000000ABC" } } } })
			.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK_FALSE(FileSystem::Exists(context.GetProject()->GetAssetDirectory() / "Materials/Broken.stmat"));

		// Null clears a map.
		RunCommandToCompletion(context, commands, "material.set", { { "material", created["asset"] }, { "properties", { { "BaseColorMap", nullptr } } } });
		const nlohmann::json cleared = RunCommandToCompletion(context, commands, "material.get", { { "material", "Materials/Checker.stmat" } })["values"]["BaseColorMap"];
		CHECK((cleared.is_null() || cleared == "0000000000000000"));
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

		// The window's commands change what the person sees, not the project: replays leave them out (Editor.Journal checks
		// the registry's own commands).
		for (const char* name : { "editor.showDialog", "editor.showPanel" })
		{
			CAPTURE(name);
			const EditorCommand* command = editor.Layer->GetCommands().Find(name);
			REQUIRE(command);
			CHECK(command->Kind == EditorCommandKind::Session);
			CHECK(command->Results == EditorCommandResults::Content);
			CHECK_FALSE(command->MayDefer);
		}
		// A picture of the window changes nothing, and answers frames later.
		const EditorCommand* screenshot = editor.Layer->GetCommands().Find("editor.screenshot");
		REQUIRE(screenshot);
		CHECK(screenshot->Kind == EditorCommandKind::Query);
		CHECK(screenshot->Results == EditorCommandResults::Content);
		CHECK(screenshot->MayDefer);
	}

	TEST_CASE("editor.screenshot reads back the window or a panel as a later frame drew it, and fails without side effects")
	{
		HarnessEditor editor({}, WithFeatureProject("Screenshot"));
		editor.Frames(2);
		REQUIRE(editor.Context().HasProject());
		const std::filesystem::path directory = CreateTemporaryDirectory("ScreenshotFiles");
		const auto execute = [&](const nlohmann::json& parameters)
		{
			return editor.Layer->GetCommands().Execute(editor.Context(), "editor.screenshot", parameters);
		};
		// Through the runner, frames until it answered.
		const auto run = [&](const nlohmann::json& parameters)
		{
			auto completed = std::make_shared<std::optional<EditorCommandResult>>();
			editor.Layer->GetCommandRunner().Run(editor.Context(), editor.Layer->GetCommands(), "editor.screenshot", parameters,
				[completed](const EditorCommandResult& result) { *completed = result; });
			REQUIRE(editor.FramesUntil([&]() { return completed->has_value(); }));
			return **completed;
		};

		// The host's window is set after the layer attached (a window would start native file dialogs).
		CHECK(execute(nlohmann::json::object()).ErrorKind == EditorCommandError::Failed);
		editor.Host->Window = true;
		editor.Frames(1);
		// What cannot be done changes nothing and asks for no readback.
		CHECK(execute({ { "panel", "Nope" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(execute({ { "path", FileSystem::ToUTF8(directory / "Shot.txt") } }).ErrorKind == EditorCommandError::InvalidParameters);
		REQUIRE(FileSystem::WriteText(directory / "Existing.png", "kept"));
		CHECK(execute({ { "path", FileSystem::ToUTF8(directory / "Existing.png") } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(*FileSystem::ReadText(directory / "Existing.png") == "kept");
		editor.Layer->GetPanels().SetOpen(EditorPanels::c_Performance, false);
		const EditorCommandResult closed = execute({ { "panel", "Performance" } });
		CHECK(closed.ErrorKind == EditorCommandError::Failed);
		CHECK(closed.Error.find("is not open") != std::string::npos);
		CHECK(editor.Host->WindowReadbacks.empty());

		// The whole window: read back at the end of the second frame's UI, then encoded and saved on job threads.
		const std::filesystem::path windowFile = directory / "Shots" / "Window.png";
		const uint64_t before = editor.Host->FrameCount;
		const EditorCommandResult whole = run({ { "path", FileSystem::ToUTF8(windowFile) } });
		REQUIRE_MESSAGE(whole.Success, whole.Error);
		REQUIRE(editor.Host->WindowReadbacks.size() == 1);
		CHECK(editor.Host->WindowReadbacks[0] == glm::uvec4(0));
		CHECK(editor.Host->FrameCount - before >= 2);
		CHECK(whole.Value["width"] == 1600);
		CHECK(whole.Value["height"] == 900);
		CHECK(whole.Value["panel"].is_null());
		CHECK(FileSystem::FromUTF8(whole.Value["path"].get<std::string>()) == windowFile);
		const std::optional<std::vector<uint8_t>> saved = FileSystem::ReadBytes(windowFile);
		REQUIRE(saved);
		CHECK(*Base64::Decode(whole.Value["Image"]["Data"].get<std::string>()) == *saved);
		const std::optional<ReadbackImage> decoded = ImageReader::DecodeRGBA8(*saved);
		REQUIRE(decoded);
		CHECK(decoded->Width == 1600);
		CHECK(decoded->Height == 900);
		CHECK(decoded->Pixels[0] == 40);
		CHECK(decoded->Pixels[1] == 90);
		CHECK(decoded->Pixels[2] == 160);

		// A panel ("Scene" names the Scene view): its window's rectangle as the frame that was read back drew it.
		editor.Run("editor.showPanel", { { "panel", "Inspector" } });
		const EditorCommandResult inspector = run({ { "panel", "Inspector" } });
		REQUIRE_MESSAGE(inspector.Success, inspector.Error);
		const std::optional<EditorPanelRegistry::WindowRect> rect = editor.Layer->GetPanels().GetShownWindowRect(EditorPanels::c_Inspector);
		REQUIRE(rect);
		const glm::uvec4 region = editor.Host->WindowReadbacks.back();
		CHECK(region.x == static_cast<uint32_t>(std::floor(rect->Min.x)));
		CHECK(region.y == static_cast<uint32_t>(std::floor(rect->Min.y)));
		CHECK(region.z == static_cast<uint32_t>(std::ceil(rect->Max.x)) - region.x);
		CHECK(region.w == static_cast<uint32_t>(std::ceil(rect->Max.y)) - region.y);
		CHECK(inspector.Value["panel"] == "Inspector");
		CHECK(inspector.Value["width"] == region.z);
		CHECK(inspector.Value["height"] == region.w);
		CHECK(inspector.Value["region"]["x"] == region.x);
		CHECK(inspector.Value["region"]["y"] == region.y);
		CHECK_FALSE(inspector.Value.contains("path"));
		const EditorCommandResult scene = run({ { "panel", "Scene" } });
		REQUIRE_MESSAGE(scene.Success, scene.Error);
		CHECK(scene.Value["panel"] == EditorPanels::c_Viewport);

		// A panel behind another tab of its dock node is not shown: the picture fails, naming the way to show it.
		editor.Run("editor.showPanel", { { "panel", "AssetInspector" } });
		editor.Frames(6);
		REQUIRE_FALSE(editor.Layer->GetPanels().GetShownWindowRect(EditorPanels::c_Inspector).has_value());
		const size_t readbacks = editor.Host->WindowReadbacks.size();
		const EditorCommandResult hidden = run({ { "panel", "Inspector" } });
		CHECK_FALSE(hidden.Success);
		CHECK(hidden.Error.find("not shown") != std::string::npos);
		CHECK(editor.Host->WindowReadbacks.size() == readbacks);

		// A readback that fails fails the command, and nothing is written.
		editor.Host->ReadbackError = "the device was lost";
		const EditorCommandResult failed = run({ { "path", FileSystem::ToUTF8(directory / "Failed.png") } });
		CHECK_FALSE(failed.Success);
		CHECK(failed.Error.find("the device was lost") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(directory / "Failed.png"));
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

	TEST_CASE("editor.showDialog opens the script dialogs: New Script for a project's scripts, External Editor's preferences without one too")
	{
		{
			HarnessEditor editor;
			editor.Layer->DismissLauncher();
			editor.Frames(2);
			// Scripts belong to a project; the program that opens them is the user's.
			CHECK(editor.Layer->GetCommands().Execute(editor.Context(), "editor.showDialog", { { "dialog", "newScript" } }).ErrorKind
				== EditorCommandError::Failed);
			editor.Frames(2);
			CHECK_FALSE(editor.Layer->GetNewScriptDialog().IsOpen());
			const nlohmann::json shown = editor.Run("editor.showDialog", { { "dialog", "externalEditor" } });
			CHECK(shown["dialog"] == "externalEditor");
			CHECK(shown["section"] == "Editor.ExternalEditor");
			editor.Frames(2);
			CHECK(editor.Layer->GetPreferencesDialog().IsOpen());
			CHECK(editor.Layer->GetPreferencesDialog().GetSelected() == "Editor.ExternalEditor");
		}
		{
			HarnessEditor editor({}, WithFeatureProject("ShowScriptDialogs"));
			editor.Frames(2);
			REQUIRE(editor.Context().HasProject());
			editor.Run("editor.showDialog", { { "dialog", "newScript" } });
			editor.Frames(2);
			CHECK(editor.Layer->GetNewScriptDialog().IsOpen());
			CHECK_FALSE(editor.Layer->GetPreferencesDialog().IsOpen());
		}
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
