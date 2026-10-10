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
	// self time, the rest of the frame on top as untimed work), with a tooltip of the frame under the mouse, markers on spikes
	// (frames over twice the median and a millisecond), the statistics per stage and of the job threads, and Copy as JSON
	// (what editor.perf returns for the 600 frames). Reads FrameProfiler::Get(), whose frames the application records.
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
		// The color of a stage in the timeline (a categorical palette of the theme's tokens); the untimed rest has its own.
		static const ImVec4& GetStageColor(FrameStageID stage);
		static const ImVec4& GetUntimedColor();
		// editor.perf's JSON of the latest frames (as Copy as JSON puts it on the clipboard).
		static std::string DescribeProfile(uint32_t frames);
	private:
		void DrawSummary();
		void DrawTimeline();
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
