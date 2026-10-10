#include <doctest/doctest.h>

#include "Editor/CommandUtils.h"
#include "Editor/HarnessEditor.h"
#include "EditorLayer.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/PerformancePanel.h"
#include "Panels/StreamingPanel.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FrameProfiler.h>
#include <Strata/Core/JsonUtils.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	ImU32 ToColor(const ImVec4& color)
	{
		return UI::ToColorU32(color);
	}

	// Frames of the harness editor that the frame profiler records, as the application's are.
	void ProfiledFrames(HarnessEditor& editor, int count)
	{
		FrameProfiler& profiler = FrameProfiler::Get();
		for (int frame = 0; frame < count; frame++)
		{
			profiler.BeginFrame();
			editor.Frames(1);
			profiler.EndFrame(0.002);
		}
	}

}

TEST_SUITE("Editor.UI")
{
	TEST_CASE("Budget pressure is comfortable, high or over")
	{
		CHECK(GetBudgetPressure(10, AssetResidencyBudgets::c_Unlimited) == BudgetPressure::Unbudgeted);
		CHECK(GetBudgetPressure(0, 100) == BudgetPressure::Comfortable);
		CHECK(GetBudgetPressure(84, 100) == BudgetPressure::Comfortable);
		CHECK(GetBudgetPressure(85, 100) == BudgetPressure::High);
		CHECK(GetBudgetPressure(100, 100) == BudgetPressure::High);
		CHECK(GetBudgetPressure(101, 100) == BudgetPressure::Over);
		CHECK(GetBudgetPressure(1, 0) == BudgetPressure::Over);

		AssetManagerStats stats;
		CHECK(GetWorstPoolPressure(stats) == BudgetPressure::Unbudgeted);
		stats.Budgets.GpuTextures = 100;
		stats.Budgets.GpuBuffers = 100;
		stats.Resident.GpuTextures = 50;
		stats.Resident.GpuBuffers = 95;
		CHECK(GetWorstPoolPressure(stats) == BudgetPressure::High);
		stats.Resident.GpuTextures = 150;
		CHECK(GetWorstPoolPressure(stats) == BudgetPressure::Over);

		// The theme's meanings: the accent while there is room, a warning close to the budget, an error beyond it.
		const UI::ThemeColors& colors = UI::GetThemeColors();
		CHECK(ToColor(GetBudgetPressureColor(BudgetPressure::Comfortable)) == ToColor(colors.Accent));
		CHECK(ToColor(GetBudgetPressureColor(BudgetPressure::High)) == ToColor(colors.Warning));
		CHECK(ToColor(GetBudgetPressureColor(BudgetPressure::Over)) == ToColor(colors.Error));
		CHECK(ToColor(GetBudgetPressureColor(BudgetPressure::Unbudgeted)) == ToColor(colors.TextSecondary));
	}

	TEST_CASE("The assets pill opens the Streaming panel, which shows asset.stats of its frame and reveals assets in the Content Browser")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIStreaming"));
		REQUIRE(editor.Context().HasProject());
		editor.Frames(2);
		// Everything the project's scene uses arrives first, so that nothing changes between the panel's statistics and the
		// command's.
		EditorAssetManager* assets = editor.Context().GetAssetManager();
		REQUIRE(assets);
		REQUIRE(editor.FramesUntil([&]() { return !assets->HasPendingLoads(); }));

		const std::string pill = editor.GetPillText("Status.Assets");
		CHECK(pill.find("ready") != std::string::npos);
		CHECK(pill.find("streaming") != std::string::npos);
		CHECK(pill.find("MB") != std::string::npos);
		CHECK_FALSE(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming)); // An instrument, closed until asked for

		REQUIRE(editor.Click("Status.Assets"));
		editor.Frames(2);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming));
		ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Streaming);
		REQUIRE(window);
		CHECK(window->Active);
		// It joined the bottom area, beside the Console (no saved layout knew it).
		ImGuiWindow* console = editor.FindPanelWindow(EditorPanels::c_Console);
		REQUIRE(console);
		CHECK(window->DockId == console->DockId);

		// The totals are asset.stats' of the same frame: the panel reads them after the frame's commands ran.
		StreamingPanel* panel = editor.Layer->GetPanels().Get<StreamingPanel>(EditorPanels::c_Streaming);
		REQUIRE(panel);
		nlohmann::json command;
		editor.FrameWith([&]() { command = editor.Run("asset.stats"); });
		REQUIRE(panel->GetStats());
		CHECK(CommandUtils::DescribeAssetStats(*panel->GetStats()) == command);
		CHECK(command["registered"].get<uint32_t>() > 10);

		// The table lists the assets that are resident, loading, failed or pinned. Without a renderer nothing keeps the
		// project's assets in use, so the scene is pinned; filtered to the project's scenes, the table's first row is the
		// feature scene, and a click shows it in the Content Browser.
		CHECK(panel->GetTableRowCount() > 0);
		const AssetPin pin = assets->Pin(assets->FindAssetByPath("Scenes/Feature.stscene"));
		REQUIRE(pin.IsValid());
		REQUIRE(editor.ReplaceText("Streaming.Filter", "Scenes/Feature"));
		// The table is read again a few times per second.
		editor.Frames(static_cast<int>(StreamingPanel::c_TableRefreshSeconds / ImGuiHarness::c_DeltaTime) + 2);
		const std::optional<UI::ItemProbe::Item> first = UI::ItemProbe::Find("Streaming.FirstAsset");
		REQUIRE(first);
		CHECK(first->Enabled);
		REQUIRE(editor.Click("Streaming.FirstAsset"));
		editor.Frames(2);
		ContentBrowserPanel* contentBrowser = editor.Layer->GetPanels().Get<ContentBrowserPanel>(EditorPanels::c_ContentBrowser);
		REQUIRE(contentBrowser);
		CHECK(contentBrowser->GetRevealedAsset() == assets->FindAssetByPath("Scenes/Feature.stscene"));
		CHECK(contentBrowser->GetCurrentDirectory() == "Scenes");

		// Assets without a file are not revealed.
		CHECK_FALSE(contentBrowser->RevealAsset(editor.Context(), BuiltinAssets::CubeMesh));
		CHECK(contentBrowser->GetRevealedAsset() == assets->FindAssetByPath("Scenes/Feature.stscene"));
	}

	TEST_CASE("Instruments opened in a layout saved before they existed join the Console")
	{
		// A layout saved by an editor without the instruments: their windows' settings removed.
		std::string ini;
		{
			HarnessEditor first({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIOldLayout"));
			first.Frames(2);
			size_t size = 0;
			const char* data = ImGui::SaveIniSettingsToMemory(&size);
			ini.assign(data, size);
		}
		for (const char* id : { "###Streaming]", "###Performance]" })
		{
			size_t start = ini.find(id);
			while (start != std::string::npos)
			{
				start = ini.rfind("[Window][", start);
				REQUIRE(start != std::string::npos);
				const size_t end = ini.find("\n\n", start);
				ini.erase(start, end == std::string::npos ? std::string::npos : end + 2 - start);
				start = ini.find(id);
			}
		}

		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIOldLayoutReopened"), ini);
		editor.Frames(2);
		REQUIRE(editor.Click("Status.Frame"));
		editor.Frames(2);
		ImGuiWindow* performance = editor.FindPanelWindow(EditorPanels::c_Performance);
		ImGuiWindow* console = editor.FindPanelWindow(EditorPanels::c_Console);
		REQUIRE(performance);
		REQUIRE(console);
		CHECK(console->DockId != 0);
		CHECK(performance->DockId == console->DockId);
	}

	TEST_CASE("The assets pill shows budget pressure")
	{
		HarnessEditor editor({}, WithFeatureProject("EditorUIBudgetPressure"));
		editor.Frames(2);
		const UI::ThemeColors& colors = UI::GetThemeColors();
		// Without a graphics device the pools have no budgets: the pill is quiet.
		CHECK(editor.GetPillColor("Status.Assets") == ToColor(colors.TextSecondary));

		// The built-in assets alone (never evicted) exceed a tiny CPU budget.
		editor.Run("asset.setBudget", { { "cpuMB", 0.0001 } });
		editor.Frames(2);
		CHECK(editor.GetPillColor("Status.Assets") == ToColor(colors.Error));
		const std::string text = editor.GetPillText("Status.Assets");
		CHECK(text.find("/0 MB") != std::string::npos);

		// Room again: quiet again.
		editor.Run("asset.setBudget", { { "cpuMB", 100000 } });
		editor.Frames(2);
		CHECK(editor.GetPillColor("Status.Assets") == ToColor(colors.TextSecondary));
	}

	TEST_CASE("The frame pill counts jobs and opens the Performance panel, which copies editor.perf's statistics")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIPerformance"));
		FrameProfiler::Get().Reset();
		ProfiledFrames(editor, 3);
		CHECK(editor.GetPillText("Status.Frame").find("jobs") != std::string::npos);
		CHECK_FALSE(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Performance));

		REQUIRE(editor.Click("Status.Frame"));
		ProfiledFrames(editor, 3);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Performance));
		ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Performance);
		REQUIRE(window);
		CHECK(window->Active);
		CHECK(UI::ItemProbe::Find("Performance.Timeline").has_value());

		// Hovering a frame of the timeline explains it.
		const std::optional<UI::ItemProbe::Item> timeline = UI::ItemProbe::Find("Performance.Timeline");
		REQUIRE(timeline);
		editor.Harness.MoveMouse(ImVec2(timeline->Max.x - 1.0f, timeline->GetCenter().y));
		ProfiledFrames(editor, 2);
		CHECK(ImGui::FindWindowByName("##Tooltip_00") != nullptr);

		// Copy as JSON: what editor.perf returns for every recorded frame.
		REQUIRE(editor.Click("Performance.CopyJson"));
		std::string error;
		const std::optional<nlohmann::json> copied = JsonUtils::Parse(editor.Harness.GetClipboard(), &error);
		REQUIRE_MESSAGE(copied, error);
		const nlohmann::json perf = editor.Run("editor.perf", { { "frames", 600 } });
		for (const char* key : { "frames", "frame", "framesPerSecond", "stageSum", "untimed", "jobs", "stages" })
		{
			CAPTURE(key);
			CHECK(copied->contains(key));
		}
		CHECK((*copied)["frames"].get<uint32_t>() >= 8);
		CHECK((*copied)["stages"].size() == perf["stages"].size());
		CHECK((*copied)["stages"]["Assets.Update"]["avgMs"].get<float>() > 0.0f);
	}

	TEST_CASE("editor.showPanel opens a panel and brings it to the front")
	{
		HarnessEditor editor({ ImVec2(1600.0f, 900.0f), 1.0f }, WithFeatureProject("EditorUIShowPanel"));
		editor.Frames(2);
		CHECK_FALSE(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming));
		CHECK(editor.Run("editor.showPanel", { { "panel", "Streaming" } })["panel"] == "Streaming");
		editor.Frames(2);
		CHECK(editor.Layer->GetPanels().IsOpen(EditorPanels::c_Streaming));
		ImGuiWindow* window = editor.FindPanelWindow(EditorPanels::c_Streaming);
		REQUIRE(window);
		CHECK(window->Active);
		CHECK(window->DockTabIsVisible); // The visible tab of its dock node

		// Only the editor's docked panels; the launcher is no panel to show.
		for (const char* panel : { "Nope", "Welcome" })
		{
			CAPTURE(panel);
			const EditorCommandResult result = editor.Layer->GetCommands().Execute(editor.Context(), "editor.showPanel", { { "panel", panel } });
			CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
		}
	}
}
