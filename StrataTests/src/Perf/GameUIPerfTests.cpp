#include "Renderer/SceneRendererTestUtils.h"

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"

#include <Strata/Core/FrameProfiler.h>
#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/GameUI/GameUIScene.h>
#include <Strata/GameUI/GameUISystem.h>
#include <Strata/Input/Input.h>

#include <chrono>
#include <string>
#include <vector>

// The game UI's costs on its scaling scene (Perf::CreateUIWidgets: one canvas of 400 vertical layout panels of 12 buttons
// with labels, 10,001 elements): a full layout and an idle frame of the UI system (Perf.GameUI, CPU), and the UI pass's own
// CPU time with 200 elements changing per frame and with nothing changing (PerfGPU.GameUI: Release and Dist, with a GPU).

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr int c_Relayouts = 30;
	constexpr uint32_t c_IdleBatches = 300;
	constexpr uint32_t c_FramesPerBatch = 10; // An idle frame is below the clock's resolution
	constexpr int c_WarmupFrames = 20;
	constexpr int c_MeasuredFrames = 300;
	constexpr uint32_t c_Width = 1920;
	constexpr uint32_t c_Height = 1080;

	// The median CPU time of the UI pass (its Render.UI frame stage: Prepare and Draw recording, nothing else of the frame)
	// over frames run like an application's, `change` called before each.
	Perf::Measurement MeasureUIPass(GPUContext& gpu, SceneRenderer& renderer, Scene& scene, const std::function<void(int)>& change, std::string_view name)
	{
		FrameProfiler& profiler = FrameProfiler::Get();
		SceneCamera camera = LookAt({ 0.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, 0.0f }, static_cast<float>(c_Width) / static_cast<float>(c_Height));
		std::vector<double> samples;
		for (int frame = 0; frame < c_WarmupFrames + c_MeasuredFrames; frame++)
		{
			change(frame);
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			profiler.BeginFrame();
			REQUIRE(renderer.Render(scene, camera));
			profiler.EndFrame(0.0);
			gpu.GetDevice().EndFrame();
			const FrameStageID stage = profiler.FindStage("Render.UI");
			REQUIRE(stage != c_InvalidFrameStage);
			if (frame >= c_WarmupFrames)
				samples.push_back(static_cast<double>(profiler.GetFrame(0).StageMs[stage]));
		}
		const Perf::Measurement measurement = Perf::Summarize(samples);
		const SceneOverlayStats& stats = renderer.GetStats().Overlays;
		MESSAGE(name, ": ", measurement.MedianMs, " ms median (", stats.Elements, " elements, ", stats.Quads, " quads, ", stats.DrawCalls, " draw calls, ",
			stats.UploadedBytes, " bytes uploaded in the last frame)");
		return measurement;
	}

}

