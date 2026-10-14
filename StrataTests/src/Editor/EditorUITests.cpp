#include <doctest/doctest.h>

#include "Audio/AudioTestUtils.h"
#include "Editor/HarnessEditor.h"
#include "Editor/ImGuiHarness.h"
#include "EditorLayer.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Panels/ConsolePanel.h"
#include "Panels/GameViewPanel.h"
#include "Panels/StreamingPanel.h"
#include "Panels/ViewportPanel.h"
#include "Scripting/ScriptTestUtils.h"
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
#include <Strata/Input/Input.h>
#include <Strata/Network/EditorSession.h>
#include <Strata/Network/RpcClient.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scripting/ScriptEngine.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <nlohmann/json.hpp>

#include <algorithm>
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

	// The status bar's pills, left to right, when every stream of work has one.
	const std::vector<std::string> c_EveryStatusPill = { "Status.PlayState", "Status.Frame", "Status.Assets", "Status.Automation", "Status.Scripts",
		"Status.InputHolds", "Status.Errors" };

	// Makes every pill of the status bar show in an editor started with automation (sessions in `sessions`): two agents (the
	// automation pill names the first), a loaded module (its name and classes), a game playing with keys a tool holds, and
	// errors (the feature scripts log some on purpose).
	void ShowEveryStatusPill(HarnessEditor& editor, const std::filesystem::path& sessions, RpcClient& first, RpcClient& second)
	{
		REQUIRE(editor.Context().HasProject());
		const std::vector<EditorSessionInfo> found = EditorSession::FindSessions(sessions);
		REQUIRE(found.size() == 1);
		REQUIRE_MESSAGE(first.Connect(found[0].Address, found[0].Port, found[0].Token, std::chrono::milliseconds(5000), "Claude Code"), first.GetLastError());
		REQUIRE_MESSAGE(second.Connect(found[0].Address, found[0].Port, found[0].Token, std::chrono::milliseconds(5000), "Visual Studio Code"),
			second.GetLastError());
		REQUIRE(editor.FramesUntil([&editor]() { return editor.GetPillText("Status.Automation").starts_with("2 agents"); }));
		editor.Run("script.load", { { "path", FileSystem::ToUTF8(GetFeatureScriptModule()) } });
		editor.RunToCompletion("play.start");
		editor.RunToCompletion("input.key", { { "key", "W" }, { "action", "press" }, { "wait", false } });
		editor.RunToCompletion("input.key", { { "key", "LeftShift" }, { "action", "press" }, { "wait", false } });
		editor.RunToCompletion("input.mouseButton", { { "button", "Left" }, { "action", "press" }, { "wait", false } });
		editor.Layer->GetPanels().SetOpen(EditorPanels::c_Console, false);
		ST_ERROR("EditorUITests: an error for the status bar");
		// The pills choose their texts by the width of the previous frame's.
		editor.Frames(3);
	}

	// Checks that the pills show side by side, left to right, and returns where the last one ends.
	float CheckPillsSideBySide(const std::vector<std::string>& pills)
	{
		float right = 0.0f;
		for (const std::string& pill : pills)
		{
			CAPTURE(pill);
			const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find(pill);
			REQUIRE(item.has_value());
			MESSAGE(pill << ": '" << item->Text << "' from " << item->Min.x << " to " << item->Max.x);
			CHECK(item->Min.x >= right);
			right = item->Max.x;
		}
		return right;
	}

	// imgui.ini text as an editor without the given panels saved it: no windows and no open states of them, and the layout
	// version it wrote.
	std::string WithoutPanels(const std::string& settings, const std::vector<std::string>& panels, int layoutVersion)
	{
		std::string result;
		std::string section;
		bool skipping = false;
		size_t start = 0;
		while (start < settings.size())
		{
			const size_t end = settings.find('\n', start);
			const std::string line = settings.substr(start, end == std::string::npos ? std::string::npos : end - start);
			start = end == std::string::npos ? settings.size() : end + 1;
			if (line.starts_with("["))
			{
				section = line;
				skipping = std::any_of(panels.begin(), panels.end(), [&line](const std::string& panel) { return line == "[Window][" + panel + "]"; });
			}
			if (skipping)
				continue;
			if (section == "[StrataPanels][Open]"
				&& std::any_of(panels.begin(), panels.end(), [&line](const std::string& panel) { return line.starts_with(panel + "="); }))
			{
				continue;
			}
			if (section == "[StrataPanels][Layout]" && line.starts_with("Version="))
			{
				result += "Version=" + std::to_string(layoutVersion) + "\n";
				continue;
			}
			result += line + "\n";
		}
		return result;
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

	TEST_CASE("At 1600 x 900 the Hierarchy is wide enough for its names, also at 200%")
	{
		const ImVec2 display(1600.0f, 900.0f);
		for (const float scale : { 1.0f, 2.0f })
		{
			CAPTURE(scale);
			HarnessEditor editor({ display, scale }, WithFeatureProject("EditorUIHierarchyWidth"));
			editor.Frames(3);
			ImGuiWindow* hierarchy = editor.FindPanelWindow(EditorPanels::c_Hierarchy);
			ImGuiWindow* inspector = editor.FindPanelWindow(EditorPanels::c_Inspector);
			ImGuiWindow* scene = editor.FindPanelWindow(EditorPanels::c_Viewport);
			REQUIRE(hierarchy);
			REQUIRE(inspector);
			REQUIRE(scene);
			float fontSize = 0.0f;
			float nameWidth = 0.0f;
			editor.Harness.Frame([&]()
			{
				fontSize = ImGui::GetFontSize();
				nameWidth = ImGui::CalcTextSize("Environment Light").x;
			});
			// A row holds its arrow, icon and visibility toggle (about six text heights with paddings) beside the name:
			// names of seventeen characters show whole. (The dock's splitters take a few pixels of each node.)
			const float splitter = 0.25f * fontSize;
			CHECK(hierarchy->Size.x >= 16.0f * fontSize - splitter);
			CHECK(hierarchy->Size.x - 6.0f * fontSize >= nameWidth);
			CHECK(inspector->Size.x >= std::min(18.0f * fontSize, 0.3f * display.x) - splitter);
			// At 100% the shares stay as they were; the Scene view keeps the most of the width at either scale.
			if (scale == 1.0f)
				CHECK(GetWindowFraction(hierarchy, display, true) == doctest::Approx(0.15).epsilon(0.05));
			CHECK(GetWindowFraction(scene, display, true) >= 0.39f);
		}
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
		// The Content Browser is the bottom area's visible tab on first run; the Problems panel waits behind it.
		CHECK_FALSE(contentBrowser->Hidden);
		CHECK(console->Hidden);
		ImGuiWindow* problems = editor.FindPanelWindow(EditorPanels::c_Problems);
		REQUIRE(problems);
		CHECK(problems->DockNode == console->DockNode);
		CHECK(problems->Hidden);

		// The Asset tab shares the Inspector's place, behind it until an asset is inspected.
		ImGuiWindow* inspector = editor.FindPanelWindow(EditorPanels::c_Inspector);
		ImGuiWindow* assetInspector = editor.FindPanelWindow(EditorPanels::c_AssetInspector);
		REQUIRE(inspector);
		REQUIRE(assetInspector);
		CHECK(assetInspector->DockNode == inspector->DockNode);
		CHECK_FALSE(inspector->Hidden);
		CHECK(assetInspector->Hidden);
		// The instruments are closed until asked for (their status pills open them), and then join the bottom area.
		EditorPanelRegistry& panels = editor.Layer->GetPanels();
		for (const char* instrument : { EditorPanels::c_Streaming, EditorPanels::c_Performance, EditorPanels::c_Activity })
		{
			CAPTURE(instrument);
			CHECK_FALSE(panels.IsOpen(instrument));
			panels.Focus(instrument);
			editor.Frames(2);
			ImGuiWindow* window = editor.FindPanelWindow(instrument);
			REQUIRE(window);
			CHECK(window->DockNode == console->DockNode);
			CHECK_FALSE(window->Hidden);
		}
		CHECK(viewport->GetImageArea().Size.x * viewport->GetImageArea().Size.y == doctest::Approx(image.x * image.y));
	}

	TEST_CASE("A layout saved before the Problems and Activity panels gives way to the default layout, which docks them with the Console")
	{
		// A laptop-sized window at 100%, where a floating panel would cover the viewport.
		const ImVec2 display(1600.0f, 900.0f);
		std::string settings;
		{
			HarnessEditor editor({ display, 1.0f }, WithFeatureProject("LayoutBeforeProblems"));
			editor.Frames(3);
			size_t size = 0;
			settings = ImGui::SaveIniSettingsToMemory(&size);
		}
		REQUIRE(settings.find("[Window][Problems]") != std::string::npos);
		// Wave 2's editors had neither panel and wrote layout version 3.
		const std::string older = WithoutPanels(settings, { EditorPanels::c_Problems, EditorPanels::c_Activity }, 3);
		REQUIRE(older.find("[Window][Problems]") == std::string::npos);
		REQUIRE(older.find("Problems=") == std::string::npos);
		REQUIRE(older.find("[StrataPanels][Layout]\nVersion=3\n") != std::string::npos);

		HarnessEditor editor({ display, 1.0f }, WithFeatureProject("LayoutBeforeProblemsReopened"), older);
		editor.Frames(3);
		EditorPanelRegistry& panels = editor.Layer->GetPanels();
		ImGuiWindow* console = editor.FindPanelWindow(EditorPanels::c_Console);
		ImGuiWindow* contentBrowser = editor.FindPanelWindow(EditorPanels::c_ContentBrowser);
		ImGuiWindow* problems = editor.FindPanelWindow(EditorPanels::c_Problems);
		REQUIRE(console);
		REQUIRE(contentBrowser);
		REQUIRE(problems);
		// Problems is open from the start, a tab of the bottom area behind the Content Browser, not a window over the viewport.
		CHECK(panels.IsOpen(EditorPanels::c_Problems));
		REQUIRE(console->DockNode);
		CHECK(problems->DockNode == console->DockNode);
		CHECK(problems->Hidden);
		CHECK_FALSE(contentBrowser->Hidden);
		// Activity is closed until asked for, and then joins them.
		CHECK_FALSE(panels.IsOpen(EditorPanels::c_Activity));
		panels.Focus(EditorPanels::c_Activity);
		editor.Frames(2);
		ImGuiWindow* activity = editor.FindPanelWindow(EditorPanels::c_Activity);
		REQUIRE(activity);
		CHECK(activity->DockNode == console->DockNode);
		CHECK_FALSE(activity->Hidden);
		// The editor saves the current version, so the default layout is built once.
		size_t size = 0;
		const std::string saved = ImGui::SaveIniSettingsToMemory(&size);
		CHECK(saved.find("[StrataPanels][Layout]\nVersion=" + std::to_string(EditorLayer::c_LayoutVersion)) != std::string::npos);
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
		const nlohmann::json status = editor.Run("editor.status", { { "sections", { "editor" } } });
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

	TEST_CASE("The status bar's pills fit side by side in a narrow window: compact at 150% while all show, full again with room")
	{
		const std::filesystem::path sessions = CreateTemporaryDirectory("StatusPillsSessions");
		ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessions));
		// 1600 x 900 pixels at 150%: about 1067 points of status bar, two thirds of the editor's 1600x900 window at that scale.
		const ImVec2 display(1600.0f, 900.0f);
		HarnessEditor editor({ display, 1.5f }, WithFeatureProject("StatusPills"), {}, true);
		editor.Frames(2);
		RpcClient first;
		RpcClient second;
		ShowEveryStatusPill(editor, sessions, first, second);

		// Their full texts would not fit: every pill shows, in its compact form (the details are in the tooltips).
		const float right = CheckPillsSideBySide(c_EveryStatusPill);
		CHECK(right <= display.x);
		MESSAGE("Free: " << display.x - right << " of " << display.x << " pixels");
		const Ref<ScriptEngine> engine = editor.Context().GetScriptEngine();
		REQUIRE(engine);
		CHECK(editor.GetPillText("Status.PlayState") == "Playing");
		CHECK(editor.GetPillText("Status.Frame").find("job") == std::string::npos);
		CHECK(editor.GetPillText("Status.Assets").find(" MB") == std::string::npos);
		CHECK(editor.GetPillText("Status.Automation") == "2 agents");
		CHECK(editor.GetPillText("Status.Scripts") == fmt::format("{} classes", engine->GetClasses().size()));
		CHECK(editor.GetPillText("Status.InputHolds") == "3 held");

		// With the holds released there is room for the full texts again.
		editor.RunToCompletion("input.releaseAll", { { "wait", false } });
		editor.Frames(3);
		CHECK_FALSE(UI::ItemProbe::Find("Status.InputHolds").has_value());
		CHECK(CheckPillsSideBySide({ "Status.PlayState", "Status.Frame", "Status.Assets", "Status.Automation", "Status.Scripts", "Status.Errors" })
			<= display.x);
		CHECK(editor.GetPillText("Status.Frame").find("job") != std::string::npos);
		CHECK(editor.GetPillText("Status.Automation") == "2 agents connected \xC2\xB7 Claude Code");
		CHECK(editor.GetPillText("Status.Scripts").starts_with(engine->GetModuleName() + " \xC2\xB7 "));
		editor.Run("play.stop");
	}

	TEST_CASE("In the editor's 1600x900 window the status bar's pills keep their full texts at 150% and at 100% while all show")
	{
		// The window --windowed opens (a laptop's): 1600 x 900 points, 2400 x 1350 pixels at 150%.
		const ImGuiHarness::Specification windows[] = { { ImVec2(2400.0f, 1350.0f), 1.5f }, { ImVec2(1600.0f, 900.0f), 1.0f } };
		for (const ImGuiHarness::Specification& window : windows)
		{
			CAPTURE(window.ContentScale);
			const std::filesystem::path sessions = CreateTemporaryDirectory("StatusPillsFullSessions");
			ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessions));
			HarnessEditor editor(window, WithFeatureProject("StatusPillsFull"), {}, true);
			editor.Frames(2);
			RpcClient first;
			RpcClient second;
			ShowEveryStatusPill(editor, sessions, first, second);

			CHECK(CheckPillsSideBySide(c_EveryStatusPill) <= window.DisplaySize.x);
			CHECK(editor.GetPillText("Status.Automation") == "2 agents connected \xC2\xB7 Claude Code");
			CHECK(editor.GetPillText("Status.InputHolds") == "Holds W, LeftShift, Mouse Left");
			CHECK(editor.GetPillText("Status.Scripts").starts_with(editor.Context().GetScriptEngine()->GetModuleName()));
			editor.Run("play.stop");
		}
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

	TEST_CASE("The scripts pill names the loaded module and counts its classes")
	{
		HarnessEditor editor({}, WithFeatureProject("EditorUIScriptClasses"));
		editor.Frames(2);

		// A module with one class says so in the singular.
		editor.Run("script.load", { { "path", FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_NEWERSDK)) } });
		editor.Frames(1);
		CHECK(editor.GetPillText("Status.Scripts") == "NewerSDK \xC2\xB7 1 class");

		// Another module replaces it; more classes are counted in the plural.
		editor.Run("script.load", { { "path", FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_FAULTS)) } });
		editor.Frames(1);
		const Ref<ScriptEngine>& engine = editor.Context().GetScriptEngine();
		REQUIRE(engine);
		REQUIRE(engine->GetClasses().size() > 1);
		CHECK(editor.GetPillText("Status.Scripts") == fmt::format("{} \xC2\xB7 {} classes", engine->GetModuleName(), engine->GetClasses().size()));
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
		const nlohmann::json status = editor.Run("editor.status", { { "sections", { "editor" } } });
		CHECK(status["editor"]["frameRate"]["frameMilliseconds"].get<double>() == doctest::Approx(4.0));
		// editor.wait reports the same measurement, recorded per frame from the host.
		CHECK(editor.Context().GetLastFrameTime() == doctest::Approx(4.0));
	}

	TEST_CASE("The viewport's chips are in a toolbar above its image, never over what the game draws")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIViewportToolbar"));
		editor.Frames(2);
		const auto checkChips = [](std::initializer_list<const char*> chips, const char* imageKey = "Viewport.Image")
		{
			const std::optional<UI::ItemProbe::Item> image = UI::ItemProbe::Find(imageKey);
			REQUIRE(image.has_value());
			REQUIRE(image->Max.y > image->Min.y);
			for (const char* key : chips)
			{
				CAPTURE(key);
				const std::optional<UI::ItemProbe::Item> chip = UI::ItemProbe::Find(key);
				REQUIRE(chip.has_value());
				// Rectangles that do not intersect: the chip ends above the image's first row, so it covers nothing the game
				// draws in it (its screen-space text, its canvases).
				CHECK(chip->Max.y <= image->Min.y);
			}
		};
		checkChips({ "Viewport.Camera", "Viewport.Grid", "Viewport.Outline", "Viewport.Gizmos", "Viewport.Stats", "Viewport.PreviewLighting",
			"Viewport.GameUI", "Viewport.DebugView" });
		// The image keeps the panel's full width and starts below the toolbar.
		const ViewportPanel* viewport = editor.Layer->GetPanels().Get<ViewportPanel>(EditorPanels::c_Viewport);
		REQUIRE(viewport);
		const ViewportImageArea& area = viewport->GetImageArea();
		CHECK(area.Min.y == doctest::Approx(UI::ItemProbe::Find("Viewport.Image")->Min.y));

		// Playing, the Game view comes forward: its chips are above its image too, also while the game has the input.
		REQUIRE(editor.Click("Toolbar.Play"));
		editor.Frames(2);
		REQUIRE(editor.Context().IsPlaying());
		const GameViewPanel* game = editor.Layer->GetPanels().Get<GameViewPanel>(EditorPanels::c_Game);
		REQUIRE(game);
		REQUIRE(game->IsShown());
		checkChips({ "GameView.Resolution", "GameView.Stats" }, "GameView.Image");
		REQUIRE(editor.Context().IsGameInputActive());
		const ViewportImageArea& gameImage = game->GetImageArea();
		editor.ClickAt(ImVec2(gameImage.Min.x + gameImage.Size.x * 0.5f, gameImage.Min.y + gameImage.Size.y * 0.5f));
		editor.Frames(1);
		REQUIRE(editor.Context().IsGameInputActive());
		checkChips({ "GameView.Resolution", "GameView.Stats" }, "GameView.Image");
		REQUIRE(editor.Click("Toolbar.Stop"));
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

	TEST_CASE("The viewport's debug view chip lights while a debug view colors the image")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIDebugView"));
		editor.Frames(2);
		const UI::ThemeColors& colors = UI::GetThemeColors();
		REQUIRE(UI::ItemProbe::Find("Viewport.DebugView").has_value());
		const ImU32 off = UI::ItemProbe::Find("Viewport.DebugView")->Color;
		CHECK(off != ToColor(colors.Accent));
		// Named after what the image shows, as in its menu.
		CHECK(UI::ItemProbe::Find("Viewport.DebugView")->Text == "Lit");

		editor.Run("viewport.setSettings", { { "debugView", "lod" } });
		editor.Frames(1);
		CHECK(UI::ItemProbe::Find("Viewport.DebugView")->Color == ToColor(colors.Accent));
		CHECK(UI::ItemProbe::Find("Viewport.DebugView")->Text == "LOD");

		// The streaming view colors loading and failed assets as the Streaming panel does: a warning and an error.
		const ViewportDebugColors& debugColors = editor.Context().GetViewport().GetSettings().DebugColors;
		CHECK(ToColor(ImVec4(debugColors.Pending.r, debugColors.Pending.g, debugColors.Pending.b, 1.0f)) == ToColor(GetAssetStateColor(AssetState::Loading)));
		CHECK(ToColor(ImVec4(debugColors.Failed.r, debugColors.Failed.g, debugColors.Failed.b, 1.0f)) == ToColor(GetAssetStateColor(AssetState::Failed)));
		CHECK(ToColor(ImVec4(debugColors.Pending.r, debugColors.Pending.g, debugColors.Pending.b, 1.0f)) == ToColor(colors.Warning));
		CHECK(ToColor(ImVec4(debugColors.Failed.r, debugColors.Failed.g, debugColors.Failed.b, 1.0f)) == ToColor(colors.Error));
		CHECK(ToColor(ImVec4(debugColors.LOD[0].r, debugColors.LOD[0].g, debugColors.LOD[0].b, 1.0f)) == ToColor(colors.Success));
		// The mip view tells the levels textures lack apart with the same meanings: as needed is a success, 3 or more an error.
		CHECK(ToColor(ImVec4(debugColors.Mip[0].r, debugColors.Mip[0].g, debugColors.Mip[0].b, 1.0f)) == ToColor(colors.Success));
		CHECK(ToColor(ImVec4(debugColors.Mip[1].r, debugColors.Mip[1].g, debugColors.Mip[1].b, 1.0f)) == ToColor(colors.Info));
		CHECK(ToColor(ImVec4(debugColors.Mip[2].r, debugColors.Mip[2].g, debugColors.Mip[2].b, 1.0f)) == ToColor(colors.Warning));
		CHECK(ToColor(ImVec4(debugColors.Mip[3].r, debugColors.Mip[3].g, debugColors.Mip[3].b, 1.0f)) == ToColor(colors.Error));
		editor.Run("viewport.setSettings", { { "debugView", "mip" } });
		editor.Frames(1);
		CHECK(UI::ItemProbe::Find("Viewport.DebugView")->Text == "Mip");
		CHECK(ImGuiHarness::CountFallbackGlyphs(editor.FindPanelWindow(EditorPanels::c_Viewport)) == 0);

		// Clicking the chip offers the views instead of toggling.
		REQUIRE(editor.Click("Viewport.DebugView"));
		CHECK(editor.Context().GetViewport().GetSettings().DebugView == ViewportDebugView::Mip);
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

	TEST_CASE("The Game view is a tab beside the Scene view, and Play brings it forward and gives the game the keyboard")
	{
		ScopedAudioEngine audio;
		REQUIRE(audio.Initialized);
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIGameView"));
		editor.Frames(2);
		EditorPanelRegistry& panels = editor.Layer->GetPanels();
		REQUIRE(panels.FindDescriptor(EditorPanels::c_Game));
		CHECK(panels.FindDescriptor(EditorPanels::c_Viewport)->Title == "Scene");
		ImGuiWindow* sceneWindow = editor.FindPanelWindow(EditorPanels::c_Viewport);
		ImGuiWindow* gameWindow = editor.FindPanelWindow(EditorPanels::c_Game);
		REQUIRE(sceneWindow);
		REQUIRE(gameWindow);
		REQUIRE(sceneWindow->DockNode);
		CHECK(gameWindow->DockNode == sceneWindow->DockNode);
		const GameViewPanel* game = panels.Get<GameViewPanel>(EditorPanels::c_Game);
		const ViewportPanel* scene = panels.Get<ViewportPanel>(EditorPanels::c_Viewport);
		REQUIRE(game);
		REQUIRE(scene);
		// The Scene view in front; the Game view renders nothing behind it.
		CHECK_FALSE(game->IsShown());
		CHECK(UI::ItemProbe::Find("Viewport.Image").has_value());
		CHECK(editor.Context().GetViewport().GetGameViewSize() == glm::uvec2(0));
		CHECK_FALSE(editor.Context().IsGameInputActive());

		// Play brings it forward with the keyboard: the game has the input at once.
		REQUIRE(editor.Click("Toolbar.Play"));
		editor.Frames(3);
		REQUIRE(editor.Context().IsPlaying());
		CHECK(game->IsShown());
		CHECK(editor.Context().IsGameInputActive());
		CHECK(Input::IsEnabled());
		const glm::uvec2 gameSize = editor.Context().GetViewport().GetGameViewSize();
		CHECK(gameSize == game->GetRenderSize());
		CHECK(gameSize.x > 0);

		// Shift+F1 leaves it: the game loses the input, the view stays.
		editor.Harness.SetKey(ImGuiMod_Shift, true);
		editor.Harness.SetKey(ImGuiKey_F1, true);
		editor.Frames(1);
		editor.Harness.SetKey(ImGuiKey_F1, false);
		editor.Harness.SetKey(ImGuiMod_Shift, false);
		editor.Frames(2);
		CHECK_FALSE(editor.Context().IsGameInputActive());
		CHECK_FALSE(Input::IsEnabled());
		CHECK(game->IsShown());
		// A click in the image hands it back; the toolbar's chips never do.
		REQUIRE(editor.Click("GameView.Stats"));
		CHECK_FALSE(editor.Context().IsGameInputActive());
		REQUIRE(editor.Click("GameView.Stats"));
		const ViewportImageArea& image = game->GetImageArea();
		editor.ClickAt(ImVec2(image.Min.x + image.Size.x * 0.5f, image.Min.y + image.Size.y * 0.5f));
		editor.Frames(1);
		CHECK(editor.Context().IsGameInputActive());

		// Stop brings the Scene view back, and the game's input goes.
		REQUIRE(editor.Click("Toolbar.Stop"));
		editor.Frames(3);
		CHECK_FALSE(editor.Context().IsPlaying());
		CHECK_FALSE(game->IsShown());
		CHECK(UI::ItemProbe::Find("Viewport.Image").has_value());
		CHECK_FALSE(editor.Context().IsGameInputActive());

		// The Game view shown while editing shows the edited scene's camera; it hands no input to a game that does not run.
		panels.Focus(EditorPanels::c_Game);
		editor.Frames(3);
		CHECK(game->IsShown());
		CHECK_FALSE(editor.Context().IsGameInputActive());
		CHECK_FALSE(UI::ItemProbe::Find("GameView.NoCamera").has_value());
		// editor.showPanel names it, and the Scene view by its old id or its title.
		editor.Run("editor.showPanel", { { "panel", "Scene" } });
		editor.Frames(3);
		CHECK_FALSE(game->IsShown());
		editor.Run("editor.showPanel", { { "panel", "Game" } });
		editor.Frames(3);
		CHECK(game->IsShown());
	}

	TEST_CASE("The Game view letterboxes aspect ratios and scales fixed resolutions to fit")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIGameViewSizes"));
		editor.Frames(2);
		EditorPanelRegistry& panels = editor.Layer->GetPanels();
		panels.Focus(EditorPanels::c_Game);
		editor.Frames(3);
		const GameViewPanel* game = panels.Get<GameViewPanel>(EditorPanels::c_Game);
		REQUIRE(game);
		REQUIRE(game->IsShown());
		const auto area = [&]()
		{
			// The image's area: what the panel shows under its toolbar (the image fills it in the free mode).
			editor.Run("viewport.setSettings", { { "gameView", { { "mode", "free" } } } });
			editor.Frames(2);
			return game->GetImageArea();
		};
		const ViewportImageArea free = area();
		CHECK(game->GetRenderSize() == free.GetPixelSize());
		const auto checkAspect = [&](const char* mode, float aspect)
		{
			CAPTURE(mode);
			editor.Run("viewport.setSettings", { { "gameView", { { "mode", mode } } } });
			editor.Frames(2);
			const ViewportImageArea& image = game->GetImageArea();
			const glm::uvec2 pixels = game->GetRenderSize();
			// The rendered image keeps the aspect ratio within a pixel...
			CHECK(std::abs(static_cast<float>(pixels.x) / aspect - static_cast<float>(pixels.y)) <= 1.0f);
			// ...and so does what shows, the largest such rectangle inside the area, centered with bars on two sides.
			CHECK(std::abs(image.Size.x / aspect - image.Size.y) <= 1.0f);
			const bool fillsWidth = std::abs(image.Size.x - free.Size.x) <= 1.0f;
			const bool fillsHeight = std::abs(image.Size.y - free.Size.y) <= 1.0f;
			CHECK(fillsWidth != fillsHeight);
			CHECK(image.Min.x >= free.Min.x - 0.5f);
			CHECK(image.Min.y >= free.Min.y - 0.5f);
			CHECK(image.Min.x + image.Size.x <= free.Min.x + free.Size.x + 0.5f);
			CHECK(image.Min.y + image.Size.y <= free.Min.y + free.Size.y + 0.5f);
		};
		checkAspect("16:9", 16.0f / 9.0f);
		checkAspect("4:3", 4.0f / 3.0f);
		checkAspect("21:9", 21.0f / 9.0f);
		// A fixed resolution renders at exactly its pixels, shown scaled to fit.
		editor.Run("viewport.setSettings", { { "gameView", { { "mode", "1920x1080" } } } });
		editor.Frames(2);
		CHECK(game->GetRenderSize() == glm::uvec2(1920, 1080));
		CHECK(std::abs(game->GetImageArea().Size.x / (16.0f / 9.0f) - game->GetImageArea().Size.y) <= 1.0f);
		editor.Run("viewport.setSettings", { { "gameView", { { "mode", "custom" }, { "width", 640 }, { "height", 480 } } } });
		editor.Frames(2);
		CHECK(game->GetRenderSize() == glm::uvec2(640, 480));
		CHECK(editor.Run("viewport.getSettings")["gameView"] == nlohmann::json { { "mode", "custom" }, { "width", 640 }, { "height", 480 } });
		// The resolution menu shows the mode and switches it.
		CHECK(UI::ItemProbe::Find("GameView.Resolution")->Text == "640 x 480");
	}

	TEST_CASE("The Scene view keeps the editor camera while playing, and the Game camera toggle (F8) looks through the scene's")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIGameCamera"));
		editor.Frames(2);
		const ViewportPanel* scene = editor.Layer->GetPanels().Get<ViewportPanel>(EditorPanels::c_Viewport);
		REQUIRE(scene);
		ViewportSettings& settings = editor.Context().GetViewport().GetSettings();
		CHECK_FALSE(scene->IsPiloting());
		REQUIRE(editor.Click("Viewport.GameCamera"));
		editor.Frames(1);
		CHECK(settings.PilotGameCamera);
		CHECK(scene->IsPiloting());
		// F8 with the Scene view under the mouse toggles it back.
		const ViewportImageArea& image = scene->GetImageArea();
		editor.Harness.MoveMouse(ImVec2(image.Min.x + image.Size.x * 0.5f, image.Min.y + image.Size.y * 0.5f));
		editor.Frames(1);
		editor.Harness.SetKey(ImGuiKey_F8, true);
		editor.Frames(1);
		editor.Harness.SetKey(ImGuiKey_F8, false);
		editor.Frames(1);
		CHECK_FALSE(settings.PilotGameCamera);
		CHECK_FALSE(scene->IsPiloting());

		// Playing, the Scene view stays an editor view of the running scene: all its chips, the editor camera.
		editor.Run("play.start", { { "build", false } });
		editor.Frames(2);
		REQUIRE(editor.Context().IsPlaying());
		editor.Layer->GetPanels().Focus(EditorPanels::c_Viewport);
		editor.Frames(3);
		for (const char* chip : { "Viewport.Camera", "Viewport.Grid", "Viewport.Gizmos", "Viewport.PreviewLighting", "Viewport.GameUI", "Viewport.GameCamera" })
		{
			CAPTURE(chip);
			CHECK(UI::ItemProbe::Find(chip).has_value());
		}
		CHECK_FALSE(scene->IsPiloting());
		CHECK_FALSE(editor.Context().IsGameInputActive());
		editor.Run("play.stop");
	}

	TEST_CASE("A cursor the game locks stays in the game view, which says how to free it; Shift+F1 and stopping free it")
	{
		ScopedAudioEngine audio;
		REQUIRE(audio.Initialized);
		HarnessEditor editor({}, WithFeatureProject("EditorUICursorLock"));
		editor.Run("script.load", { { "path", FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)) } });
		const nlohmann::json created = editor.Run("entity.create", { { "name", "Mouse Look" } });
		editor.Run("script.add", { { "entity", created["id"] }, { "class", "CursorProbe" } });
		editor.Frames(2);
		const GameViewPanel* viewport = editor.Layer->GetPanels().Get<GameViewPanel>(EditorPanels::c_Game);
		REQUIRE(viewport);
		const ImGuiIO& io = ImGui::GetIO();

		// Playing (started by the command: the Game view does not come forward), the game asked for a locked cursor; until
		// the Game view has the input the editor keeps the mouse.
		editor.Run("play.start", { { "build", false } });
		editor.Layer->GetPanels().ShowTab(EditorPanels::c_Game);
		editor.Frames(2);
		REQUIRE(editor.Context().IsPlaying());
		REQUIRE(viewport->IsShown());
		CHECK(Input::GetGameCursorMode() == CursorMode::Locked);
		CHECK(Input::GetCursorMode() == CursorMode::Normal);
		CHECK((io.ConfigFlags & ImGuiConfigFlags_NoMouse) == 0);
		CHECK(viewport->GetShownMouseHint().empty());

		// Clicking the game gives it the input: the cursor locks, the editor's UI ignores the mouse, and the view says how to
		// get it back.
		const ViewportImageArea& image = viewport->GetImageArea();
		const ImVec2 center(image.Min.x + image.Size.x * 0.5f, image.Min.y + image.Size.y * 0.5f);
		editor.ClickAt(center);
		editor.Frames(1);
		REQUIRE(editor.Context().IsGameInputActive());
		CHECK(Input::GetCursorMode() == CursorMode::Locked);
		CHECK((io.ConfigFlags & ImGuiConfigFlags_NoMouse) != 0);
		CHECK(viewport->GetShownMouseHint() == GameViewPanel::c_MouseHint);

		// Shift+F1 leaves the game view: the cursor and the mouse are the editor's again, and the hint goes.
		editor.Harness.SetKey(ImGuiMod_Shift, true);
		editor.Harness.SetKey(ImGuiKey_F1, true);
		editor.Frames(1);
		editor.Harness.SetKey(ImGuiKey_F1, false);
		editor.Harness.SetKey(ImGuiMod_Shift, false);
		editor.Frames(2);
		CHECK_FALSE(editor.Context().IsGameInputActive());
		CHECK(Input::GetCursorMode() == CursorMode::Normal);
		CHECK((io.ConfigFlags & ImGuiConfigFlags_NoMouse) == 0);
		CHECK(viewport->GetShownMouseHint().empty());

		// Clicking back locks it again, with the hint, which goes after a few seconds while the cursor stays locked.
		editor.ClickAt(center);
		editor.Frames(1);
		CHECK(Input::GetCursorMode() == CursorMode::Locked);
		CHECK((io.ConfigFlags & ImGuiConfigFlags_NoMouse) != 0);
		CHECK_FALSE(viewport->GetShownMouseHint().empty());
		editor.Frames(static_cast<int>(GameViewPanel::c_MouseHintSeconds) + 1, 1.0f);
		CHECK(viewport->GetShownMouseHint().empty());
		CHECK((io.ConfigFlags & ImGuiConfigFlags_NoMouse) != 0);

		// Stopping (Ctrl+P, here the command) gives everything back.
		editor.Run("play.stop");
		editor.Frames(2);
		CHECK(Input::GetGameCursorMode() == CursorMode::Normal);
		CHECK(Input::GetCursorMode() == CursorMode::Normal);
		CHECK((io.ConfigFlags & ImGuiConfigFlags_NoMouse) == 0);
		CHECK(viewport->GetShownMouseHint().empty());
	}

	TEST_CASE("The game view says what the game asked of its window, which play mode does not apply")
	{
		ScopedAudioEngine audio;
		REQUIRE(audio.Initialized);
		HarnessEditor editor({}, WithFeatureProject("EditorUIGameWindow"));
		editor.Run("script.load", { { "path", FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)) } });
		const nlohmann::json created = editor.Run("entity.create", { { "name", "Display Options" } });
		editor.Run("script.add", { { "entity", created["id"] }, { "class", "WindowRequests" }, { "fields", { { "Mode", 1 }, { "Width", 1920 }, { "Height", 1080 } } } });
		editor.Frames(2);
		const GameViewPanel* viewport = editor.Layer->GetPanels().Get<GameViewPanel>(EditorPanels::c_Game);
		REQUIRE(viewport);
		CHECK(viewport->GetShownGameWindowNotice().empty());

		REQUIRE(editor.Click("Toolbar.Play"));
		editor.Frames(3);
		REQUIRE(editor.Context().IsPlaying());
		REQUIRE(viewport->IsShown());
		// The requests reach the editor's game window service only (EditorHost offers no way to change the editor's window).
		CHECK(viewport->GetShownGameWindowNotice() == "The game asked for fullscreen at 1920 x 1080 pixels with vsync off; play mode keeps the editor's window.");
		CHECK(editor.Run("editor.status", { { "sections", { "game" } } })["game"]["windowRequests"]["mode"] == "fullscreen");
		// The "Game's request" mode renders at the resolution the game asked for.
		editor.Run("viewport.setSettings", { { "gameView", { { "mode", "game" } } } });
		editor.Frames(2);
		CHECK(viewport->GetRenderSize() == glm::uvec2(1920, 1080));
		CHECK(UI::ItemProbe::Find("GameView.Resolution")->Text == "Game's 1920 x 1080");

		REQUIRE(editor.Click("Toolbar.Stop"));
		editor.Frames(1);
		CHECK(viewport->GetShownGameWindowNotice().empty());
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
