#include <doctest/doctest.h>

#include "Editor/ImGuiHarness.h"
#include "EditorLayer.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "TestHelpers.h"
#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Events/ApplicationEvent.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// An EditorLayer drawn by an ImGuiHarness through a fake host: no automation, no file watchers, no native dialogs.
	struct HarnessEditor
	{
		Ref<FakeEditorHost::State> Host = CreateRef<FakeEditorHost::State>();
		ImGuiHarness Harness;
		Scope<EditorLayer> Layer;

		explicit HarnessEditor(const ImGuiHarness::Specification& specification = {}, EditorOptions options = {})
			: Harness(specification)
		{
			options.EnableAutomation = false;
			options.WatchFiles = false;
			Host->UIScale = specification.ContentScale;
			Layer = CreateScope<EditorLayer>(options, CreateScope<FakeEditorHost>(Host));
			Layer->OnAttach();
		}

		~HarnessEditor()
		{
			Layer->OnDetach();
		}

		HarnessEditor(const HarnessEditor&) = delete;
		HarnessEditor& operator=(const HarnessEditor&) = delete;

		// Frames of the application: the host's clock and frame count advance with them.
		void Frames(int count, float seconds = ImGuiHarness::c_DeltaTime)
		{
			for (int frame = 0; frame < count; frame++)
			{
				Host->Time += seconds;
				Harness.Frame(*Layer, seconds);
				Host->FrameCount++;
			}
		}

		EditorContext& Context() { return Layer->GetContext(); }

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			const EditorCommandResult result = Layer->GetCommands().Execute(Context(), name, parameters);
			INFO(std::string(name), ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}
	};

	EditorOptions WithFeatureProject(const std::string& directoryName)
	{
		EditorOptions options;
		options.ProjectPath = CopyFeatureProject(CreateTemporaryDirectory(directoryName) / "Project");
		return options;
	}

}

