#include <doctest/doctest.h>

#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Core/FrameProfiler.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Renderer/GPUProfiler.h"
#include "Strata/Renderer/Renderer.h"

#include <chrono>
#include <numeric>
#include <vector>

using namespace Strata;

namespace
{

	// A shadowed scene of a few boxes in front of the camera: every pass of the scene renderer has work.
	struct TimedScene
	{
		Tests::SceneTestAssets Assets;
		Ref<Scene> TestScene = CreateRef<Scene>();
		SceneCamera Camera = Tests::LookAt({ 0.0f, 3.0f, 8.0f }, { 0.0f, 0.0f, 0.0f });

		TimedScene()
		{
			const AssetHandle material = Assets.AddMaterial(Tests::UnlitColor({ 0.8f, 0.4f, 0.2f, 1.0f }));
			for (int x = -2; x <= 2; x++)
			{
				for (int z = -2; z <= 2; z++)
					Tests::AddMesh(*TestScene, BuiltinAssets::CubeMesh, material, { static_cast<float>(x) * 1.5f, 0.0f, static_cast<float>(z) * 1.5f }, "Box");
			}
			Tests::AddSun(*TestScene, glm::normalize(glm::vec3(-0.3f, -1.0f, -0.4f)), 3.0f);
		}
	};

	// One application-like frame: the device's frame pacing, the renderer's frame (which resolves GPU times) and a render.
	void RunFrame(GraphicsDevice& device, FrameProfiler& profiler, SceneRenderer& renderer, TimedScene& timed)
	{
		profiler.BeginFrame();
		REQUIRE(device.BeginFrame());
		Renderer::BeginFrame();
		const auto start = std::chrono::steady_clock::now();
		CHECK(renderer.Render(*timed.TestScene, timed.Camera));
		const double work = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		device.EndFrame();
		profiler.EndFrame(work);
	}

	nvrhi::TextureHandle CreateTarget(nvrhi::IDevice* device, uint32_t size)
	{
		nvrhi::TextureDesc desc;
		desc.width = size;
		desc.height = size;
		desc.format = nvrhi::Format::RGBA8_UNORM;
		desc.isRenderTarget = true;
		desc.debugName = "TimerTarget";
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		desc.setClearValue(nvrhi::Color(0.0f));
		return device->createTexture(desc);
	}

}

