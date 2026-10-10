#include <doctest/doctest.h>

#include "Audio/AudioTestUtils.h"
#include "Editor/HarnessEditor.h"
#include "Editor/ImGuiHarness.h"
#include "EditorLayer.h"
#include "Panels/ConsolePanel.h"
#include "Panels/ViewportPanel.h"
#include "TestHelpers.h"
#include "UI/EditorFonts.h"
#include "UI/EditorPanelRegistry.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Events/ApplicationEvent.h>
#include <Strata/Network/EditorSession.h>
#include <Strata/Network/RpcClient.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	ImU32 ToColor(const ImVec4& color)
	{
		return UI::ToColorU32(color);
	}

	// Script builds that run the test executable as CMake (STRATA_TEST_FAKE_CMAKE=succeed): they build nothing.
	ScriptBuildSettings MakeFakeScriptBuild()
	{
		ScriptBuildSettings settings = ScriptBuildSettings::GetEngineDefaults();
		settings.CMake = GetTestExecutablePath();
		return settings;
	}

	// Whether the last frame drew a filled shape of exactly this color inside the rectangle (ImGui's draw data).
	bool DrewColor(const ImVec2& min, const ImVec2& max, ImU32 color)
	{
		const ImDrawData* drawData = ImGui::GetDrawData();
		if (!drawData)
			return false;
		for (const ImDrawList* list : drawData->CmdLists)
		{
			for (const ImDrawVert& vertex : list->VtxBuffer)
			{
				if (vertex.col == color && vertex.pos.x >= min.x && vertex.pos.x <= max.x && vertex.pos.y >= min.y && vertex.pos.y <= max.y)
					return true;
			}
		}
		return false;
	}

	float GetWindowFraction(ImGuiWindow* window, const ImVec2& display, bool width)
	{
		REQUIRE(window);
		return width ? window->Size.x / display.x : window->Size.y / display.y;
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

		editor.Frames(1, ImGuiHarness::c_DeltaTime);
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

	TEST_CASE("The default layout is viewport first, with the Content Browser showing")
	{
		// The editor's window on this project's reference machine: 3840 x 2054 at 150%.
		const ImVec2 display(3840.0f, 2054.0f);
		HarnessEditor editor({ display, 1.5f }, WithFeatureProject("EditorUILayout"));
		editor.Frames(3);

		const ViewportPanel* viewport = editor.Layer->GetPanels().Get<ViewportPanel>(EditorPanels::c_Viewport);
		REQUIRE(viewport);
		const glm::vec2 image = viewport->GetImageArea().Size;
		const float share = image.x * image.y / (display.x * display.y);
		MESSAGE("Viewport image: " << image.x << " x " << image.y << " = " << share * 100.0f << "% of the window");
		CHECK(share >= 0.45f);

		// About 15% of the width for the Hierarchy, 20% for the Inspector, and the bottom area under the viewport.
		CHECK(GetWindowFraction(editor.FindPanelWindow(EditorPanels::c_Hierarchy), display, true) == doctest::Approx(0.15).epsilon(0.05));
		CHECK(GetWindowFraction(editor.FindPanelWindow(EditorPanels::c_Inspector), display, true) == doctest::Approx(0.20).epsilon(0.05));
		ImGuiWindow* viewportWindow = editor.FindPanelWindow(EditorPanels::c_Viewport);
		REQUIRE(viewportWindow);
		ImGuiWindow* contentBrowser = editor.FindPanelWindow(EditorPanels::c_ContentBrowser);
		ImGuiWindow* console = editor.FindPanelWindow(EditorPanels::c_Console);
		REQUIRE(contentBrowser);
		REQUIRE(console);
		CHECK(contentBrowser->DockNode == console->DockNode);
		CHECK(contentBrowser->Pos.y >= viewportWindow->Pos.y + viewportWindow->Size.y - 1.0f);
		CHECK(contentBrowser->Size.x == doctest::Approx(viewportWindow->Size.x).epsilon(0.01));
		// The Content Browser is the bottom area's visible tab on first run.
		CHECK_FALSE(contentBrowser->Hidden);
		CHECK(console->Hidden);
	}

	TEST_CASE("Panels close and reopen, and imgui.ini remembers which are open")
	{
		// Without a project, past the launcher (Continue without a project): the dock space and its panels.
		std::string settings;
		{
			HarnessEditor editor;
			editor.Layer->DismissLauncher();
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
			editor.Layer->DismissLauncher();
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
			editor.Layer->DismissLauncher();
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

	TEST_CASE("Idle throttling lowers the frame rate only while nothing happens")
	{
		ScopedAudioEngine audio;
		REQUIRE(audio.Initialized);
		HarnessEditor editor;
		REQUIRE(editor.Layer->IsThrottlingEnabled());
		// Startup counts as activity.
		editor.Frames(2);
		CHECK(editor.Host->MaxFrameRate == 0);
		CHECK_FALSE(editor.Layer->IsIdle());

		// Nothing for a second: the focused editor idles at 30 frames per second, an unfocused one at 10.
		editor.Frames(1, 1.0f);
		CHECK(editor.Layer->IsIdle());
		CHECK(editor.Host->MaxFrameRate == EditorLayer::c_IdleFrameRate);
		editor.Host->WindowFocused = false;
		editor.Frames(1);
		CHECK(editor.Host->MaxFrameRate == EditorLayer::c_UnfocusedIdleFrameRate);
		editor.Host->WindowFocused = true;

		// Input: the full rate at once, and for half a second after it.
		editor.Harness.MoveMouse(ImVec2(400.0f, 300.0f));
		editor.Frames(1);
		CHECK(editor.Host->MaxFrameRate == 0);
		editor.Frames(1, 0.3f);
		CHECK(editor.Host->MaxFrameRate == 0);
		editor.Frames(1, 0.3f);
		CHECK(editor.Host->MaxFrameRate == EditorLayer::c_IdleFrameRate);
		// A held key keeps it up even with the mouse at rest (e.g. flying the camera).
		editor.Harness.SetKey(ImGuiKey_W, true);
		editor.Frames(1);
		editor.Frames(1, 1.0f);
		CHECK(editor.Host->MaxFrameRate == 0);
		editor.Harness.SetKey(ImGuiKey_W, false);
		editor.Frames(1);
		editor.Frames(1, 1.0f);
		CHECK(editor.Host->MaxFrameRate == EditorLayer::c_IdleFrameRate);

		// A running scene needs every frame; a paused one does not, unless it steps.
		editor.Run("play.start");
		editor.Frames(1, 1.0f);
		CHECK(editor.Host->MaxFrameRate == 0);
		editor.Run("play.pause", { { "paused", true } });
		editor.Frames(1, 1.0f);
		CHECK(editor.Host->MaxFrameRate == EditorLayer::c_IdleFrameRate);
		editor.Run("play.step", { { "frames", 5 } });
		editor.Frames(1, 1.0f);
		CHECK(editor.Host->MaxFrameRate == 0);
		editor.Run("play.stop");
		editor.Frames(1, 1.0f);
		CHECK(editor.Host->MaxFrameRate == EditorLayer::c_IdleFrameRate);

		// Commands that take frames run at the full rate.
		bool waited = false;
		editor.Layer->GetCommandRunner().Run(editor.Context(), editor.Layer->GetCommands(), "editor.wait", { { "frames", 3 } },
			[&waited](const EditorCommandResult& result) { waited = result.Success; });
		editor.Frames(1, 1.0f);
		CHECK(editor.Host->MaxFrameRate == 0);
		editor.Frames(4, 1.0f);
		CHECK(waited);
		CHECK(editor.Host->MaxFrameRate == EditorLayer::c_IdleFrameRate);

		// editor.status reports it.
		const nlohmann::json status = editor.Run("editor.status");
		const nlohmann::json& frameRate = status["editor"]["frameRate"];
		CHECK(frameRate["idle"] == true);
		CHECK(frameRate["cap"] == EditorLayer::c_IdleFrameRate);
		CHECK(frameRate["throttling"] == true);
		CHECK(frameRate["average"].get<double>() > 0.0);
		CHECK(status["editor"]["uiScale"] == 1.0);
		// The fake host has no window.
		CHECK(status["editor"]["window"].is_null());
	}

	TEST_CASE("Scripted and frame-limited editors are never throttled")
	{
		EditorOptions options;
		options.MaxFrames = 1000;
		HarnessEditor editor({}, options);
		CHECK_FALSE(editor.Layer->IsThrottlingEnabled());
		editor.Frames(3, 1.0f);
		CHECK_FALSE(editor.Layer->IsIdle());
		CHECK(editor.Host->FrameRateChanges == 0);
	}

	TEST_CASE("The status bar counts unread errors and opens the Console")
	{
		// The status bar belongs to the editor past the launcher.
		HarnessEditor editor;
		editor.Layer->DismissLauncher();
		editor.Frames(2);
		CHECK_FALSE(UI::ItemProbe::Find("Status.Errors").has_value());
		editor.Layer->GetPanels().SetOpen(EditorPanels::c_Console, false);
		ST_ERROR("EditorUITests: an error for the status bar");
		editor.Frames(1);
		const ConsolePanel* console = editor.Layer->GetPanels().Get<ConsolePanel>(EditorPanels::c_Console);
		REQUIRE(console);
		CHECK(console->GetUnreadErrors() == 1);
		REQUIRE(UI::ItemProbe::Find("Status.Errors").has_value());

		// The Console appears, is read on the frame after (not on the one it appears in, see the next test), and the status
		// bar, drawn before the panels, drops the pill in the frame after that.
		REQUIRE(editor.Click("Status.Errors"));
		editor.Frames(3);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Console));
		CHECK(console->GetUnreadErrors() == 0);
		CHECK_FALSE(UI::ItemProbe::Find("Status.Errors").has_value());
	}

	TEST_CASE("Errors logged before the first frame reach the errors pill")
	{
		// A project that cannot be opened is reported while the editor starts, before the Console was ever drawn. Its
		// window takes the focus on the frame it appears in, before its dock node shows the Content Browser's tab instead:
		// that is no reading of the error.
		EditorOptions options;
		options.ProjectPath = CreateTemporaryDirectory("EditorUIStartupError") / "Missing";
		HarnessEditor editor({}, options);
		// Without a project the launcher shows (and says why, see Editor.Launcher); the status bar is the editor's.
		editor.Layer->DismissLauncher();
		editor.Frames(3);
		const ConsolePanel* console = editor.Layer->GetPanels().Get<ConsolePanel>(EditorPanels::c_Console);
		REQUIRE(console);
		CHECK(console->GetUnreadErrors() >= 1);
		CHECK(editor.GetPillColor("Status.Errors") == ToColor(UI::GetThemeColors().Error));
		ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Console);
		REQUIRE(window);
		CHECK(window->Hidden);

		REQUIRE(editor.Click("Status.Errors"));
		editor.Frames(3);
		CHECK_FALSE(window->Hidden);
		CHECK(console->GetUnreadErrors() == 0);
		CHECK_FALSE(UI::ItemProbe::Find("Status.Errors").has_value());
	}

	TEST_CASE("The automation pill tells whether and why tools cannot control the editor")
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		{
			// --no-automation: off, as asked. The status bar is the editor's, not the launcher's.
			HarnessEditor editor;
			editor.Layer->DismissLauncher();
			editor.Frames(2);
			CHECK(editor.GetPillColor("Status.Automation") == ToColor(colors.TextSecondary));
		}
		{
			// Asked for, but it cannot start: its session file cannot be written (the session directory would be inside a
			// file). The pill is an error that leads to the Console.
			const std::filesystem::path blocker = CreateTemporaryDirectory("EditorUIAutomationFailure") / "File";
			REQUIRE(FileSystem::WriteText(blocker, "not a directory"));
			ScopedEnvironmentVariable sessions("STRATA_SESSION_DIR", FileSystem::ToUTF8(blocker / "Sessions"));
			HarnessEditor editor({}, {}, {}, true);
			editor.Layer->DismissLauncher();
			editor.Frames(2);
			CHECK(editor.GetPillColor("Status.Automation") == ToColor(colors.Error));
			editor.Layer->GetPanels().SetOpen(EditorPanels::c_Console, false);
			editor.Frames(1);
			REQUIRE(editor.Click("Status.Automation"));
			editor.Frames(2);
			CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Console));
		}
	}

	TEST_CASE("Automation requests keep the full frame rate for a second")
	{
		const std::filesystem::path sessions = CreateTemporaryDirectory("EditorUIAutomationSessions");
		ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessions));
		HarnessEditor editor({}, {}, {}, true);
		editor.Layer->DismissLauncher(); // The status bar is the editor's, not the launcher's
		editor.Frames(2);
		const UI::ThemeColors& colors = UI::GetThemeColors();
		CHECK(editor.GetPillColor("Status.Automation") == ToColor(colors.TextSecondary)); // Listening, no client yet

		const std::vector<EditorSessionInfo> found = EditorSession::FindSessions(sessions);
		REQUIRE(found.size() == 1);
		RpcClient client;
		REQUIRE_MESSAGE(client.Connect(found[0].Address, found[0].Port, found[0].Token, std::chrono::milliseconds(5000)), client.GetLastError());
		editor.Frames(1, 1.0f);
		REQUIRE(editor.Host->MaxFrameRate == EditorLayer::c_IdleFrameRate);
		CHECK(editor.GetPillColor("Status.Automation") == ToColor(colors.Info)); // A client is connected

		// A tool's request: the editor answers it in its frames, at the full rate, and stays there for a second.
		std::future<RpcResult> call = std::async(std::launch::async, [&client]() { return client.Call("editor.status", nlohmann::json::object(),
			std::chrono::milliseconds(10000)); });
		REQUIRE(editor.FramesUntil([&call]() { return call.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }));
		CHECK(call.get().IsSuccess());
		CHECK(editor.Host->MaxFrameRate == 0);
		editor.Frames(1, 0.5f);
		CHECK(editor.Host->MaxFrameRate == 0);
		editor.Frames(1, 0.6f);
		CHECK(editor.Host->MaxFrameRate == EditorLayer::c_IdleFrameRate);
		client.Close();
	}

	TEST_CASE("Loading assets keep the full frame rate")
	{
		HarnessEditor editor({}, WithFeatureProject("EditorUILoading"));
		editor.Frames(2);
		editor.Frames(1, 1.0f);
		REQUIRE(editor.Host->MaxFrameRate == EditorLayer::c_IdleFrameRate);

		EditorAssetManager* assets = editor.Context().GetAssetManager();
		REQUIRE(assets);
		AssetHandle unloaded = UUID::Null();
		for (const AssetMetadata& metadata : assets->GetAllMetadata())
		{
			if (!metadata.IsBuiltin() && assets->GetAssetState(metadata.Handle) == AssetState::Unloaded)
			{
				unloaded = metadata.Handle;
				break;
			}
		}
		REQUIRE(unloaded.IsValid());
		// Requested after the asset manager's update in the frame: still loading when the frame chooses its rate.
		editor.FrameWith([&]() { assets->GetAsset(unloaded); }, 1.0f);
		CHECK(assets->GetAssetState(unloaded) == AssetState::Loading);
		CHECK(editor.Host->MaxFrameRate == 0);
		REQUIRE(editor.FramesUntil([&]() { return assets->GetAssetState(unloaded) != AssetState::Loading; }));
		editor.Frames(1, 1.0f);
		CHECK(editor.Host->MaxFrameRate == EditorLayer::c_IdleFrameRate);
	}

	TEST_CASE("The assets pill turns red when an asset fails to load")
	{
		EditorOptions options = WithFeatureProject("EditorUIFailedAsset");
		// An image that is none: it fails to import, and so to load.
		REQUIRE(FileSystem::WriteText(options.ProjectPath.parent_path() / "Assets" / "Textures" / "Broken.png", "not a PNG"));
		HarnessEditor editor({}, options);
		editor.Frames(2);
		const UI::ThemeColors& colors = UI::GetThemeColors();
		CHECK(editor.GetPillColor("Status.Assets") == ToColor(colors.TextSecondary));

		EditorAssetManager* assets = editor.Context().GetAssetManager();
		REQUIRE(assets);
		AssetHandle broken = UUID::Null();
		REQUIRE(editor.FramesUntil([&]()
		{
			broken = assets->FindAssetByPath("Textures/Broken.png");
			if (!broken.IsValid())
				return false;
			assets->GetAsset(broken);
			return assets->GetAssetState(broken) == AssetState::Failed;
		}));
		editor.Frames(1);
		CHECK(editor.GetPillColor("Status.Assets") == ToColor(colors.Error));
	}

	TEST_CASE("The scripts pill offers to build scripts that are not built, and script builds keep the full frame rate")
	{
		// The test executable stands in for CMake (it builds nothing, see TestMain.cpp).
		ScopedEnvironmentVariable fakeCMake("STRATA_TEST_FAKE_CMAKE", "succeed");
		EditorOptions options = WithFeatureProject("EditorUIScripts");
		options.ScriptBuild = MakeFakeScriptBuild();
		// The test build builds the feature project's scripts; this copy gets a script build of its own.
		const std::filesystem::path scripts = options.ProjectPath.parent_path() / "Scripts";
		REQUIRE(FileSystem::CreateDirectories(scripts));
		REQUIRE(FileSystem::WriteText(scripts / "CMakeLists.txt", "# Built by the stand-in for CMake\n"));
		HarnessEditor editor({}, options);
		editor.Frames(2);
		CHECK(editor.GetPillColor("Status.Scripts") == ToColor(UI::GetThemeColors().Warning));

		// Clicking the pill builds them, as does the toolbar's Build Scripts.
		const ScriptBuilder& builder = editor.Context().GetScriptBuilder();
		REQUIRE(editor.Click("Status.Scripts"));
		REQUIRE(editor.FramesUntil([&builder]() { return !builder.IsRunning() && builder.GetLastResult().ID != 0; }));
		const uint64_t firstBuild = builder.GetLastResult().ID;
		REQUIRE(editor.Click("Toolbar.BuildScripts"));
		REQUIRE(editor.FramesUntil([&builder, firstBuild]() { return !builder.IsRunning() && builder.GetLastResult().ID > firstBuild; }));

		// A running build keeps the frame rate up, also without a command waiting for it.
		editor.Frames(1, 1.0f);
		REQUIRE(editor.Host->MaxFrameRate == EditorLayer::c_IdleFrameRate);
		std::string error;
		REQUIRE_MESSAGE(editor.Context().BuildScripts(&error), error);
		editor.Frames(1, 1.0f);
		CHECK(builder.IsRunning());
		CHECK(editor.Host->MaxFrameRate == 0);
		REQUIRE(editor.FramesUntil([&builder]() { return !builder.IsRunning(); }));
		editor.Frames(1, 1.0f);
		CHECK(editor.Host->MaxFrameRate == EditorLayer::c_IdleFrameRate);
	}

	TEST_CASE("The frame time is what frames take, not the interval of the idle frame rate")
	{
		HarnessEditor editor;
		// Idle at 30 frames per second: a frame every 33 ms, each taking 4 ms.
		editor.Host->FrameWorkTime = 0.004;
		editor.Frames(90, 1.0f / 30.0f);
		REQUIRE(editor.Layer->IsIdle());
		const EditorFrameStats& frame = editor.Layer->GetFrameStats();
		CHECK(frame.WorkMilliseconds == doctest::Approx(4.0f));
		CHECK(frame.FramesPerSecond == doctest::Approx(30.0f).epsilon(0.02));
		CHECK(frame.Idle);
		const nlohmann::json status = editor.Run("editor.status");
		CHECK(status["editor"]["frameRate"]["frameMilliseconds"].get<double>() == doctest::Approx(4.0));
		// editor.wait reports the same measurement, recorded per frame from the host.
		CHECK(editor.Context().GetLastFrameTime() == doctest::Approx(4.0));
	}

	TEST_CASE("The toolbar's tools and the viewport's chips change the viewport's settings")
	{
		HarnessEditor editor({}, WithFeatureProject("EditorUIToolbar"));
		editor.Frames(2);
		ViewportSettings& settings = editor.Context().GetViewport().GetSettings();
		// The selection outline carries the theme's accent, like selected rows.
		const ImVec4& accent = UI::GetThemeColors().Accent;
		CHECK(settings.SelectionColor == glm::vec4(accent.x, accent.y, accent.z, accent.w));

		REQUIRE(editor.Click("Toolbar.Rotate"));
		CHECK(settings.Gizmo == GizmoOperation::Rotate);
		REQUIRE(editor.Click("Toolbar.Scale"));
		CHECK(settings.Gizmo == GizmoOperation::Scale);
		REQUIRE(editor.Click("Toolbar.Select"));
		CHECK(settings.Gizmo == GizmoOperation::None);
		REQUIRE(editor.Click("Toolbar.Move"));
		CHECK(settings.Gizmo == GizmoOperation::Translate);
		const GizmoSpace space = settings.Space;
		REQUIRE(editor.Click("Toolbar.Space"));
		CHECK(settings.Space != space);
		REQUIRE(editor.Click("Toolbar.Space"));
		CHECK(settings.Space == space);
		REQUIRE_FALSE(settings.Snap);
		REQUIRE(editor.Click("Toolbar.Snap"));
		CHECK(settings.Snap);
		REQUIRE(editor.Click("Toolbar.Snap"));
		CHECK_FALSE(settings.Snap);

		const auto toggles = [&](const char* chip, bool& value)
		{
			CAPTURE(chip);
			const bool before = value;
			REQUIRE(editor.Click(chip));
			CHECK(value != before);
			REQUIRE(editor.Click(chip));
			CHECK(value == before);
		};
		toggles("Viewport.Grid", settings.ShowGrid);
		toggles("Viewport.Outline", settings.ShowSelectionOutline);
		toggles("Viewport.Gizmos", settings.ShowSceneGizmos);
		toggles("Viewport.Stats", settings.ShowStats);
		// The camera chip opens the camera settings.
		REQUIRE(editor.Click("Viewport.Camera"));
		CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
	}

	TEST_CASE("Simulate, Pause and Step on the toolbar")
	{
		ScopedAudioEngine audio;
		REQUIRE(audio.Initialized);
		HarnessEditor editor({}, WithFeatureProject("EditorUISimulate"));
		editor.Frames(2);
		CHECK_FALSE(editor.Click("Toolbar.Step")); // Only while paused

		REQUIRE(editor.Click("Toolbar.Simulate"));
		CHECK(editor.Context().GetSceneState() == SceneState::Simulate);
		editor.Frames(1);
		CHECK_FALSE(UI::ItemProbe::Find("Toolbar.Simulate")->Enabled);
		REQUIRE(editor.Click("Toolbar.Pause"));
		REQUIRE(editor.Context().IsPaused());
		editor.Frames(1);
		REQUIRE(editor.Click("Toolbar.Step"));
		CHECK(editor.Context().GetActiveScene()->GetStepFrames() == 1);
		editor.Frames(1);
		CHECK(editor.Context().GetActiveScene()->GetStepFrames() == 0);
		REQUIRE(editor.Click("Toolbar.Pause"));
		CHECK_FALSE(editor.Context().IsPaused());
		REQUIRE(editor.Click("Toolbar.Stop"));
		CHECK(editor.Context().GetSceneState() == SceneState::Edit);
	}

	TEST_CASE("The game view keeps the stats toggle while the game does not have the input")
	{
		ScopedAudioEngine audio;
		REQUIRE(audio.Initialized);
		HarnessEditor editor({}, WithFeatureProject("EditorUIGameView"));
		editor.Frames(2);
		CHECK(UI::ItemProbe::Find("Viewport.Grid").has_value());

		// Playing through the scene's camera: of the chips only the stats toggle remains, and it works while playing.
		REQUIRE(editor.Click("Toolbar.Play"));
		editor.Frames(1);
		REQUIRE(editor.Context().IsPlaying());
		CHECK_FALSE(UI::ItemProbe::Find("Viewport.Grid").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Viewport.Camera").has_value());
		// The editor-view settings do not apply to the game's view.
		CHECK_FALSE(UI::ItemProbe::Find("Viewport.PreviewLighting").has_value());
		CHECK_FALSE(UI::ItemProbe::Find("Viewport.GameUI").has_value());
		ViewportSettings& settings = editor.Context().GetViewport().GetSettings();
		const bool stats = settings.ShowStats;
		REQUIRE(editor.Click("Viewport.Stats"));
		CHECK(settings.ShowStats != stats);
		// The chip does not hand the game the input.
		CHECK_FALSE(editor.Context().IsGameInputActive());

		// Clicking the game gives it the input, and every click on the image is the game's: no chip.
		const ViewportPanel* viewport = editor.Layer->GetPanels().Get<ViewportPanel>(EditorPanels::c_Viewport);
		REQUIRE(viewport);
		const ViewportImageArea& image = viewport->GetImageArea();
		editor.ClickAt(ImVec2(image.Min.x + image.Size.x * 0.5f, image.Min.y + image.Size.y * 0.5f));
		editor.Frames(1);
		REQUIRE(editor.Context().IsGameInputActive());
		CHECK_FALSE(UI::ItemProbe::Find("Viewport.Stats").has_value());

		// Shift+F1 takes the input back: the toggle returns.
		editor.Harness.SetKey(ImGuiMod_Shift, true);
		editor.Harness.SetKey(ImGuiKey_F1, true);
		editor.Frames(1);
		editor.Harness.SetKey(ImGuiKey_F1, false);
		editor.Harness.SetKey(ImGuiMod_Shift, false);
		editor.Frames(2);
		CHECK_FALSE(editor.Context().IsGameInputActive());
		CHECK(UI::ItemProbe::Find("Viewport.Stats").has_value());
		editor.Run("play.stop");
	}

	TEST_CASE("Gizmo drags snap per the toolbar's snap toggle, inverted while Ctrl is held")
	{
		HarnessEditor editor({}, WithFeatureProject("EditorUISnap"));
		editor.Frames(2);
		// An entity where the editor camera looks: at the center of the image, under the gizmo's center handle (which
		// moves it in the view's plane).
		const glm::vec3 target = editor.Context().GetViewport().GetCamera().GetTarget();
		const nlohmann::json created = editor.Run("entity.create", { { "name", "SnapTarget" },
			{ "components", { { "Transform", { { "Translation", { target.x, target.y, target.z } } } } } } });
		const std::optional<UUID> id = UUIDFromJson(created["id"]);
		REQUIRE(id);
		editor.Run("selection.set", { { "entities", { created["id"] } } });
		editor.Frames(2);
		const ViewportPanel* viewport = editor.Layer->GetPanels().Get<ViewportPanel>(EditorPanels::c_Viewport);
		REQUIRE(viewport);
		const ViewportImageArea& image = viewport->GetImageArea();
		const ImVec2 center(image.Min.x + image.Size.x * 0.5f, image.Min.y + image.Size.y * 0.5f);
		const ViewportSettings& settings = editor.Context().GetViewport().GetSettings();
		REQUIRE(settings.Gizmo == GizmoOperation::Translate);
		const float step = settings.TranslateSnap;

		// Drags the center handle across a good part of the image (with Ctrl held or not) and returns how far the entity
		// went; the drag is undone afterwards, so the entity is back under the image's center.
		const auto drag = [&](bool ctrl)
		{
			Entity entity = editor.Context().GetActiveScene()->GetEntityByUUID(*id);
			REQUIRE(entity);
			const glm::vec3 before = entity.GetComponent<TransformComponent>().Translation;
			// Hover first, over a few frames: the viewport hands the mouse to the gizmo once it knows the gizmo is under it,
			// and ImGui applies a mouse move queued with a key change a frame later.
			editor.Harness.MoveMouse(center);
			editor.Frames(1);
			if (ctrl)
				editor.Harness.SetKey(ImGuiMod_Ctrl, true);
			editor.Frames(3);
			REQUIRE(ImGuizmo::IsOver());
			editor.Harness.SetMouseButton(ImGuiMouseButton_Left, true);
			editor.Frames(1);
			REQUIRE(ImGuizmo::IsUsingAny());
			editor.Harness.MoveMouse(ImVec2(center.x + image.Size.x * 0.1f, center.y + image.Size.y * 0.05f));
			editor.Frames(1);
			editor.Harness.MoveMouse(ImVec2(center.x + image.Size.x * 0.2f, center.y + image.Size.y * 0.1f));
			editor.Frames(1);
			editor.Harness.SetMouseButton(ImGuiMouseButton_Left, false);
			editor.Frames(1);
			if (ctrl)
				editor.Harness.SetKey(ImGuiMod_Ctrl, false);
			editor.Frames(1);
			const glm::vec3 moved = entity.GetComponent<TransformComponent>().Translation - before;
			// The drag was one undo step.
			REQUIRE(editor.Context().Undo());
			editor.Frames(1);
			CHECK(entity.GetComponent<TransformComponent>().Translation == before);
			return moved;
		};
		// Whether every axis moved by whole snapping steps.
		const auto snapped = [step](const glm::vec3& moved)
		{
			for (int axis = 0; axis < 3; axis++)
			{
				const float steps = moved[axis] / step;
				if (std::abs(steps - std::round(steps)) > 0.01f)
					return false;
			}
			return true;
		};

		// Snapping off: free drags; Ctrl snaps.
		glm::vec3 moved = drag(false);
		CHECK(glm::length(moved) > step);
		CHECK_FALSE(snapped(moved));
		moved = drag(true);
		CHECK(glm::length(moved) > step);
		CHECK(snapped(moved));
		// Snapping on: snapped drags; Ctrl frees them.
		REQUIRE(editor.Click("Toolbar.Snap"));
		REQUIRE(settings.Snap);
		moved = drag(false);
		CHECK(glm::length(moved) > step);
		CHECK(snapped(moved));
		moved = drag(true);
		CHECK(glm::length(moved) > step);
		CHECK_FALSE(snapped(moved));
	}

	TEST_CASE("A selected row keeps the accent under the mouse, other rows hover in a neutral color")
	{
		ImGuiHarness harness;
		ImVec2 rowMin[2];
		ImVec2 rowMax[2];
		const auto draw = [&]()
		{
			ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
			ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 20.0f, ImGui::GetFontSize() * 10.0f));
			ImGui::Begin("Rows");
			for (int row = 0; row < 2; row++)
			{
				const bool selected = row == 0;
				UI::PushSelectionColors(selected);
				ImGui::Selectable(selected ? "Selected" : "Other", selected);
				UI::PopSelectionColors();
				// Selectables draw their background into half the item spacing around them.
				const ImVec2 spacing = ImGui::GetStyle().ItemSpacing;
				rowMin[row] = ImVec2(ImGui::GetItemRectMin().x - spacing.x, ImGui::GetItemRectMin().y - spacing.y);
				rowMax[row] = ImVec2(ImGui::GetItemRectMax().x + spacing.x, ImGui::GetItemRectMax().y + spacing.y);
			}
			ImGui::End();
		};
		harness.Frame(draw);
		harness.Frame(draw);
		const ImU32 selected = ImGui::GetColorU32(ImGuiCol_Header);
		const ImU32 hovered = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
		const ImU32 selectedHovered = ImGui::GetColorU32(UI::GetThemeColors().SelectionHovered);
		CHECK(DrewColor(rowMin[0], rowMax[0], selected));

		const auto hover = [&](int row)
		{
			harness.MoveMouse(ImVec2((rowMin[row].x + rowMax[row].x) * 0.5f, (rowMin[row].y + rowMax[row].y) * 0.5f));
			harness.Frame(draw);
			harness.Frame(draw);
		};
		hover(0);
		CHECK(DrewColor(rowMin[0], rowMax[0], selectedHovered));
		CHECK_FALSE(DrewColor(rowMin[0], rowMax[0], hovered));
		hover(1);
		CHECK(DrewColor(rowMin[1], rowMax[1], hovered));
		CHECK_FALSE(DrewColor(rowMin[1], rowMax[1], selectedHovered));
		CHECK(DrewColor(rowMin[0], rowMax[0], selected));
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
