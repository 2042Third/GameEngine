#include <doctest/doctest.h>

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ImGuiHarness.h"
#include "Panels/PerformancePanel.h"
#include "Panels/StreamingPanel.h"
#include "Perf/PerfUtils.h"
#include "Renderer/SceneRendererTestUtils.h"

#include <Strata/Core/FrameProfiler.h>
#include <Strata/Renderer/Material.h>

#include <imgui.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_WarmupFrames = 30;
	constexpr uint32_t c_MeasuredFrames = 240; // Four seconds of harness frames: the Streaming table refreshes 16 times
	constexpr uint32_t c_RegisteredAssets = 10'000;
	// The streaming observatory's acceptance limits for the panels' cost per frame, in milliseconds.
	constexpr double c_MaxStreamingPanelMs = 0.3;
	constexpr double c_MaxPerformancePanelMs = 0.2;

	// Draws a panel in a window of its own for a number of harness frames and returns its cost per frame (OnUpdate and
	// OnImGuiRender, timed inside the frame) in milliseconds.
	std::vector<double> MeasurePanel(ImGuiHarness& harness, EditorPanel& panel, EditorPanelContext& context, uint32_t frames)
	{
		std::vector<double> samples;
		samples.reserve(frames);
		for (uint32_t frame = 0; frame < frames; frame++)
		{
			harness.Frame([&]()
			{
				ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
				ImGui::SetNextWindowSize(ImVec2(1200.0f, 800.0f));
				ImGui::Begin("Panel", nullptr, ImGuiWindowFlags_NoSavedSettings);
				const auto start = std::chrono::steady_clock::now();
				panel.OnUpdate(context);
				panel.OnImGuiRender(context);
				samples.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
				ImGui::End();
			});
		}
		return samples;
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
		REQUIRE(streaming.GetTableRowCount() >= c_RegisteredAssets);
		const std::vector<double> streamingSamples = MeasurePanel(harness, streaming, panelContext, c_MeasuredFrames);
		const Perf::Measurement streamingStats = Perf::Summarize(streamingSamples);
		const double streamingMean = Mean(streamingSamples);
		MESSAGE("Streaming panel with ", c_RegisteredAssets, " assets: mean ", streamingMean, " ms, median ", streamingStats.MedianMs, " ms, p95 ",
			streamingStats.P95Ms, " ms, max ", streamingStats.MaxMs, " ms per frame");

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
		const std::vector<double> performanceSamples = MeasurePanel(harness, performance, panelContext, c_MeasuredFrames);
		const Perf::Measurement performanceStats = Perf::Summarize(performanceSamples);
		const double performanceMean = Mean(performanceSamples);
		MESSAGE("Performance panel with ", profiler.GetRecordedFrameCount(), " frames: mean ", performanceMean, " ms, median ", performanceStats.MedianMs,
			" ms, p95 ", performanceStats.P95Ms, " ms, max ", performanceStats.MaxMs, " ms per frame");

		// Per frame on average: the table refresh and the statistics are spread over frames.
		CHECK(streamingMean <= c_MaxStreamingPanelMs);
		CHECK(performanceMean <= c_MaxPerformancePanelMs);
		Perf::CheckBudget("Perf.EditorPanels.Streaming10kAssets", streamingMean);
		Perf::CheckBudget("Perf.EditorPanels.Performance600Frames", performanceMean);
	}
}
