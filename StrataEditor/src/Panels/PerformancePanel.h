#pragma once

#include "UI/EditorPanelRegistry.h"

#include <Strata/Core/FrameProfiler.h>
#include <Strata/Core/JobSystem.h>

#include <imgui.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	// Where the frames go: a stacked timeline of the frame profiler's last 600 frames (each column a frame, stacked by stage
	// self time, the rest of the frame on top as untimed work, scaled to the usual frames) with the GPU's timeline under it
	// (the same frames at the same scale, stacked by GPU stage; frames without GPU times blank), a tooltip of the frame under
	// the mouse, markers on spikes (frames over twice the median and a 60 FPS frame), the statistics per stage under the
	// frame's totals and of the job threads, a GPU section (what bounds the frames, the GPU's busy time, its stages), and
	// Copy as JSON (what editor.perf returns for the 600 frames). Reads FrameProfiler::Get(), whose frames the application
	// records. Pause (or Space with the panel focused) holds the timeline, its tooltip, the summary, the table and Copy as
	// JSON still on a copy of the profiler's ring (FrameProfileSnapshot), taken once the GPU times of the frames on screen at
	// the click arrived; the profiler, editor.perf and the status bar keep measuring, and hiding the panel resumes it.
	class PerformancePanel : public EditorPanel
	{
	public:
		static constexpr double c_SummaryRefreshSeconds = 0.25;
		// Frames the status bar's job count is averaged over.
		static constexpr uint32_t c_JobSamples = 60;

		void OnUpdate(EditorPanelContext& context) override;
		void OnImGuiRender(EditorPanelContext& context) override;
		// Hidden (closed, collapsed or behind another tab): a pause is for reading what is in front of the person, so it resumes
		// and frees its copy.
		void OnHidden(EditorPanelContext& context) override;

		// Paused: waiting for the GPU times of the frames at the click, or showing their snapshot.
		bool IsPaused() const { return m_PauseFrame.has_value() || m_Snapshot.has_value(); }
		// The frames the paused panel shows (null while live or while the snapshot waits for GPU times).
		const FrameProfileSnapshot* GetSnapshot() const { return m_Snapshot ? &*m_Snapshot : nullptr; }
		// What the Pause button and Space do: holds the frames on screen now (FrameProfiler::Get()'s newest), its snapshot taken once
		// their GPU times arrived; nothing without recorded frames. Resume goes back to the latest frames.
		void Pause();
		void Resume();

		// Jobs running or queued, averaged over the last c_JobSamples frames (the status bar's frame pill).
		float GetAverageJobsInFlight() const;
		// The job system as sampled in this frame's OnUpdate (threads, running and queued jobs).
		const JobSystemStats& GetJobStats() const { return m_Jobs; }
		// The statistics the panel shows (computed when it was last drawn, at most c_SummaryRefreshSeconds ago).
		const std::optional<FrameProfileSummary>& GetSummary() const { return m_Summary; }
		// How many times the stage table's rows were made: with the statistics, not in every frame drawn (tests).
		uint64_t GetTableBuildCount() const { return m_TableBuilds; }
		// The color of a stage in the timeline (a categorical palette of the theme's tokens); the untimed rest has its own.
		static const ImVec4& GetStageColor(FrameStageID stage);
		static const ImVec4& GetUntimedColor();
		// editor.perf's JSON of the latest frames (as Copy as JSON puts it on the clipboard).
		static std::string DescribeProfile(uint32_t frames);
		// editor.perf's JSON of a snapshot's frames with the jobs as they were, and "paused": {firstFrame, lastFrame} (Copy as
		// JSON while paused).
		static std::string DescribeSnapshot(const FrameProfileSnapshot& snapshot, const JobSystemStats& jobs);
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
		// A row of the stage table. Its text is made with the statistics (a few times per second, UpdateTableRows), not in each
		// frame that draws it: the table of every registered stage would format about a hundred numbers per frame.
		struct TableRow
		{
			// The swatch's color: the text's (the totals), the untimed rest's or the row's stage's; None for a line of text without
			// a swatch or values (the GPU section's headings).
			enum class SwatchColor : uint8_t
			{
				Text = 0,
				Untimed,
				Stage,
				None
			};
			SwatchColor Swatch = SwatchColor::Text;
			FrameStageID Stage = c_InvalidFrameStage;
			std::string Name;
			std::array<std::string, 4> Values; // The average, p50, p95 and maximum in milliseconds
			bool Secondary = false;            // In the secondary text color: the untimed rest, stages that did not run
			const char* Probe = nullptr;       // The item probe recorded with the row's last item (and its name as the text)
		};

		// Makes the stage table's rows of m_Summary.
		void UpdateTableRows();
		void DrawSummary();
		void DrawTimeline(const ImVec2& size);
		// One strip of the timeline: the CPU's stages and untimed work, or the GPU's stages. Frames: FrameProfiler while live,
		// FrameProfileSnapshot while paused (the same accessors); instantiated in the .cpp only.
		template<typename Frames>
		void DrawTimelineStrip(const Frames& frames, const ImVec2& size, bool gpu);
		void DrawStageTable();
		// Whether the panel shows the GPU now: ShowsGPU, with the snapshot's expectation while paused.
		bool ShowsGPUNow() const;
		// Takes the snapshot once the GPU times of the clicked frame arrived (or are not expected); resumes when the profiler
		// was reset since the click, or the clicked frame left the ring.
		void UpdatePause(const FrameProfiler& profiler);
	private:
		JobSystemStats m_Jobs;
		std::array<float, c_JobSamples> m_JobSamples = {};
		uint32_t m_JobCursor = 0;
		uint32_t m_JobSampleCount = 0;
		std::optional<FrameProfileSummary> m_Summary;
		// The stage table of m_Summary: the frame's totals and the CPU's stages, and the GPU section (shown with the GPU).
		std::vector<TableRow> m_CPURows;
		std::vector<TableRow> m_GPURows;
		uint64_t m_TableBuilds = 0;
		double m_LastSummaryTime = -1.0;
		bool m_Copied = false;
		double m_CopiedTime = 0.0;
		// The pause: the newest frame at the click (while its snapshot waits for GPU times) and the profiler's reset count then,
		// the frames shown and the jobs as they were when taken, and the summary line's note.
		std::optional<uint64_t> m_PauseFrame;
		uint32_t m_PauseResets = 0;
		std::optional<FrameProfileSnapshot> m_Snapshot;
		JobSystemStats m_SnapshotJobs;
		std::string m_PausedLabel;
	};

}
