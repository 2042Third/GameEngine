#pragma once

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Renderer/GPUTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"

#include <Strata/Core/FrameProfiler.h>
#include <Strata/Renderer/GPUProfiler.h>
#include <Strata/Renderer/MeshFactory.h>
#include <Strata/Renderer/Renderer.h>

#include <doctest/doctest.h>

#include <chrono>
#include <functional>
#include <optional>
#include <vector>

// Measuring renderers' frames on the CPU and the GPU (PerfGPU suites). PerfUtils.h stays free of renderer includes.
namespace Strata::Tests::Perf
{

	// What MeasureFrames measured: the CPU time of the timed part of each measured frame, the GPU's busy time of the frames
	// whose GPU times arrived (none without timestamps), and the GPU profiler's own CPU time per frame (GPUTimerCost).
	struct FrameMeasurement
	{
		Measurement CPU;
		std::optional<Measurement> GPU;
		uint32_t GPUFrames = 0; // Measured frames with GPU times
		// GPUProfilerStats::CPUMilliseconds of each measured frame (its writes, and the resolve in Renderer::BeginFrame).
		Measurement TimerCost;
	};

	// The renderers whose frames are measured redraw every shadow cascade every frame (SceneRendererSpecification::
	// CacheShadowCascades off): a still camera over a still scene keeps them, which would leave the shadows' cost out.
	inline SceneRendererSpecification MeasuredRendererSpecification()
	{
		SceneRendererSpecification specification;
		specification.CacheShadowCascades = false;
		return specification;
	}

	// Runs `warmup` then `measured` frames as an application does - the frame profiler's frame (FrameProfiler::Get(), reset
	// first) around the device's frame (frame pacing) and the renderer's (Renderer::BeginFrame, which hands the GPU times of
	// completed frames to the profiler) - timing `render` in each. After the last frame it waits for the GPU and runs one
	// more renderer frame, so every measured frame's GPU times are collected.
	inline FrameMeasurement MeasureFrames(GPUContext& gpu, int warmup, int measured, const std::function<void()>& render)
	{
		FrameProfiler& profiler = FrameProfiler::Get();
		profiler.Reset();
		GPUProfiler& gpuProfiler = Renderer::GetGPUProfiler();
		std::vector<double> samples;
		std::vector<double> timerCosts;
		uint64_t firstMeasured = 0;
		for (int frame = 0; frame < warmup + measured; frame++)
		{
			if (frame == warmup)
				firstMeasured = profiler.GetTotalFrameCount();
			const double timerCostStart = gpuProfiler.GetStats().CPUMilliseconds;
			profiler.BeginFrame();
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			const auto start = std::chrono::steady_clock::now();
			render();
			const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			gpu.GetDevice().EndFrame();
			profiler.EndFrame(milliseconds / 1000.0);
			if (frame >= warmup)
			{
				samples.push_back(milliseconds);
				timerCosts.push_back(gpuProfiler.GetStats().CPUMilliseconds - timerCostStart);
			}
		}
		FrameMeasurement result;
		result.CPU = Summarize(samples);
		result.TimerCost = Summarize(timerCosts);

		// The last frames' GPU times: their slots close with the next renderer frame, and resolve once the GPU is idle.
		gpu.GetDevice().WaitForIdle();
		profiler.BeginFrame();
		Renderer::BeginFrame();
		gpuProfiler.Resolve(true);
		profiler.EndFrame(0.0);
		std::vector<double> busy;
		for (uint32_t age = 0; age < profiler.GetRecordedFrameCount(); age++)
		{
			const FrameSample frame = profiler.GetFrame(age);
			if (frame.Index >= firstMeasured && frame.Index < firstMeasured + static_cast<uint64_t>(measured) && frame.HasGPU)
				busy.push_back(frame.GPUBusyMs);
		}
		result.GPUFrames = static_cast<uint32_t>(busy.size());
		if (!busy.empty())
			result.GPU = Summarize(busy);
		return result;
	}

	// `count` distinct meshes on the cube grid (CreateCubeGrid): boxes of slightly different sizes, each a memory asset of
	// `assets` (the scale audit's Unique10k scene: every proxy its own mesh, so draws cannot share a mesh).
	inline std::vector<Entity> CreateUniqueMeshGrid(Scene& scene, SceneTestAssets& assets, size_t count)
	{
		std::vector<Entity> entities = CreateCubeGrid(scene, count);
		for (size_t index = 0; index < entities.size(); index++)
		{
			const float size = 0.8f + 0.4f * static_cast<float>(index) / static_cast<float>(count);
			const AssetHandle mesh = assets.Add(MeshFactory::CreateCube(size), "UniqueBox");
			entities[index].GetComponent<MeshRendererComponent>().Mesh = mesh;
			entities[index].MarkModified<MeshRendererComponent>();
		}
		return entities;
	}

	// Whether GPU metrics are measured: on devices that write timestamps. Without them, tests leave the metrics out (a 0 would
	// pass any budget).
	inline bool MeasuresGPU()
	{
		const bool timestamps = Renderer::GetGraphicsDevice().GetInfo().SupportsTimestamps;
		if (!timestamps)
			MESSAGE("No GPU times: the device writes no timestamps");
		return timestamps;
	}

	// The median GPU busy time of the frames MeasureFrames measured, on a device that writes timestamps (MeasuresGPU): their
	// GPU times must have arrived, so a regression that stops them fails instead of recording 0 ms.
	inline double MeasureGPU(const FrameMeasurement& measurement)
	{
		REQUIRE(Renderer::GetGPUProfiler().IsEnabled());
		REQUIRE(measurement.GPU.has_value());
		// Every measured frame's times arrive (MeasureFrames waits for the GPU at the end); allow a few dropped ones.
		CHECK(measurement.GPUFrames * 10 >= measurement.CPU.Iterations * 9);
		return measurement.GPU->MedianMs;
	}

}
