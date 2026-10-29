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
#include <Strata/Runtime/GameGraphicsService.h>

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

	// The tooltip of Custom's cell of a row, as the page records it while it shows (written only then).
	std::string HoverCustomTooltip(HarnessEditor& editor, std::string_view field)
	{
		const std::optional<UI::ItemProbe::Item> cell = UI::ItemProbe::Find(PageKey("Custom." + std::string(field)));
		REQUIRE(cell.has_value());
		editor.Harness.MoveMouse(cell->GetCenter());
		editor.Frames(8, 0.1f);
		return TextOf(PageKey("CustomTooltip"));
	}

	// Scrolls the page to its end, where Reset to Defaults is: a row per field of Renderer.Quality puts it below the
	// window's bottom in the harness's 1600x900 display, where a click reaches nothing.
	void ScrollPageToEnd(HarnessEditor& editor)
	{
		const std::optional<UI::ItemProbe::Item> row = UI::ItemProbe::Find(PageKey("Row.ShadowResolution"));
		REQUIRE(row.has_value());
		editor.Harness.MoveMouse(row->GetCenter());
		editor.Frames(1);
		editor.Harness.ScrollMouse(-50.0f); // Far past the end: the page's scroll stops there
		editor.Frames(2);
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

		// Not the generic page's fields: the page's own table, a row per knob, a cell per preset and Custom's widget; the rows
		// the presets leave alone under Players' choices instead.
		CHECK_FALSE(IsDrawn(PageKey("ShadowCascades")));
		size_t choices = 0;
		for (const RendererQuality::RowTraits& row : RendererQuality::GetRowTraits())
		{
			const std::string field(row.Field);
			CAPTURE(field);
			if (!row.FollowsPreset)
			{
				choices++;
				CHECK(IsDrawn(PageKey("Choice." + field)));
				CHECK_FALSE(IsDrawn(PageKey("Row." + field)));
				continue;
			}
			CHECK(IsDrawn(PageKey("Row." + field)));
			for (const char* preset : { "Low", "Medium", "High", "Ultra" })
				CHECK(IsDrawn(PageKey("Cell." + field + "." + preset)));
			CHECK(IsDrawn(PageKey("Custom." + field)));
		}
		// Shadows and detail (7), the sun's filter taps and contact shadows, the ambient occlusion's samples, the sky's reflection
		// map, the screen-space reflections, the motion blur's samples; the players' anti-aliasing, motion blur, depth of field,
		// film grain and chromatic aberration.
		CHECK(RendererQuality::GetRowTraits().size() == 18);
		CHECK(choices == 5);
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
		// Cascades plus the local lights' atlas of one view, the sky's prefiltered cube (4 MiB at 256 per face, 16 at 512), and the
		// screen-space reflections' and the motion blur's targets but at Low, which turns both off (RowTraits::MemoryBytes; the
		// reflections': Checkerboard 19.8 MiB at 1920 x 1080 and 79.1 MiB at 3840 x 2160, Spatial 58.0 and 232.0 MiB; the motion
		// blur's: 24.2 and 96.9 MiB). Whole MiB, rounded down.
		CHECK(TextOf(PageKey("Memory1080.Low")) == "31 MiB");
		CHECK(TextOf(PageKey("Memory1080.Medium")) == "175 MiB");
		CHECK(TextOf(PageKey("Memory1080.High")) == "226 MiB");
		CHECK(TextOf(PageKey("Memory2160.High")) == "664 MiB");
		CHECK(TextOf(PageKey("Memory2160.Ultra")) == "1.59 GiB");
		// Custom's column under High shows where a switch to Custom starts: High's values.
		CHECK(TextOf(PageKey("Memory2160.Custom")) == "664 MiB");
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
		// Custom's column keeps showing the stored tuning (two cascades of High's, 128 MiB, with the atlas, the sky's map and the
		// reflections' and motion blur's targets), which comes back when Custom is chosen, and an edit there starts from it, as
		// its tooltip says.
		CHECK(TextOf(PageKey("Memory2160.Custom")) == "536 MiB");
		CHECK(HoverCustomTooltip(editor, "ShadowCascades") == "Editing a value switches to Custom, starting from the Custom values chosen earlier, "
			"which this column shows");

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
		// Nothing tuned yet: an edit starts from the preset's values.
		CHECK(HoverCustomTooltip(editor, "ShadowCascades") == "Editing a value switches to Custom, starting from High's values");

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
		ScrollPageToEnd(editor);
		REQUIRE(editor.Click(PageKey("Reset")));
		editor.Frames(2);
		CHECK_FALSE(editor.Context().GetProject()->GetSettings().IsStored(RendererQuality::c_SectionId));
		CHECK(ColorOf(PageKey("Header.High")) == AccentMuted());
	}

	TEST_CASE("Rows the presets leave alone appear under Players' choices")
	{
		// A section of the test's own with a row that does not follow the preset (a Bool, which the engine's rows are not).
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

	TEST_CASE("Through the scene's camera the quality chip names the game's quality, the player's choice while playing")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "GraphicsQualityChipGame");
		const ImU32 accent = UI::ToColorU32(UI::GetThemeColors().Accent);
		// The editor camera at the view's own preset: lit, named after it.
		editor.Run("viewport.setSettings", { { "quality", "ultra" } });
		editor.Frames(2);
		CHECK(TextOf("Viewport.Quality") == "Ultra");
		CHECK(ColorOf("Viewport.Quality") == accent);
		// The scene's camera draws at the game's quality (GetSceneViewQuality): the project's High, the view's Ultra on but muted.
		editor.Run("viewport.setSettings", { { "pilotGameCamera", true } });
		editor.Frames(2);
		CHECK(TextOf("Viewport.Quality") == "High");
		CHECK(ColorOf("Viewport.Quality") != accent);
		// While playing, the player's choice.
		editor.Run("play.start", { { "build", false } });
		REQUIRE(editor.Context().GetGameGraphics().SetOption("Preset", "Low"));
		editor.Frames(2);
		CHECK(TextOf("Viewport.Quality") == "Low");
		editor.Run("play.stop");
		editor.Frames(2);
		CHECK(TextOf("Viewport.Quality") == "High");
		// The editor camera again: the view's own.
		editor.Run("viewport.setSettings", { { "pilotGameCamera", false } });
		editor.Frames(2);
		CHECK(TextOf("Viewport.Quality") == "Ultra");
		CHECK(ColorOf("Viewport.Quality") == accent);
	}

	TEST_CASE("The players' banner shows what their options set and its button resets them")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "GraphicsQualityPlayers");
		OpenGraphicsQuality(editor);
		CHECK_FALSE(IsDrawn(PageKey("PlayerOptions")));
		CHECK_FALSE(IsDrawn(PageKey("Row.Preset.Player")));

		// While playing, a game's options menu sets the players' choices: the banner names them, marked on the page.
		const std::filesystem::path file = *editor.Context().GetGameSaveDirectory() / GraphicsOptionsFile::c_FileName;
		editor.Run("play.start", { { "build", false } });
		GameGraphicsService& graphics = editor.Context().GetGameGraphics();
		REQUIRE(graphics.SetOption("Preset", "Low"));
		REQUIRE(graphics.SetOption("ShadowCascades", 2));
		// A number as the table writes it: a float widened to a double (as a 32-bit float reaches it) shows as 0.6.
		REQUIRE(graphics.SetOption("LODErrorPixels", static_cast<double>(0.6f)));
		REQUIRE(editor.FramesUntil([]() { return IsDrawn(PageKey("PlayerOptions")); }));
		CHECK(TextOf(PageKey("PlayerOptions")).find("The running game's player chose: ") != std::string::npos);
		CHECK(TextOf(PageKey("PlayerOptions")).find("LODErrorPixels 0.6,") != std::string::npos);
		CHECK(TextOf(PageKey("PlayerOptions")).find("0.600") == std::string::npos);
		CHECK(IsDrawn(PageKey("Row.ShadowCascades.Player")));
		// It saves them and play stops: the next session starts with them.
		REQUIRE(graphics.SaveOptions());
		editor.Run("play.stop");
		REQUIRE(editor.FramesUntil([]() { return TextOf(PageKey("PlayerOptions")).find("saved options") != std::string::npos; }));
		CHECK(TextOf(PageKey("PlayerOptions")).find("The players' saved options replace the project's in play: ") != std::string::npos);
		CHECK(TextOf(PageKey("PlayerOptions")).find("ShadowCascades 2") != std::string::npos);
		CHECK(IsDrawn(PageKey("Row.Preset.Player")));
		CHECK(IsDrawn(PageKey("Row.ShadowCascades.Player")));

		// The button runs graphics.resetPlayerOptions: the file goes, and so do the banner and the pills.
		REQUIRE(editor.Click(PageKey("PlayerOptions.Reset")));
		editor.Frames(2);
		CHECK_FALSE(FileSystem::Exists(file));
		CHECK_FALSE(IsDrawn(PageKey("PlayerOptions")));
		CHECK_FALSE(IsDrawn(PageKey("Row.Preset.Player")));
	}
}

