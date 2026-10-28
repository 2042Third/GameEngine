#include <doctest/doctest.h>

#include "Editor/EditorContext.h"
#include "Editor/EditorPreferences.h"
#include "Editor/HarnessEditor.h"
#include "Editor/ImGuiHarness.h"
#include "Editor/UndoStack.h"
#include "Renderer/QualityTestModel.h"
#include "TestHelpers.h"
#include "UI/GraphicsQualityPage.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Layer.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Project/Project.h>
#include <Strata/Renderer/RendererQuality.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr const char* c_Prefix = "ProjectSettings.Renderer.Quality.";

	std::string PageKey(std::string_view name)
	{
		return std::string(c_Prefix) + std::string(name);
	}

	bool IsDrawn(std::string_view key)
	{
		return UI::ItemProbe::Find(key).has_value();
	}

	std::string TextOf(std::string_view key)
	{
		const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(key);
		REQUIRE_MESSAGE(item.has_value(), "No ", std::string(key), " in the last frame");
		return item->Text;
	}

	ImU32 ColorOf(std::string_view key)
	{
		const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(key);
		REQUIRE_MESSAGE(item.has_value(), "No ", std::string(key), " in the last frame");
		return item->Color;
	}

	ImU32 AccentMuted()
	{
		return ImGui::ColorConvertFloat4ToU32(UI::GetThemeColors().AccentMuted);
	}

	void CreateProject(HarnessEditor& editor, const std::string& name)
	{
		const std::filesystem::path directory = CreateTemporaryDirectory(name) / "Game";
		editor.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" }, { "template", "basic3d" } });
		editor.Frames(2);
	}

	// Opens Project Settings at Graphics Quality and waits for its table.
	void OpenGraphicsQuality(HarnessEditor& editor)
	{
		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		REQUIRE(editor.Click("Menu.File.ProjectSettings"));
		REQUIRE(editor.FramesUntil([]() { return IsDrawn("ProjectSettings.Page.Renderer.Quality"); }));
		REQUIRE(editor.Click("ProjectSettings.Page.Renderer.Quality"));
		REQUIRE(editor.FramesUntil([]() { return IsDrawn(PageKey("Row.ShadowResolution")); }));
	}

	const nlohmann::json* StoredQuality(HarnessEditor& editor)
	{
		return editor.Context().GetProject()->GetSettings().FindSection(RendererQuality::c_SectionId);
	}

	// A layer that draws one page in a window, for pages no registered section shows (a test section's).
	class PageLayer final : public Layer
	{
	public:
		PageLayer(SettingsPage& page, EditorPanelContext& context)
			: Layer("PageLayer"), m_Page(page), m_Context(context)
		{
		}

		void OnImGuiRender() override
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(1200.0f, 700.0f));
			ImGui::Begin("Page", nullptr, ImGuiWindowFlags_NoSavedSettings);
			m_Page.Draw(m_Context, 1100.0f);
			ImGui::End();
		}
	private:
		SettingsPage& m_Page;
		EditorPanelContext& m_Context;
	};

}

