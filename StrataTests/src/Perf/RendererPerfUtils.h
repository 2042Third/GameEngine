#pragma once

#include "Perf/PerfUtils.h"
#include "Renderer/GPUTestUtils.h"

#include <Strata/Core/FrameProfiler.h>
#include <Strata/Renderer/GPUProfiler.h>
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
		uint32_t GPUFrames = 0;     // Measured frames with GPU times
		double TimerCostMs = 0.0;   // GPUProfilerStats::CPUMilliseconds per measured frame
	};

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
		double timerCostStart = 0.0;
		uint64_t firstMeasured = 0;
		for (int frame = 0; frame < warmup + measured; frame++)
		{
			if (frame == warmup)
			{
				timerCostStart = gpuProfiler.GetStats().CPUMilliseconds;
				firstMeasured = profiler.GetTotalFrameCount();
			}
			profiler.BeginFrame();
			REQUIRE(gpu.GetDevice().BeginFrame());
			Renderer::BeginFrame();
			const auto start = std::chrono::steady_clock::now();
			render();
			const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			gpu.GetDevice().EndFrame();
			profiler.EndFrame(milliseconds / 1000.0);
			if (frame >= warmup)
				samples.push_back(milliseconds);
		}
		FrameMeasurement result;
		result.CPU = Summarize(samples);
		result.TimerCostMs = (gpuProfiler.GetStats().CPUMilliseconds - timerCostStart) / static_cast<double>(std::max(measured, 1));

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

	// The median GPU busy time of the frames MeasureFrames measured; 0 (with a message) on a device without timestamps.
	inline double MeasureGPU(const FrameMeasurement& measurement)
	{
		if (!measurement.GPU)
		{
			MESSAGE("No GPU times: the device writes no timestamps");
			return 0.0;
		}
		return measurement.GPU->MedianMs;
	}

}
