#pragma once

#include "UI/EditorPanelRegistry.h"

#include <Strata/Core/FrameProfiler.h>
#include <Strata/Core/JobSystem.h>

#include <imgui.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace Strata
{

	// Where the frames go: a stacked timeline of the frame profiler's last 600 frames (each column a frame, stacked by stage
	// self time, the rest of the frame on top as untimed work, scaled to the usual frames) with the GPU's timeline under it
	// (the same frames at the same scale, stacked by GPU stage; frames without GPU times blank), a tooltip of the frame under
	// the mouse, markers on spikes (frames over twice the median and a 60 FPS frame), the statistics per stage under the
	// frame's totals and of the job threads, a GPU section (what bounds the frames, the GPU's busy time, its stages), and
	// Copy as JSON (what editor.perf returns for the 600 frames). Reads FrameProfiler::Get(), whose frames the application
	// records.
	class PerformancePanel : public EditorPanel
	{
	public:
		static constexpr double c_SummaryRefreshSeconds = 0.25;
		// Frames the status bar's job count is averaged over.
		static constexpr uint32_t c_JobSamples = 60;

		void OnUpdate(EditorPanelContext& context) override;
		void OnImGuiRender(EditorPanelContext& context) override;

		// Jobs running or queued, averaged over the last c_JobSamples frames (the status bar's frame pill).
		float GetAverageJobsInFlight() const;
		// The job system as sampled in this frame's OnUpdate (threads, running and queued jobs).
		const JobSystemStats& GetJobStats() const { return m_Jobs; }
		// The statistics the panel shows (computed when it was last drawn, at most c_SummaryRefreshSeconds ago).
		const std::optional<FrameProfileSummary>& GetSummary() const { return m_Summary; }
		// The color of a stage in the timeline (a categorical palette of the theme's tokens); the untimed rest has its own.
		static const ImVec4& GetStageColor(FrameStageID stage);
		static const ImVec4& GetUntimedColor();
		// editor.perf's JSON of the latest frames (as Copy as JSON puts it on the clipboard).
		static std::string DescribeProfile(uint32_t frames);
		// The timeline's height in milliseconds: a 1-2-5 step (at least 1 ms) with room above the 95th percentile frame, so
		// one slow frame does not flatten the others; taller frames are cut at the top.
		static float GetTimelineScale(const FrameProfileSummary& summary);
		// Frames taking longer are spikes: twice the median frame, and at least a frame at 60 frames per second.
		static float GetSpikeThreshold(const FrameProfileSummary& summary);
		// What bounds the frames, in a line: "GPU-bound: GPU 11.8 ms busy of 12.3 ms frames (CPU 9.8 ms)", CPU-bound, capped or
		// unknown (the panel's GPU section and the status bar's frame pill).
		static std::string DescribeBound(const FrameProfileSummary& summary);
		// Whether the panel shows the GPU: GPU times arrived, or are expected.
		static bool ShowsGPU(const FrameProfileSummary& summary);
	private:
		void DrawSummary();
		void DrawTimeline(const ImVec2& size);
		// One strip of the timeline: the CPU's stages and untimed work, or the GPU's stages.
		void DrawTimelineStrip(const ImVec2& size, bool gpu);
		void DrawStageTable();
	private:
		JobSystemStats m_Jobs;
		std::array<float, c_JobSamples> m_JobSamples = {};
		uint32_t m_JobCursor = 0;
		uint32_t m_JobSampleCount = 0;
		std::optional<FrameProfileSummary> m_Summary;
		double m_LastSummaryTime = -1.0;
		bool m_Copied = false;
		double m_CopiedTime = 0.0;
	};

}
