#include <doctest/doctest.h>

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ImGuiHarness.h"
#include "Panels/ContentBrowserPanel.h"
#include "Panels/PerformancePanel.h"
#include "Panels/StreamingPanel.h"
#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/FrameProfiler.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Renderer/Material.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_WarmupFrames = 30;
	constexpr uint32_t c_MeasuredFrames = 240; // Four seconds of harness frames: the Streaming table refreshes up to 16 times
	constexpr uint32_t c_RegisteredAssets = 10'000;
	constexpr uint32_t c_BrowserAssets = 10'001;
	// The Streaming table's first build (every name of 10,000 assets) gets this long to finish on the job threads.
	constexpr std::chrono::seconds c_FirstBuildTimeout(30);

	// What a panel cost per frame (OnUpdate and OnImGuiRender, timed inside the frame) in milliseconds, and separately the
	// frames in which `refreshes` (when given) counted up.
	struct PanelFrames
	{
		std::vector<double> All;
		std::vector<double> Refreshes;
	};

	// Draws a panel in a window of its own for a number of harness frames.
	PanelFrames MeasurePanel(ImGuiHarness& harness, EditorPanel& panel, EditorPanelContext& context, uint32_t frames,
		const std::function<uint64_t()>& refreshes = {})
	{
		PanelFrames result;
		result.All.reserve(frames);
		for (uint32_t frame = 0; frame < frames; frame++)
		{
			const uint64_t refreshesBefore = refreshes ? refreshes() : 0;
			double cost = 0.0;
			harness.Frame([&]()
			{
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(ImVec2(1200.0f, 800.0f));
				ImGui::Begin("Panel", nullptr, ImGuiWindowFlags_NoSavedSettings);
				const auto start = std::chrono::steady_clock::now();
				panel.OnUpdate(context);
				panel.OnImGuiRender(context);
				cost = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				ImGui::End();
			});
			result.All.push_back(cost);
			if (refreshes && refreshes() != refreshesBefore)
				result.Refreshes.push_back(cost);
		}
		return result;
	}

	double Mean(const std::vector<double>& samples)
	{
		double sum = 0.0;
		for (const double sample : samples)
			sum += sample;
		return samples.empty() ? 0.0 : sum / static_cast<double>(samples.size());
	}

}