TEST_SUITE("Editor.UI")
{
	TEST_CASE("The editor's fonts: Inter by default, SemiBold and Mono, with every icon")
	{
		ImGuiHarness harness;
		REQUIRE(harness.AreFontsLoaded());
		const ImGuiIO& io = ImGui::GetIO();
		ImFont* regular = UI::EditorFonts::Get(UI::EditorFont::Regular);
		ImFont* semiBold = UI::EditorFonts::Get(UI::EditorFont::SemiBold);
		ImFont* mono = UI::EditorFonts::Get(UI::EditorFont::Mono);
		REQUIRE(regular);
		REQUIRE(semiBold);
		REQUIRE(mono);
		CHECK(io.FontDefault == regular);
		CHECK(std::string_view(regular->GetDebugName()) == "Inter Regular");
		CHECK(std::string_view(semiBold->GetDebugName()) == "Inter SemiBold");
		CHECK(std::string_view(mono->GetDebugName()) == "JetBrains Mono");
		// Exactly the editor's fonts: ImGui's built-in one (AddFontDefault) is never added.
		REQUIRE(io.Fonts->Fonts.Size == 3);
		for (const ImFont* font : io.Fonts->Fonts)
			CHECK(std::string_view(font->GetDebugName()).find("Proggy") == std::string_view::npos);

		harness.Frame([&]()
		{
			// A frame draws with Inter at the body size.
			CHECK(ImGui::GetFont() == regular);
			CHECK(ImGui::GetFontSize() == doctest::Approx(UI::GetTextSize(UI::TextSize::Body)));
			UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Display);
			CHECK(ImGui::GetFont() == mono);
			CHECK(ImGui::GetFontSize() == doctest::Approx(UI::GetTextSize(UI::TextSize::Display)));
			ImGui::PopFont();

			// Every icon of UI/Icons.h has a glyph in the UI fonts (the icon font is merged into them), also in the text
			// fonts' own sizes.
			ImFontBaked* body = regular->GetFontBaked(UI::GetTextSize(UI::TextSize::Body));
			ImFontBaked* header = semiBold->GetFontBaked(UI::GetTextSize(UI::TextSize::Title));
			int missing = 0;
			for (const Icons::IconGlyph& icon : Icons::c_All)
			{
				if (!body->FindGlyphNoFallback(static_cast<ImWchar>(icon.Codepoint)) || !header->FindGlyphNoFallback(static_cast<ImWchar>(icon.Codepoint)))
				{
					if (missing++ < 5)
						FAIL_CHECK("No glyph for the icon " << icon.Name);
				}
			}
			CHECK(missing == 0);
			CHECK(Icons::c_All.size() > 1000);
			// Text glyphs come from Inter, not from the icon font.
			CHECK(body->FindGlyphNoFallback('A'));
			CHECK_FALSE(mono->GetFontBaked(UI::GetTextSize(UI::TextSize::Body))->FindGlyphNoFallback(static_cast<ImWchar>(Icons::c_FirstCodepoint)));
		});
		CHECK(harness.GetTextureRequestCount() > 0);
	}

	TEST_CASE("The editor draws its UI headless with the feature project, without id conflicts")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIFeature"));
		REQUIRE(editor.Context().HasProject());
		editor.Frames(3);
		CHECK(editor.Harness.GetHoveredItemIdCount() <= 1);

		// With an entity selected the inspector shows its components; then the mouse goes over the whole window, so every
		// item that reacts to the mouse is hovered once: none shares its id with another (ConfigDebugHighlightIdConflicts).
		const nlohmann::json found = editor.Run("entity.find", { { "name", "Ball" } })["entities"];
		REQUIRE(found.size() == 1);
		editor.Run("selection.set", { { "entities", found } });
		editor.Frames(2);
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

	TEST_CASE("A content scale event restyles the UI and scales its fonts")
	{
		HarnessEditor editor;
		editor.Frames(2);
		const ImGuiStyle before = ImGui::GetStyle();
		CHECK(before.FontScaleDpi == 1.0f);

		WindowContentScaleEvent event(2.0f);
		editor.Harness.DispatchEvent(event, *editor.Layer);
		// Others may react to it too.
		CHECK_FALSE(event.Handled);
		const ImGuiStyle& after = ImGui::GetStyle();
		CHECK(after.FontScaleDpi == 2.0f);
		CHECK(after.FramePadding.x == before.FramePadding.x * 2.0f);
		CHECK(after.FramePadding.y == before.FramePadding.y * 2.0f);
		CHECK(after.ItemSpacing.x == before.ItemSpacing.x * 2.0f);
		CHECK(after.ItemSpacing.y == before.ItemSpacing.y * 2.0f);
		CHECK(after.IndentSpacing == before.IndentSpacing * 2.0f);
		CHECK(after.ScrollbarSize == before.ScrollbarSize * 2.0f);
		// Still the theme, not ImGui's style.
		CHECK(after.Colors[ImGuiCol_CheckMark].x == UI::GetThemeColors().Accent.x);

		editor.Frames(1);
		editor.Harness.Frame([]()
		{
			CHECK(ImGui::GetFontSize() == doctest::Approx(UI::GetTextSize(UI::TextSize::Body) * 2.0f));
		});
	}

	TEST_CASE("editor.quit closes the editor through its host")
	{
		HarnessEditor editor;
		editor.Frames(1);
		CHECK(editor.Host->Running);
		editor.Run("editor.quit");
		editor.Frames(1);
		CHECK_FALSE(editor.Host->Running);
		CHECK(editor.Host->ExitCode == 0);
	}

	TEST_CASE("The widget kit: buttons, chips, pills, cards and dialogs")
	{
		ImGuiHarness harness;
		bool toggle = false;
		int pillClicks = 0;
		int cardClicks = 0;
		int disabledClicks = 0;
		bool modalShown = false;
		bool modalOpen = true;
		const auto draw = [&]()
		{
			ImGui::Begin("Widgets");
			UI::ToggleChip("Test.Chip", Icons::Grid3x3, "Grid", &toggle, "A toggle");
			if (UI::Pill("Test.Pill", Icons::Gauge, "16.7 ms", UI::GetThemeColors().TextSecondary, "A pill"))
				pillClicks++;
			UI::ButtonStyle disabled;
			disabled.Enabled = false;
			if (UI::ToolbarButton("Test.Disabled", Icons::Play, "Disabled", nullptr, disabled))
				disabledClicks++;
			if (UI::Card("Test.Card", Icons::Box, "Empty", "A project with nothing in it", ImVec2(ImGui::GetFontSize() * 12.0f, ImGui::GetFontSize() * 8.0f)))
				cardClicks++;
			UI::IconButton("Test.Twice", Icons::X, nullptr);
			ImGui::PushID("Other");
			UI::IconButton("Test.Twice", Icons::X, nullptr);
			ImGui::PopID();
			if (UI::SectionHeader("Test.Section", "Section"))
				UI::Heading("Heading", UI::TextSize::Display);
			ImGui::End();

			if (!modalShown)
			{
				UI::OpenModal("Test Modal");
				modalShown = true;
			}
			if (UI::BeginModal("Test Modal", "A dialog", &modalOpen))
			{
				UI::DialogButton("Test.Ok", "OK", true);
				UI::EndModal();
			}
		};
		harness.Frame(draw);
		harness.Frame(draw);

		const auto click = [&](std::string_view key)
		{
			const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(key);
			REQUIRE(item.has_value());
			harness.MoveMouse(item->GetCenter());
			harness.Frame(draw);
			harness.SetMouseButton(ImGuiMouseButton_Left, true);
			harness.Frame(draw);
			harness.SetMouseButton(ImGuiMouseButton_Left, false);
			harness.Frame(draw);
		};
		// The modal is open first: its close button ends it.
		CHECK(UI::ItemProbe::Find("Test.Ok").has_value());
		click("Modal.Close");
		CHECK_FALSE(modalOpen);
		harness.Frame(draw);
		CHECK_FALSE(UI::ItemProbe::Find("Test.Ok").has_value());

		click("Test.Chip");
		CHECK(toggle);
		click("Test.Chip");
		CHECK_FALSE(toggle);
		click("Test.Pill");
		CHECK(pillClicks == 1);
		click("Test.Card");
		CHECK(cardClicks == 1);
		click("Test.Disabled");
		CHECK(disabledClicks == 0);
		CHECK_FALSE(UI::ItemProbe::Find("Test.Disabled")->Enabled);
		// A key that names two widgets is marked, so tests cannot click the wrong one.
		CHECK(UI::ItemProbe::Find("Test.Twice")->Duplicate);
		CHECK_FALSE(UI::ItemProbe::Find("Test.Pill")->Duplicate);
		// Sizes follow the font: the pill is as tall as a frame.
		const UI::ItemProbe::Item pill = *UI::ItemProbe::Find("Test.Pill");
		harness.Frame([&]()
		{
			CHECK(pill.Max.y - pill.Min.y == doctest::Approx(ImGui::GetFrameHeight()));
			draw();
		});
	}
}