TEST_SUITE("Editor.UI.Inspector")
{
	TEST_CASE("Post Process draws a heading per section")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "InspectorSections");
		const std::string entity = editor.Run("entity.create", { { "name", "Look" }, { "components", { { "PostProcess", nlohmann::json::object() } } } })["id"].get<std::string>();
		editor.Run("selection.set", { { "entities", { entity } } });
		REQUIRE(editor.FramesUntil([]() { return IsDrawn("Inspector.PostProcess.Section.Lens"); }));
		// In the order of their first property, each under its heading.
		float previous = -1.0f;
		for (const char* section : { "Tone and color", "Exposure", "Bloom", "Ambient occlusion", "Anti-aliasing", "Reflections", "Lens" })
		{
			CAPTURE(section);
			const std::optional<UI::ItemProbe::Item> heading = UI::ItemProbe::Find(std::string("Inspector.PostProcess.Section.") + section);
			REQUIRE(heading.has_value());
			CHECK(heading->Text == section);
			CHECK(heading->Min.y > previous);
			previous = heading->Min.y;
		}
		const std::optional<UI::ItemProbe::Item> exposure = UI::ItemProbe::Find("Inspector.PostProcess.Exposure.Label");
		const std::optional<UI::ItemProbe::Item> bloomHeading = UI::ItemProbe::Find("Inspector.PostProcess.Section.Bloom");
		REQUIRE(exposure.has_value());
		CHECK(exposure->Min.y > UI::ItemProbe::Find("Inspector.PostProcess.Section.Exposure")->Min.y);
		CHECK(exposure->Min.y < bloomHeading->Min.y);
	}

	TEST_CASE("The Post Process says how its screen-space reflections are traced and links to Graphics Quality")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "InspectorReflections");
		const std::string post = editor.Run("entity.create", { { "name", "Post" }, { "components", { { "PostProcess",
			{ { "ScreenSpaceReflections", true } } } } } })["id"].get<std::string>();
		editor.Run("selection.set", { { "entities", { post } } });
		REQUIRE(editor.FramesUntil([]() { return IsDrawn("Inspector.PostProcess.ReflectionsQuality"); }));
		// Without a GPU the Scene view draws no frame: the project's quality, High (Reflections Spatial) by default.
		CHECK(TextOf("Inspector.PostProcess.ReflectionsQuality") == "Screen-space reflections: every pixel, with any anti-aliasing (Graphics Quality: High)");
		const auto preset = [&editor](const char* name)
		{
			editor.Run("project.setSettings", { { "section", RendererQuality::c_SectionId }, { "value", { { "Preset", name } } } });
			editor.Frames(2);
		};
		preset("Medium");
		CHECK(TextOf("Inspector.PostProcess.ReflectionsQuality") == "Screen-space reflections: half resolution, only with TAA (Graphics Quality: Medium)");
		preset("Ultra");
		CHECK(TextOf("Inspector.PostProcess.ReflectionsQuality")
			== "Screen-space reflections: every pixel, longer rays, with any anti-aliasing (Graphics Quality: Ultra)");
		preset("Low");
		CHECK(TextOf("Inspector.PostProcess.ReflectionsQuality") == "Screen-space reflections: none (Graphics Quality: Low, Reflections Off)");
		// A Post Process that does not ask for them.
		editor.Run("component.set", { { "entity", post }, { "component", "PostProcess" }, { "values", { { "ScreenSpaceReflections", false } } } });
		editor.Frames(2);
		CHECK(TextOf("Inspector.PostProcess.ReflectionsQuality") == "No screen-space reflections: Screen Space Reflections is off");
		// The line sits under the Post Process's many properties, below the Inspector's bottom: scroll it to its end first.
		const std::optional<UI::ItemProbe::Item> heading = UI::ItemProbe::Find("Inspector.PostProcess.Section.Exposure");
		REQUIRE(heading.has_value());
		editor.Harness.MoveMouse(heading->GetCenter());
		editor.Frames(1);
		editor.Harness.ScrollMouse(-200.0f); // Far past the end: the Inspector's scroll stops there
		editor.Frames(2);
		REQUIRE(editor.Click("Inspector.PostProcess.ReflectionsQuality.Open"));
		CHECK(editor.FramesUntil([]() { return IsDrawn(PageKey("Row.ShadowResolution")); }));
	}

	TEST_CASE("The sun shows its shadow maps and links to Graphics Quality")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "InspectorSun");
		const std::string sun = editor.Run("entity.create", { { "name", "Sun" }, { "components", { { "DirectionalLight", nlohmann::json::object() } } } })["id"].get<std::string>();
		editor.Run("selection.set", { { "entities", { sun } } });
		REQUIRE(editor.FramesUntil([]() { return IsDrawn("Inspector.DirectionalLight.ShadowQuality"); }));
		// Without a GPU the Scene view draws no frame: the project's quality (High) resolved for 1080 rows.
		CHECK(TextOf("Inspector.DirectionalLight.ShadowQuality") == "Shadow maps: 4 x 2048 at 1080 rows (Graphics Quality: High)");
		CHECK(IsDrawn("Inspector.DirectionalLight.Section.Shadows"));
		editor.Run("project.setSettings", { { "section", RendererQuality::c_SectionId }, { "value", { { "Preset", "Low" } } } });
		editor.Frames(2);
		CHECK(TextOf("Inspector.DirectionalLight.ShadowQuality") == "Shadow maps: 3 x 1024 at 1080 rows (Graphics Quality: Low)");
		// A sun that casts no shadow has no shadow maps.
		editor.Run("component.set", { { "entity", sun }, { "component", "DirectionalLight" }, { "values", { { "CastShadows", false } } } });
		editor.Frames(2);
		CHECK(TextOf("Inspector.DirectionalLight.ShadowQuality") == "No shadow maps: Cast Shadows is off");
		REQUIRE(editor.Click("Inspector.DirectionalLight.ShadowQuality.Open"));
		CHECK(editor.FramesUntil([]() { return IsDrawn(PageKey("Row.ShadowResolution")); }));
	}

	TEST_CASE("With nothing selected the Inspector links to Graphics Quality")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CreateProject(editor, "InspectorNothing");
		editor.Run("selection.set", { { "entities", nlohmann::json::array() } });
		REQUIRE(editor.FramesUntil([]() { return IsDrawn("Inspector.GraphicsQuality"); }));
		CHECK(TextOf("Inspector.GraphicsQuality") == "High");
		REQUIRE(editor.Click("Inspector.GraphicsQuality.Open"));
		CHECK(editor.FramesUntil([]() { return IsDrawn(PageKey("Row.ShadowResolution")); }));
	}
}