TEST_SUITE("GPU.Renderer.Timers")
{

	TEST_CASE("Timestamps written outside and inside render passes read back in order without waiting")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!gpu.GetDevice().GetInfo().SupportsTimestamps)
		{
			MESSAGE("The device writes no timestamps: nothing to check");
			return;
		}
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();
		Scope<GPUTimestampPool> pool = gpu.GetDevice().CreateTimestampPool(5);
		REQUIRE(pool);
		CHECK(pool->GetCount() == 5);

		constexpr uint32_t size = 512;
		nvrhi::TextureHandle target = CreateTarget(device, size);
		REQUIRE(target);
		nvrhi::FramebufferHandle framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(target));
		nvrhi::GraphicsPipelineDesc pipelineDesc;
		pipelineDesc.VS = Renderer::GetShaderLibrary().Get("Debug/ColorTriangle.vert");
		pipelineDesc.PS = Renderer::GetShaderLibrary().Get("Debug/ColorTriangle.frag");
		pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
		pipelineDesc.renderState.rasterState.setCullNone();
		pipelineDesc.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
		pipelineDesc.renderState.blendState.targets[0].setBlendEnable(true).setSrcBlend(nvrhi::BlendFactor::One).setDestBlend(nvrhi::BlendFactor::One);
		nvrhi::GraphicsPipelineHandle pipeline = device->createGraphicsPipeline(pipelineDesc, framebuffer->getFramebufferInfo());
		REQUIRE(pipeline);
		nvrhi::GraphicsState state;
		state.pipeline = pipeline;
		state.framebuffer = framebuffer;
		state.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(size), static_cast<float>(size)));

		// 0 and 4 lie outside render passes (around clears, which end them), 1 to 3 inside one: between draws of a render pass
		// that NVRHI keeps open, as the scene renderer's pass boundaries are.
		nvrhi::CommandListHandle commandList = device->createCommandList();
		commandList->open();
		commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		pool->Write(commandList, 0);
		commandList->setGraphicsState(state);
		for (int draw = 0; draw < 200; draw++)
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		pool->Write(commandList, 1);
		for (int draw = 0; draw < 200; draw++)
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		pool->Write(commandList, 2);
		for (int draw = 0; draw < 200; draw++)
			commandList->draw(nvrhi::DrawArguments().setVertexCount(3));
		pool->Write(commandList, 3);
		commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		pool->Write(commandList, 4);
		commandList->close();

		std::vector<uint64_t> ticks(5, 0);
		CHECK_FALSE(pool->Read(0, 5, ticks)); // Not executed yet: unavailable, and the read does not wait
		REQUIRE(gpu.ExecuteAndWait(commandList));
		REQUIRE(pool->Read(0, 5, ticks));
		const double period = gpu.GetDevice().GetInfo().TimestampPeriodNs;
		std::vector<double> intervals;
		for (uint32_t index = 1; index < 5; index++)
		{
			CHECK(ticks[index] >= ticks[index - 1]);
			intervals.push_back(static_cast<double>((ticks[index] - ticks[index - 1]) & pool->GetValueMask()) * period * 1.0e-6);
		}
		// Drivers of tile-based GPUs may write a timestamp recorded inside a render pass when the pass ends (MoltenVK samples
		// Metal's counters at encoder boundaries), so the draws' time can fall into any of the first three intervals: only
		// their order and their sum are the same everywhere.
		MESSAGE("Timestamp intervals (ms): draws " << intervals[0] << ", " << intervals[1] << ", " << intervals[2] << "; clear " << intervals[3]);
		CHECK(intervals[0] + intervals[1] + intervals[2] > 0.0);

		// A reset makes them unavailable again.
		pool->Reset(0, 5);
		CHECK_FALSE(pool->Read(0, 5, ticks));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("GPU stages resolve within the frames in flight, add up within the frames and never make a frame wait")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		GPUProfiler& gpuProfiler = Renderer::GetGPUProfiler();
		if (!gpuProfiler.IsEnabled())
		{
			MESSAGE("The device writes no timestamps: nothing to check");
			return;
		}
		FrameProfiler& profiler = FrameProfiler::Get();
		profiler.Reset();
		const RendererGPUStages& stages = RendererGPUStages::Get();

		TimedScene timed;
		SceneRenderer renderer;
		renderer.SetViewportSize(Tests::c_SceneTestSize, Tests::c_SceneTestSize);
		// Assets load and upload in the first frames.
		for (int frame = 0; frame < 10; frame++)
			RunFrame(gpu.GetDevice(), profiler, renderer, timed);
		REQUIRE(Tests::GetDrawnCounts(renderer).Instances == 25);

		std::vector<uint64_t> heardFrames;
		std::vector<bool> heardWithGPU;
		const uint32_t listener = profiler.AddFrameListener([&](const FrameSample& sample)
		{
			heardFrames.push_back(sample.Index);
			heardWithGPU.push_back(sample.HasGPU);
		}, FrameListenerTiming::WithGPU);

		const uint64_t firstMeasured = profiler.GetTotalFrameCount();
		const uint32_t latencyLimit = gpu.GetDevice().GetMaxFramesInFlight() + 2;
		const auto start = std::chrono::steady_clock::now();
		constexpr uint32_t c_Frames = 30;
		for (uint32_t frame = 0; frame < c_Frames; frame++)
			RunFrame(gpu.GetDevice(), profiler, renderer, timed);
		gpu.GetDevice().WaitForIdle();
		const double wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

		// Every measured frame but the last few had its GPU times by the time MaxFramesInFlight + 2 frames followed it.
		double busySum = 0.0;
		uint32_t framesWithGPU = 0;
		for (uint32_t age = latencyLimit; age < c_Frames; age++)
		{
			const FrameSample sample = profiler.GetFrame(age);
			REQUIRE(sample.Index >= firstMeasured);
			INFO("frame " << sample.Index);
			CHECK(sample.HasGPU);
			CHECK(sample.GPULatencyFrames <= latencyLimit);
			if (!sample.HasGPU)
				continue;
			framesWithGPU++;
			busySum += sample.GPUBusyMs;
			// A scene that draws takes GPU time in its passes.
			CHECK(sample.GPUStageMs[stages.Shadows] > 0.0f);
			CHECK(sample.GPUStageMs[stages.Prepass] > 0.0f);
			CHECK(sample.GPUStageMs[stages.Opaque] > 0.0f);
			CHECK(sample.GPUStageMs[stages.Post] > 0.0f);
			// GPU stages are not CPU time.
			CHECK(sample.StageMs[stages.Opaque] == 0.0f);
			const float stageSum = std::accumulate(sample.GPUStageMs.begin(), sample.GPUStageMs.end(), 0.0f);
			CHECK(stageSum == doctest::Approx(sample.GPUBusyMs).epsilon(1e-4));
		}
		CHECK(framesWithGPU == c_Frames - latencyLimit);
		// The GPU runs the frames one after the other: their busy times fit in the time they took.
		CHECK(busySum <= wallMs);

		// The WithGPU listener heard of the frames in order, with their GPU times.
		REQUIRE(heardFrames.size() >= c_Frames - latencyLimit);
		for (size_t index = 0; index < heardFrames.size(); index++)
		{
			CHECK(heardFrames[index] == firstMeasured + index);
			CHECK(heardWithGPU[index]);
		}
		profiler.RemoveFrameListener(listener);

		// The summary's GPU part.
		const FrameProfileSummary summary = profiler.Summarize(c_Frames);
		CHECK(summary.GPU.Frames >= c_Frames - latencyLimit);
		CHECK(summary.GPU.Busy.P50 > 0.0f);
		CHECK(summary.GPU.LatencyFrames >= 1.0f);
		CHECK(summary.GPU.LatencyFrames <= static_cast<float>(latencyLimit));
		CHECK(summary.GPU.BoundBy != FrameBound::Unknown);

		// The renderer's own times: its stages, of a frame it rendered.
		renderer.ResolveGPUStats(true);
		const SceneRendererGPUStats& own = renderer.GetGPUStats();
		CHECK(own.HasTimes);
		CHECK(own.BusyMs > 0.0f);
		CHECK(own.Frame >= firstMeasured);
		CHECK(gpuProfiler.GetStats().Timestamps > 0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A frame whose timestamps are never executed is dropped without waiting")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		if (!gpu.GetDevice().GetInfo().SupportsTimestamps)
		{
			MESSAGE("The device writes no timestamps: nothing to check");
			return;
		}
		// A profiler of its own, driven frame by frame, so the frame's slot is known.
		FrameProfilerSpecification specification;
		specification.SampleJobs = false;
		FrameProfiler frames(specification);
		const FrameStageID stage = frames.RegisterStage("GPU.Test", false, FrameStageKind::GPU);
		uint64_t heard = 0;
		frames.AddFrameListener([&heard](const FrameSample&) { heard++; }, FrameListenerTiming::WithGPU);
		GPUProfiler gpuProfiler(gpu.GetDevice(), frames);
		REQUIRE(gpuProfiler.IsEnabled());
		CHECK(frames.AreGPUTimesExpected());
		nvrhi::IDevice* device = gpu.GetNvrhiDevice();

		// Frame 0 records a timed command list that is not executed.
		frames.BeginFrame();
		gpuProfiler.BeginFrame();
		nvrhi::TextureHandle target = CreateTarget(device, 16);
		REQUIRE(target);
		nvrhi::CommandListHandle commandList = device->createCommandList();
		commandList->open();
		{
			GPUPassTimer timer(&gpuProfiler, commandList.Get(), stage);
			commandList->clearTextureFloat(target, nvrhi::AllSubresources, nvrhi::Color(1.0f));
		}
		commandList->close();
		frames.EndFrame(0.001);
		CHECK(heard == 0); // Its GPU times are awaited

		// Once the GPU finished the frame's work, its times are given one more frame (drivers may make timestamps readable a
		// little after the work completed); still missing then, they can never arrive: the frame is resolved without them,
		// without waiting for anything, and the listener hears of it.
		gpu.GetDevice().WaitForIdle();
		frames.BeginFrame();
		auto start = std::chrono::steady_clock::now();
		gpuProfiler.BeginFrame();
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
		frames.EndFrame(0.001);
		CHECK(gpuProfiler.GetStats().DroppedFrames == 0);
		CHECK(heard == 0);
		frames.BeginFrame();
		start = std::chrono::steady_clock::now();
		gpuProfiler.BeginFrame();
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
		frames.EndFrame(0.001);
		CHECK(gpuProfiler.GetStats().DroppedFrames == 1);
		CHECK(gpuProfiler.GetStats().ResolvedFrames == 0);
		const FrameSample first = frames.GetFrame(2);
		CHECK(first.Index == 0);
		CHECK_FALSE(first.HasGPU);
		CHECK(heard >= 1);

		// NVRHI frees a command list's buffer only once it was executed: it runs now, into timestamps that were reset and
		// never written, before the profiler goes.
		REQUIRE(gpu.ExecuteAndWait(commandList));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The back buffer's first preparation in a frame is timed as the Present stage, outside the work time")
	{
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		FrameProfiler& profiler = FrameProfiler::Get();
		profiler.Reset();
		const FrameStageID present = profiler.FindStage("Present");
		REQUIRE(present != c_InvalidFrameStage);
		CHECK_FALSE(profiler.IsStageInWorkTime(present));
		const FrameStageID ui = profiler.RegisterStage("Test.UI");
		REQUIRE(ui != c_InvalidFrameStage);
		float presentMs = 0.0f;
		const uint32_t listener = profiler.AddFrameListener([&](const FrameSample& frame) { presentMs += frame.StageMs[present]; });

		// The editor's first use of the back buffer is inside its UI stage (where MoltenVK waits for the display's image).
		// Headless, the device has no image and returns at once: a short time, so many frames, as one frame's can be below
		// the clock's resolution.
		double outOfWorkSeconds = 0.0;
		for (int frame = 0; frame < 200; frame++)
		{
			profiler.BeginFrame();
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			{
				ST_PROFILE_STAGE("Test.UI");
				CHECK(Renderer::PrepareBackBuffer() == nullptr);
				CHECK(Renderer::PrepareBackBuffer() == nullptr); // Prepared already: no second wait
				outOfWorkSeconds += profiler.GetOutOfWorkSeconds();
			}
			gpu.GetDevice().EndFrame();
			profiler.EndFrame(0.001);
		}
		profiler.RemoveFrameListener(listener);
		CHECK(presentMs > 0.0f);
		CHECK(outOfWorkSeconds > 0.0);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