TEST_SUITE("Editor.UI.GraphicsQuality")
{
	TEST_CASE("The page replaces the generic page and shows a column per preset with the chosen one highlighted")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "GraphicsQualityPage");
		OpenGraphicsQuality(editor);

		// Not the generic page's fields: the page's own table, a row per step-0 knob, a cell per preset and Custom's widget.
		CHECK_FALSE(IsDrawn(PageKey("ShadowCascades")));
		for (const RendererQuality::RowTraits& row : RendererQuality::GetRowTraits())
		{
			const std::string field(row.Field);
			CAPTURE(field);
			CHECK(IsDrawn(PageKey("Row." + field)));
			for (const char* preset : { "Low", "Medium", "High", "Ultra" })
				CHECK(IsDrawn(PageKey("Cell." + field + "." + preset)));
			CHECK(IsDrawn(PageKey("Custom." + field)));
		}
		CHECK(RendererQuality::GetRowTraits().size() == 7);
		// The project's preset, High (the default), is chosen: its header and cells on the accent's highlight.
		for (const char* preset : { "Low", "Medium", "High", "Ultra", "Custom" })
			CHECK(IsDrawn(PageKey(std::string("Preset.") + preset)));
		CHECK(ColorOf(PageKey("Header.High")) == AccentMuted());
		CHECK(ColorOf(PageKey("Cell.ShadowCascades.High")) == AccentMuted());
		CHECK(ColorOf(PageKey("Cell.ShadowCascades.Low")) == 0);
		CHECK(ColorOf(PageKey("Header.Ultra")) == 0);
		// The automatic resolutions name the sizes they resolve to.
		CHECK(TextOf(PageKey("Cell.ShadowResolution.Low")) == "1024");
		CHECK(TextOf(PageKey("Cell.ShadowResolution.High")) == "Auto (2048 / 4096)");
		CHECK(TextOf(PageKey("Cell.ShadowResolution.Ultra")) == "Auto fine (4096 / 8192)");
		CHECK(TextOf(PageKey("Cell.LODErrorPixels.Ultra")) == "0.5");
		CHECK(TextOf(PageKey("Cell.MaxShadowedLocalLights.Low")) == "4");
	}

	TEST_CASE("The memory rows match the estimates and the GPU line names the Scene view's stages")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "GraphicsQualityMemory");
		OpenGraphicsQuality(editor);
		// Cascades plus the local lights' atlas of one view (RowTraits::MemoryBytes).
		CHECK(TextOf(PageKey("Memory1080.Low")) == "28 MiB");
		CHECK(TextOf(PageKey("Memory1080.Medium")) == "128 MiB");
		CHECK(TextOf(PageKey("Memory1080.High")) == "128 MiB");
		CHECK(TextOf(PageKey("Memory2160.High")) == "320 MiB");
		CHECK(TextOf(PageKey("Memory2160.Ultra")) == "1.25 GiB");
		// Custom's column under High shows where a switch to Custom starts: High's values.
		CHECK(TextOf(PageKey("Memory2160.Custom")) == "320 MiB");
		CHECK(ColorOf(PageKey("Memory2160.High")) == AccentMuted());
		// The harness renders nothing on a GPU.
		CHECK(TextOf(PageKey("GPU")) == "GPU times appear while the Scene view renders");
	}

	TEST_CASE("Choosing a preset is one project.setSettings step and keeps the rows' stored values")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "GraphicsQualityPreset");
		editor.Run("project.setSettings", { { "section", RendererQuality::c_SectionId }, { "value", { { "Preset", "Custom" }, { "ShadowCascades", 2 } } } });
		OpenGraphicsQuality(editor);
		CHECK(ColorOf(PageKey("Header.Custom")) == AccentMuted());
		const size_t steps = editor.Context().GetUndoStack().GetCount();

		REQUIRE(editor.Click(PageKey("Preset.Ultra")));
		editor.Frames(2);
		REQUIRE(StoredQuality(editor));
		CHECK((*StoredQuality(editor))["Preset"] == "Ultra");
		CHECK((*StoredQuality(editor))["ShadowCascades"] == 2);
		CHECK(editor.Context().GetUndoStack().GetCount() == steps + 1);
		CHECK_FALSE(editor.Context().IsSceneModified());
		CHECK(ColorOf(PageKey("Header.Ultra")) == AccentMuted());
		// Custom's column keeps showing the stored tuning (two cascades of High's), which comes back when Custom is chosen.
		CHECK(TextOf(PageKey("Memory2160.Custom")) == "192 MiB");

		// One undo reverts it.
		editor.Run("edit.undo");
		editor.Frames(2);
		CHECK((*StoredQuality(editor))["Preset"] == "Custom");
		CHECK(ColorOf(PageKey("Header.Custom")) == AccentMuted());

		// Custom again: the tuning, kept.
		REQUIRE(editor.Click(PageKey("Preset.Low")));
		editor.Frames(2);
		REQUIRE(editor.Click(PageKey("Preset.Custom")));
		editor.Frames(2);
		CHECK((*StoredQuality(editor))["Preset"] == "Custom");
		CHECK((*StoredQuality(editor))["ShadowCascades"] == 2);
	}

	TEST_CASE("Editing Custom's cell under High switches to Custom from High's values in one step")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "GraphicsQualityCustomEdit");
		OpenGraphicsQuality(editor);
		const size_t steps = editor.Context().GetUndoStack().GetCount();

		REQUIRE(editor.Click(PageKey("Custom.ShadowResolution")));
		editor.Frames(1);
		REQUIRE(editor.Click(PageKey("Custom.ShadowResolution.4096")));
		editor.Frames(2);
		CHECK(editor.Context().GetUndoStack().GetCount() == steps + 1);
		const SettingsSection& section = *editor.Context().GetSettingsRegistry().Find(RendererQuality::c_SectionId);
		nlohmann::json expected = RendererQuality::MakeCustomStart(section.GetDefault(), RendererQuality::GetOptionModel(section));
		expected["ShadowResolution"] = "4096";
		REQUIRE(StoredQuality(editor));
		CHECK(*StoredQuality(editor) == expected);
		CHECK(ColorOf(PageKey("Header.Custom")) == AccentMuted());
	}

	TEST_CASE("Reset to Defaults resets the section")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "GraphicsQualityReset");
		OpenGraphicsQuality(editor);
		REQUIRE(editor.Click(PageKey("Preset.Low")));
		editor.Frames(2);
		REQUIRE(editor.Context().GetProject()->GetSettings().IsStored(RendererQuality::c_SectionId));
		REQUIRE(editor.Click(PageKey("Reset")));
		editor.Frames(2);
		CHECK_FALSE(editor.Context().GetProject()->GetSettings().IsStored(RendererQuality::c_SectionId));
		CHECK(ColorOf(PageKey("Header.High")) == AccentMuted());
	}

	TEST_CASE("Rows the presets leave alone appear under Players' choices")
	{
		// A section of the test's own with a row that does not follow the preset (the engine's rows all follow it so far).
		const QualityTestModel test;
		SettingsRegistry registry;
		EditorPreferences::RegisterSections(registry);
		REQUIRE(registry.Register(test.Section));
		registry.Freeze();
		EditorOptions options;
		options.Settings = &registry;
		HarnessEditor editor({}, options);
		editor.Frames(1);
		CreateProject(editor, "GraphicsQualityChoices");

		RendererQuality::OptionModel model = test.GetModel();
		model.Section = registry.Find(QualityTestModel::c_SectionId);
		GraphicsQualityPage page(model);
		EditorPanelContext panelContext { editor.Context(), editor.Layer->GetCommands(), editor.Layer->GetCommandRunner() };
		PageLayer layer(page, panelContext);
		editor.Harness.Frame(layer);
		editor.Harness.Frame(layer);

		const std::string prefix = "ProjectSettings.Test.Quality.";
		CHECK(UI::ItemProbe::Find(prefix + "Row.Detail").has_value());
		CHECK_FALSE(UI::ItemProbe::Find(prefix + "Row.Comfort").has_value());
		CHECK(UI::ItemProbe::Find(prefix + "Choice.Comfort").has_value());
		CHECK(UI::ItemProbe::Find(prefix + "Cell.Detail.Ultra")->Text == "4");
		// A test model's rows have no memory of their own.
		CHECK_FALSE(UI::ItemProbe::Find(prefix + "Memory1080.High").has_value());

		// A choice's edit sets the field alone: the preset stays.
		REQUIRE(editor.Harness.ClickItem(prefix + "Choice.Comfort", layer));
		editor.Harness.Frame(layer);
		const nlohmann::json* value = editor.Context().GetProject()->GetSettings().FindSection(QualityTestModel::c_SectionId);
		REQUIRE(value);
		CHECK((*value)["Comfort"] == false);
		CHECK((*value)["Preset"] == "High");

		// A row that follows the preset, edited under High: Custom from High's values, the choice kept.
		REQUIRE(editor.Harness.ClickItem(prefix + "Preset.Custom", layer));
		editor.Harness.Frame(layer);
		value = editor.Context().GetProject()->GetSettings().FindSection(QualityTestModel::c_SectionId);
		REQUIRE(value);
		CHECK(*value == nlohmann::json { { "Preset", "Custom" }, { "Detail", 3 }, { "Comfort", false } });
	}
}
