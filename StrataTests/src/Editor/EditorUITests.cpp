#include <doctest/doctest.h>

#include "Audio/AudioTestUtils.h"
#include "Editor/ImGuiHarness.h"
#include "EditorLayer.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Panels/ConsolePanel.h"
#include "TestHelpers.h"
#include "UI/EditorFonts.h"
#include "UI/EditorPanelRegistry.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/Log.h>
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

		// iniSettings: imgui.ini contents loaded before the first frame (the editor's saved layout and panel states).
		explicit HarnessEditor(const ImGuiHarness::Specification& specification = {}, EditorOptions options = {}, const std::string& iniSettings = {})
			: Harness(specification)
		{
			options.EnableAutomation = false;
			options.WatchFiles = false;
			Host->UIScale = specification.ContentScale;
			Layer = CreateScope<EditorLayer>(options, CreateScope<FakeEditorHost>(Host));
			Layer->OnAttach();
			if (!iniSettings.empty())
				ImGui::LoadIniSettingsFromMemory(iniSettings.c_str(), iniSettings.size());
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

		bool Click(std::string_view probeKey)
		{
			Host->Time += 3.0 * ImGuiHarness::c_DeltaTime;
			const bool clicked = Harness.ClickItem(probeKey, *Layer);
			Host->FrameCount += 3;
			return clicked;
		}

		EditorContext& Context() { return Layer->GetContext(); }

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			const EditorCommandResult result = Layer->GetCommands().Execute(Context(), name, parameters);
			INFO(std::string(name), ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		ImGuiWindow* FindPanelWindow(const char* id)
		{
			return ImGui::FindWindowByName(Layer->GetPanels().GetWindowName(id).c_str());
		}
	};

	EditorOptions WithFeatureProject(const std::string& directoryName)
	{
		EditorOptions options;
		options.ProjectPath = CopyFeatureProject(CreateTemporaryDirectory(directoryName) / "Project");
		return options;
	}

	// A panel for registry tests.
	class TestPanel : public EditorPanel
	{
	public:
		void OnImGuiRender(EditorPanelContext&) override { Draws++; }
		void OnHidden(EditorPanelContext&) override { Hides++; }

		int Draws = 0;
		int Hides = 0;
	};

	EditorPanelDescriptor MakeTestPanel(std::string id, std::string title = "Test Panel")
	{
		EditorPanelDescriptor descriptor;
		descriptor.Id = std::move(id);
		descriptor.Title = std::move(title);
		descriptor.Icon = Icons::Box;
		descriptor.Create = []() { return CreateScope<TestPanel>(); };
		return descriptor;
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

		// The panels are up, the toolbar and the status bar too.
		for (const char* panel : { EditorPanels::c_Viewport, EditorPanels::c_Hierarchy, EditorPanels::c_Inspector, EditorPanels::c_ContentBrowser,
			EditorPanels::c_Console })
		{
			CAPTURE(panel);
			ImGuiWindow* window = editor.FindPanelWindow(panel);
			REQUIRE(window);
			CHECK(window->Active);
		}
		for (const char* item : { "Toolbar.Play", "Toolbar.Move", "Toolbar.BuildScripts", "Status.PlayState", "Status.Assets", "Viewport.Grid" })
		{
			CAPTURE(item);
			CHECK(UI::ItemProbe::Find(item).has_value());
		}

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

	TEST_CASE("Clicking Play on the toolbar runs play.start, and Stop play.stop")
	{
		ScopedAudioEngine audio;
		REQUIRE(audio.Initialized);
		HarnessEditor editor({}, WithFeatureProject("EditorUIPlay"));
		editor.Frames(2);
		REQUIRE_FALSE(editor.Context().IsPlaying());
		// Stop is disabled while editing: the harness refuses to click it.
		CHECK_FALSE(editor.Click("Toolbar.Stop"));

		REQUIRE(editor.Click("Toolbar.Play"));
		CHECK(editor.Context().GetSceneState() == SceneState::Play);
		CHECK(editor.Context().GetActiveScene() != editor.Context().GetEditScene());
		editor.Frames(1);
		// Play is shown active (disabled while playing); Pause becomes available.
		CHECK_FALSE(UI::ItemProbe::Find("Toolbar.Play")->Enabled);
		REQUIRE(editor.Click("Toolbar.Pause"));
		CHECK(editor.Context().IsPaused());
		REQUIRE(editor.Click("Toolbar.Stop"));
		CHECK_FALSE(editor.Context().IsPlaying());
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

	TEST_CASE("Panels close and reopen, and imgui.ini remembers which are open")
	{
		std::string settings;
		{
			HarnessEditor editor;
			editor.Frames(2);
			EditorPanelRegistry& panels = editor.Layer->GetPanels();
			CHECK(panels.IsOpen(EditorPanels::c_Console));
			panels.SetOpen(EditorPanels::c_Console, false);
			editor.Frames(1);
			ImGuiWindow* console = editor.FindPanelWindow(EditorPanels::c_Console);
			REQUIRE(console);
			CHECK_FALSE(console->Active);

			size_t size = 0;
			settings = ImGui::SaveIniSettingsToMemory(&size);
			CHECK(settings.find("[StrataPanels][Open]") != std::string::npos);
			CHECK(settings.find("Console=0") != std::string::npos);
			CHECK(settings.find("Hierarchy=1") != std::string::npos);
			CHECK(settings.find("[StrataPanels][Layout]\nVersion=" + std::to_string(EditorLayer::c_LayoutVersion)) != std::string::npos);
		}
		{
			// A new session with those settings keeps the Console closed until it is focused (e.g. by the errors pill).
			HarnessEditor editor({}, {}, settings);
			editor.Frames(2);
			EditorPanelRegistry& panels = editor.Layer->GetPanels();
			CHECK_FALSE(panels.IsOpen(EditorPanels::c_Console));
			CHECK(panels.IsOpen(EditorPanels::c_Viewport));
			panels.Focus(EditorPanels::c_Console);
			editor.Frames(2);
			CHECK(panels.IsOpen(EditorPanels::c_Console));
			ImGuiWindow* console = editor.FindPanelWindow(EditorPanels::c_Console);
			REQUIRE(console);
			CHECK(console->Active);
			CHECK_FALSE(console->Hidden);
		}
		{
			// Settings of an older default layout are replaced by the current default layout, with every panel open.
			std::string outdated = settings;
			const std::string version = "Version=" + std::to_string(EditorLayer::c_LayoutVersion);
			outdated.replace(outdated.find(version), version.size(), "Version=1");
			HarnessEditor editor({}, {}, outdated);
			editor.Frames(2);
			CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Console));
		}
	}

	TEST_CASE("The panel registry checks what registers and draws open panels in their windows")
	{
		ImGuiHarness harness;
		EditorPanelRegistry registry;
		std::string error;
		CHECK_FALSE(registry.Register(MakeTestPanel(""), &error));
		CHECK_FALSE(registry.Register(MakeTestPanel("Bad=Id"), &error));
		CHECK_FALSE(registry.Register(MakeTestPanel("Bad###Id"), &error));
		CHECK_FALSE(registry.Register(MakeTestPanel("Untitled", ""), &error));
		EditorPanelDescriptor noFactory = MakeTestPanel("NoFactory");
		noFactory.Create = nullptr;
		CHECK_FALSE(registry.Register(std::move(noFactory), &error));
		EditorPanelDescriptor emptyFactory = MakeTestPanel("EmptyFactory");
		emptyFactory.Create = []() { return Scope<EditorPanel>(); };
		CHECK_FALSE(registry.Register(std::move(emptyFactory), &error));
		CHECK(registry.GetCount() == 0);

		REQUIRE_MESSAGE(registry.Register(MakeTestPanel("Test"), &error), error);
		CHECK_FALSE(registry.Register(MakeTestPanel("Test"), &error));
		EditorPanelDescriptor closed = MakeTestPanel("Closed", "Closed Panel");
		closed.OpenByDefault = false;
		closed.MenuPath = "Debug";
		REQUIRE(registry.Register(std::move(closed), &error));
		CHECK(registry.GetCount() == 2);
		CHECK(registry.GetWindowName("Test") == std::string(Icons::Box) + "  Test Panel###Test");
		CHECK(registry.GetWindowName("Missing").empty());
		CHECK_FALSE(registry.IsOpen("Closed"));

		TestPanel* open = registry.Get<TestPanel>("Test");
		TestPanel* hidden = registry.Get<TestPanel>("Closed");
		REQUIRE(open);
		REQUIRE(hidden);
		EditorContext context(EditorContextSpecification { false, false });
		EditorCommandRegistry commands;
		EditorCommandRunner runner;
		EditorPanelContext panelContext { context, commands, runner };
		harness.Frame([&]() { registry.OnImGuiRender(panelContext); });
		CHECK(open->Draws == 1);
		CHECK(hidden->Draws == 0);
		CHECK(hidden->Hides == 1);
		registry.SetOpen("Closed", true);
		harness.Frame([&]() { registry.OnImGuiRender(panelContext); });
		CHECK(hidden->Draws == 1);
	}

	TEST_CASE("The status bar counts unread errors and opens the Console")
	{
		HarnessEditor editor;
		editor.Frames(2);
		CHECK_FALSE(UI::ItemProbe::Find("Status.Errors").has_value());
		editor.Layer->GetPanels().SetOpen(EditorPanels::c_Console, false);
		ST_ERROR("EditorUITests: an error for the status bar");
		editor.Frames(1);
		const ConsolePanel* console = editor.Layer->GetPanels().Get<ConsolePanel>(EditorPanels::c_Console);
		REQUIRE(console);
		CHECK(console->GetUnreadErrors() == 1);
		REQUIRE(UI::ItemProbe::Find("Status.Errors").has_value());

		REQUIRE(editor.Click("Status.Errors"));
		editor.Frames(2);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Console));
		CHECK(console->GetUnreadErrors() == 0);
		CHECK_FALSE(UI::ItemProbe::Find("Status.Errors").has_value());
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