TEST_SUITE("Perf.EditorPanels")
{
	TEST_CASE("The Streaming panel with 10,000 assets and the Performance panel cost a fraction of a millisecond per frame")
	{
		// The job threads the editor has: the Streaming panel builds its table on them.
		Perf::ScopedApplicationJobSystem jobs;
		ImGuiHarness harness;
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		EditorCommandRunner runner;
		EditorPanelContext panelContext { context, commands, runner };

		// The Streaming panel: 10,000 registered assets besides the built-in ones, all resident (memory assets), in the
		// table.
		SceneTestAssets assets;
		for (uint32_t index = 0; index < c_RegisteredAssets; index++)
		{
			AssetMetadata metadata;
			metadata.Type = AssetType::Material;
			metadata.Name = "Materials/Perf" + std::to_string(index) + ".stmat";
			assets.Manager->AddMemoryAsset(Material::Create(), metadata);
		}
		REQUIRE(assets.Manager->GetStats().RegisteredAssets >= c_RegisteredAssets);
		StreamingPanel streaming;
		MeasurePanel(harness, streaming, panelContext, c_WarmupFrames);
		const auto deadline = std::chrono::steady_clock::now() + c_FirstBuildTimeout;
		while ((streaming.IsBuildingTable() || streaming.GetTableRowCount() < c_RegisteredAssets) && std::chrono::steady_clock::now() < deadline)
			MeasurePanel(harness, streaming, panelContext, 1);
		REQUIRE(streaming.GetTableRowCount() >= c_RegisteredAssets);
		// Per frame, not on average (the streaming observatory's acceptance limit, 0.3 ms per frame, is far above the
		// budgets): the 95th percentile frame, and the median of the frames that refresh the table, so that a periodic hitch
		// cannot hide among the others (the main thread copies the residency; a job names, sorts and filters it).
		Perf::CheckBudgetsConfirmed({ "Perf.EditorPanels.Streaming10kAssetsP95", "Perf.EditorPanels.Streaming10kAssetsRefreshFrame" }, [&]
		{
			const PanelFrames streamingFrames = MeasurePanel(harness, streaming, panelContext, c_MeasuredFrames, [&]() { return streaming.GetTableBuildCount(); });
			const Perf::Measurement streamingStats = Perf::Summarize(streamingFrames.All);
			// The table is read again four times per second of harness time, unless the previous build is still running.
			REQUIRE(streamingFrames.Refreshes.size() >= c_MeasuredFrames / 60);
			const Perf::Measurement refreshStats = Perf::Summarize(streamingFrames.Refreshes);
			MESSAGE("Streaming panel with ", c_RegisteredAssets, " assets: mean ", Mean(streamingFrames.All), " ms, median ", streamingStats.MedianMs, " ms, p95 ",
				streamingStats.P95Ms, " ms, max ", streamingStats.MaxMs, " ms per frame; ", streamingFrames.Refreshes.size(), " frames that refreshed the table: median ",
				refreshStats.MedianMs, " ms, max ", refreshStats.MaxMs, " ms");
			return Perf::MetricValues { { "Perf.EditorPanels.Streaming10kAssetsP95", streamingStats.P95Ms },
				{ "Perf.EditorPanels.Streaming10kAssetsRefreshFrame", refreshStats.MedianMs } };
		});

		// The Content Browser: 10,001 assets in one folder, shown as a grid.
		{
			EditorContext projectContext { EditorContextSpecification { false } };
			EditorPanelContext browserContext { projectContext, commands, runner };
			const std::filesystem::path project = CreateTemporaryDirectory("PerfContentBrowser") / "Game";
			const EditorCommandResult created = commands.Execute(projectContext, "project.create", { { "directory", FileSystem::ToUTF8(project) }, { "name", "Game" } });
			REQUIRE_MESSAGE(created.Success, created.Error);
			const std::string document = JsonUtils::Dump(Material::Create()->Serialize(), 1, '\t');
			for (uint32_t index = 0; index < c_BrowserAssets; index++)
				REQUIRE(FileSystem::WriteText(project / "Assets" / "Materials" / fmt::format("Perf{}.stmat", index), document));
			std::string error;
			REQUIRE_MESSAGE(projectContext.OpenProject(project, &error), error);
			ContentBrowserPanel browser;
			browser.SetCurrentDirectory("Materials");
			MeasurePanel(harness, browser, browserContext, c_WarmupFrames);
			REQUIRE(browser.GetView().GetItems().size() == c_BrowserAssets);
			const uint64_t rebuilds = browser.GetView().GetRebuildCount();
			// The 95th percentile frame (the acceptance limit, 0.5 ms, is the budget's ceiling). The rows are made when the
			// project's assets change, never periodically: an idle browser copies nothing.
			Perf::CheckBudgetConfirmed("Perf.EditorPanels.ContentBrowser10kAssetsP95", [&]
			{
				const PanelFrames browserFrames = MeasurePanel(harness, browser, browserContext, c_MeasuredFrames);
				const Perf::Measurement browserStats = Perf::Summarize(browserFrames.All);
				MESSAGE("Content Browser with ", c_BrowserAssets, " assets in a folder: mean ", Mean(browserFrames.All), " ms, median ", browserStats.MedianMs,
					" ms, p95 ", browserStats.P95Ms, " ms, max ", browserStats.MaxMs, " ms per frame");
				return browserStats.P95Ms;
			});
			CHECK(browser.GetView().GetRebuildCount() == rebuilds);
		}

		// The Performance panel: a full ring of frames with the editor's stages.
		FrameProfiler& profiler = FrameProfiler::Get();
		profiler.Reset();
		for (uint32_t frame = 0; frame < profiler.GetCapacity(); frame++)
		{
			profiler.BeginFrame();
			for (const BuiltinFrameStage& stage : c_BuiltinFrameStages)
			{
				const FrameStageScope scope(profiler, profiler.FindStage(stage.Name));
				const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(2);
				while (std::chrono::steady_clock::now() < end)
				{
				}
			}
			profiler.EndFrame(0.0001);
		}
		PerformancePanel performance;
		MeasurePanel(harness, performance, panelContext, c_WarmupFrames);
		// The 95th percentile frame (its acceptance limit, 0.2 ms, is far above the budget).
		Perf::CheckBudgetConfirmed("Perf.EditorPanels.Performance600FramesP95", [&]
		{
			const PanelFrames performanceFrames = MeasurePanel(harness, performance, panelContext, c_MeasuredFrames);
			const Perf::Measurement performanceStats = Perf::Summarize(performanceFrames.All);
			MESSAGE("Performance panel with ", profiler.GetRecordedFrameCount(), " frames: mean ", Mean(performanceFrames.All), " ms, median ",
				performanceStats.MedianMs, " ms, p95 ", performanceStats.P95Ms, " ms, max ", performanceStats.MaxMs, " ms per frame");
			return performanceStats.P95Ms;
		});
	}
}