TEST_SUITE("Perf.GameUI")
{
	TEST_CASE("A full layout of 10,000 elements")
	{
		Scene scene("Widgets");
		Perf::CreateUIWidgets(scene);
		GameUIScene& ui = GameUIScene::Get(scene);
		ui.EnsureLayout({});
		ui.TakeStats();
		// Every relayout is for a screen of another size in canvas units (a window being resized), so every element is laid
		// out; the labels do not fit their text, so no text is shaped.
		int relayout = 0;
		Perf::CheckBudgetConfirmed("Perf.GameUI.Layout10k", [&]
		{
			const Perf::Measurement measurement = Perf::Measure("Layout10k", 2, c_Relayouts, [&]
			{
				relayout++;
				const glm::vec2 size = relayout % 2 == 0 ? glm::vec2(1920.0f, 1080.0f + static_cast<float>(relayout)) : glm::vec2(1280.0f, 1024.0f + static_cast<float>(relayout));
				ui.EnsureLayout(UIScreen { size, 1.0f });
			});
			const GameUISceneStats stats = ui.TakeStats();
			CHECK(stats.TextsShaped == 0);
			CHECK(stats.ElementsLaidOut == 10'001ull * (2 + c_Relayouts));
			return measurement.MedianMs;
		});
	}

	TEST_CASE("An idle frame of the UI system on 10,000 elements")
	{
		Input::Reset();
		Ref<Scene> scene = CreateRef<Scene>("Widgets");
		const Perf::UIWidgets widgets = Perf::CreateUIWidgets(*scene);
		scene->OnRuntimeStart();
		GameUISystem* system = scene->GetSystem<GameUISystem>();
		REQUIRE(system);
		// The pointer rests on a button (hovered, its fade over): what a player looking at a menu costs per frame.
		GameUIScene& ui = GameUIScene::Get(*scene);
		UICanvasView view;
		uint32_t element = 0;
		REQUIRE(ui.FindElement(widgets.Buttons[40].GetHandle(), GameUIScene::GetInputScreen().Screen, view, element));
		Input::SimulateMouseMove(ui.GetElementBox(view, element).GetCenter() * view.Scale);
		for (int frame = 0; frame < 30; frame++)
		{
			Input::BeginFrame();
			scene->OnUpdateRuntime(1.0f / 60.0f);
		}
		REQUIRE(system->GetHovered() == widgets.Buttons[40]);
		ui.TakeStats();

		// The UI system's update alone (with its EnsureLayout), the other systems of the frame not included.
		Perf::CheckBudgetConfirmed("Perf.GameUI.IdleFrame10k", [&]
		{
			const Perf::Measurement batches = Perf::Measure("IdleFrame10k (10 frames)", 2, c_IdleBatches, [&]
			{
				for (uint32_t frame = 0; frame < c_FramesPerBatch; frame++)
				{
					Input::BeginFrame();
					system->OnUpdate(1.0f / 60.0f);
				}
			});
			const GameUISceneStats stats = ui.TakeStats();
			CHECK(stats.ElementsLaidOut == 0);
			CHECK(stats.TextsShaped == 0);
			CHECK(stats.QuadsBuilt == 0);
			return batches.MedianMs / c_FramesPerBatch;
		});
		scene->OnRuntimeStop();
		Input::Reset();
	}
}

TEST_SUITE("PerfGPU.GameUI")
{
	TEST_CASE("The UI pass on 10,000 elements, changing and idle")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		Scene scene("Widgets");
		const Perf::UIWidgets widgets = Perf::CreateUIWidgets(scene);
		SceneRenderer renderer;
		renderer.SetViewportSize(c_Width, c_Height);

		// 100 labels change their text and 100 buttons their color in every frame (other ones in each frame).
		int changed = 0;
		const auto change = [&](int frame)
		{
			for (int index = 0; index < 100; index++)
			{
				const size_t which = (static_cast<size_t>(frame) * 100 + static_cast<size_t>(index)) * 7 % widgets.Labels.size();
				Entity label = widgets.Labels[which];
				label.GetComponent<UITextComponent>().Text = "Value " + std::to_string(frame * 100 + index);
				label.MarkModified<UITextComponent>();
				Entity button = widgets.Buttons[(which + 13) % widgets.Buttons.size()];
				button.GetComponent<UIImageComponent>().Color = { static_cast<float>(frame % 10) / 10.0f, 0.5f, 0.9f, 1.0f };
				button.MarkModified<UIImageComponent>();
			}
			changed++;
		};
		Perf::CheckBudgetConfirmed("PerfGPU.GameUI.Render10k", [&]
		{
			const Perf::Measurement measurement = MeasureUIPass(gpu, renderer, scene, change, "Render10k");
			CHECK(renderer.GetStats().Overlays.Elements == 10'001);
			CHECK(renderer.GetStats().Overlays.DrawCalls == 1);
			return measurement.MedianMs;
		});

		// Nothing changes: what an idle game view pays.
		Perf::CheckBudgetConfirmed("PerfGPU.GameUI.RenderIdle10k", [&]
		{
			const Perf::Measurement measurement = MeasureUIPass(gpu, renderer, scene, [](int) {}, "RenderIdle10k");
			CHECK(renderer.GetStats().Overlays.UploadedBytes == 0);
			return measurement.MedianMs;
		});
		CHECK(changed > 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
