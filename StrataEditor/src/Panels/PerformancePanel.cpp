#include "Panels/PerformancePanel.h"

#include "Editor/CommandUtils.h"
#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace Strata
{

	namespace
	{

		// Above the stage table, the timeline takes this share of the height below the summary, at most this many text
		// heights; the table gets the rest.
		constexpr float c_TimelineShare = 0.4f;
		constexpr float c_MaxTimelineHeightInFontSizes = 9.0f;
		// A panel too short for the full timeline over this many table rows (header, the frame's totals and a stage), but this
		// many text heights wide, puts the timeline beside the table instead, both at the full height (the bottom dock at
		// large UI scales); the timeline then takes this share of the width.
		constexpr float c_StackedTableRows = 4.0f;
		constexpr float c_SideBySideWidthInFontSizes = 40.0f;
		constexpr float c_SideBySideTimelineShare = 0.45f;
		// The timeline's scale leaves this much room above the 95th percentile frame.
		constexpr float c_ScaleHeadroom = 1.25f;
		// A frame is a spike when it takes this many times the median frame, and at least a frame at 60 frames per second
		// (shorter frames do not hitch, however they compare to the median).
		constexpr float c_SpikeFactor = 2.0f;
		constexpr float c_MinSpikeMs = 1000.0f / 60.0f;
		// Reference lines: the frame times of 60 and 30 frames per second.
		constexpr float c_ReferenceFrameMs[] = { 1000.0f / 60.0f, 1000.0f / 30.0f };
		// The side of a color swatch, in text heights.
		constexpr float c_SwatchInFontSizes = 0.7f;

		// The smallest of 1, 2 and 5 times a power of ten at or above `value` (at least 1 ms): the timeline's scale.
		float GetNiceScale(float value)
		{
			float scale = 1.0f;
			while (scale < value)
			{
				for (const float step : { 2.0f, 2.5f, 2.0f })
				{
					scale *= step;
					if (scale >= value)
						break;
				}
			}
			return scale;
		}

		// A square of the color, centered on the current text line, as an item (drawn, not a glyph: every font has it).
		void Swatch(const ImVec4& color)
		{
			const float side = ImGui::GetFontSize() * c_SwatchInFontSizes;
			const ImVec2 position = ImGui::GetCursorScreenPos();
			ImGui::Dummy(ImVec2(side, ImGui::GetTextLineHeight()));
			const ImVec2 min(position.x, position.y + (ImGui::GetTextLineHeight() - side) * 0.5f);
			ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + side, min.y + side), UI::ToColorU32(color));
		}

	}

	float PerformancePanel::GetTimelineScale(const FrameProfileSummary& summary)
	{
		// One scale for the CPU's and the GPU's strips, so their heights compare.
		return GetNiceScale(std::max(summary.Work.P95, summary.GPU.Busy.P95) * c_ScaleHeadroom);
	}

	float PerformancePanel::GetSpikeThreshold(const FrameProfileSummary& summary)
	{
		return std::max(c_MinSpikeMs, summary.Work.P50 * c_SpikeFactor);
	}

	const ImVec4& PerformancePanel::GetStageColor(FrameStageID stage)
	{
		// The built-in stages in the order of c_BuiltinFrameStages: neighbors in the stack differ, the scene's systems get the
		// calm hues, and the stages that are usually large (rendering, the UI) stand apart from each other. Scene.Load shares
		// Present's color, the one stage the stack never shows (it comes after the frame), so no two stages in the stack do.
		const UI::ThemePalette& palette = UI::GetThemePalette();
		const ImVec4* builtin[] = { &palette.Dust, &palette.Umber, &palette.Ochre, &palette.Limestone, &palette.Malachite, &palette.Azurite,
			&palette.Sandstone, &palette.Rust, &palette.Sulfur, &palette.Cinnabar, &palette.Ash, &palette.Limestone };
		static_assert(std::size(builtin) == c_BuiltinFrameStages.size(), "Every built-in stage needs a color");
		if (stage < std::size(builtin))
			return *builtin[stage];
		// Stages modules registered.
		const ImVec4* others[] = { &palette.Azurite, &palette.Malachite, &palette.Sandstone, &palette.Rust, &palette.Sulfur };
		return *others[(stage - std::size(builtin)) % std::size(others)];
	}

	const ImVec4& PerformancePanel::GetUntimedColor()
	{
		return UI::GetThemeColors().Border;
	}

	std::string PerformancePanel::DescribeProfile(uint32_t frames)
	{
		return CommandUtils::DescribeFrameProfile(FrameProfiler::Get().Summarize(frames), JobSystem::GetStats()).dump(2);
	}

	void PerformancePanel::OnUpdate(EditorPanelContext&)
	{
		m_Jobs = JobSystem::GetStats();
		const uint64_t inFlight = m_Jobs.Workers.RunningJobs + m_Jobs.IO.RunningJobs + m_Jobs.Background.RunningJobs + m_Jobs.Workers.QueuedJobs
			+ m_Jobs.IO.QueuedJobs + m_Jobs.Background.QueuedJobs;
		m_JobSamples[m_JobCursor] = static_cast<float>(inFlight);
		m_JobCursor = (m_JobCursor + 1) % c_JobSamples;
		m_JobSampleCount = std::min(m_JobSampleCount + 1, c_JobSamples);
	}

	float PerformancePanel::GetAverageJobsInFlight() const
	{
		if (m_JobSampleCount == 0)
			return 0.0f;
		float sum = 0.0f;
		for (uint32_t sample = 0; sample < m_JobSampleCount; sample++)
			sum += m_JobSamples[sample];
		return sum / static_cast<float>(m_JobSampleCount);
	}

	void PerformancePanel::OnImGuiRender(EditorPanelContext&)
	{
		const FrameProfiler& profiler = FrameProfiler::Get();
		// The statistics move slowly over 600 frames: computed a few times per second, not every frame (but at once while the
		// last ones had no frames, e.g. when the panel opened with the editor).
		const double now = ImGui::GetTime();
		if (!m_Summary || m_Summary->Frames == 0 || now - m_LastSummaryTime >= c_SummaryRefreshSeconds || now < m_LastSummaryTime)
		{
			m_Summary = profiler.Summarize(profiler.GetCapacity());
			m_LastSummaryTime = now;
		}
		if (profiler.GetRecordedFrameCount() == 0)
		{
			ImGui::TextDisabled("No frames were profiled yet");
			return;
		}
		DrawSummary();

		// The stage table always keeps rows in view: a short, wide panel puts the timeline beside it, otherwise the timeline
		// above it takes at most a share of the height. The table fills the rest, so only the table scrolls.
		const ImVec2 available = ImGui::GetContentRegionAvail();
		const float fullTimeline = ImGui::GetFontSize() * c_MaxTimelineHeightInFontSizes;
		const bool sideBySide = available.x >= ImGui::GetFontSize() * c_SideBySideWidthInFontSizes
			&& available.y < fullTimeline + ImGui::GetFrameHeightWithSpacing() * c_StackedTableRows;
		if (sideBySide)
		{
			DrawTimeline(ImVec2(available.x * c_SideBySideTimelineShare, available.y));
			ImGui::SameLine();
		}
		else
		{
			DrawTimeline(ImVec2(available.x, std::clamp(available.y * c_TimelineShare, ImGui::GetFrameHeight(), fullTimeline)));
		}
		DrawStageTable();
	}

	void PerformancePanel::DrawSummary()
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const FrameProfileSummary& summary = *m_Summary;
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		ImGui::AlignTextToFramePadding();
		ImGui::Text("Frame %.2f ms \xC2\xB7 p95 %.2f \xC2\xB7 max %.2f \xC2\xB7 %.0f FPS", summary.Work.Average, summary.Work.P95, summary.Work.Max, summary.FramesPerSecond);
		ImGui::SameLine();
		ImGui::TextColored(colors.TextSecondary, "\xC2\xB7 workers %.0f%% \xC2\xB7 I/O %.0f%% \xC2\xB7 background %.0f%% busy \xC2\xB7 %u jobs running, %llu queued",
			summary.WorkerBusyPercent, summary.IOBusyPercent, summary.BackgroundBusyPercent,
			m_Jobs.Workers.RunningJobs + m_Jobs.IO.RunningJobs + m_Jobs.Background.RunningJobs,
			static_cast<unsigned long long>(m_Jobs.Workers.QueuedJobs + m_Jobs.IO.QueuedJobs + m_Jobs.Background.QueuedJobs));
		ImGui::PopFont();

		// Copy as JSON at the line's end.
		const char* label = "Copy as JSON";
		const ImGuiStyle& style = ImGui::GetStyle();
		const float width = style.FramePadding.x * 2.0f + ImGui::CalcTextSize(Icons::Copy).x + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
		ImGui::SameLine();
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width));
		const bool copiedRecently = m_Copied && ImGui::GetTime() - m_CopiedTime < UI::c_CopiedFeedbackSeconds;
		if (UI::ToolbarButton("Performance.CopyJson", copiedRecently ? Icons::Check : Icons::Copy, label,
			"Copy the statistics of the last 600 frames as JSON (what editor.perf returns)"))
		{
			ImGui::SetClipboardText(DescribeProfile(FrameProfiler::Get().GetCapacity()).c_str());
			m_Copied = true;
			m_CopiedTime = ImGui::GetTime();
		}
	}

	bool PerformancePanel::ShowsGPU(const FrameProfileSummary& summary)
	{
		return summary.GPU.Frames > 0 || FrameProfiler::Get().AreGPUTimesExpected();
	}

	std::string PerformancePanel::DescribeBound(const FrameProfileSummary& summary)
	{
		const FrameGPUSummary& gpu = summary.GPU;
		switch (gpu.BoundBy)
		{
			case FrameBound::GPU:
				return fmt::format("GPU-bound: GPU {:.1f} ms busy of {:.1f} ms frames (CPU {:.1f} ms)", gpu.Busy.P50, summary.Interval.P50, summary.Work.P50);
			case FrameBound::CPU:
				return fmt::format("CPU-bound: CPU {:.1f} ms of {:.1f} ms frames (GPU {:.1f} ms busy)", summary.Work.P50, summary.Interval.P50, gpu.Busy.P50);
			case FrameBound::Capped:
				return fmt::format("Capped: {:.1f} ms frames, CPU {:.1f} ms, GPU {:.1f} ms busy (vsync, a frame-rate cap or idle redraws set the pace)",
					summary.Interval.P50, summary.Work.P50, gpu.Busy.P50);
			case FrameBound::Unknown:
				break;
		}
		return "Bound by: unknown (no GPU times: the GPU writes no timestamps, or no frame reached it yet)";
	}

	void PerformancePanel::DrawTimeline(const ImVec2& requestedSize)
	{
		const ImVec2 size(std::max(requestedSize.x, ImGui::GetFontSize()), std::max(requestedSize.y, ImGui::GetFrameHeight()));
		// The GPU's strip under the CPU's: the same frames in the same columns, at the same scale.
		const bool gpu = ShowsGPU(*m_Summary);
		const float gap = gpu ? ImGui::GetStyle().ItemSpacing.y : 0.0f;
		const float stripHeight = gpu ? std::max((size.y - gap) * 0.5f, ImGui::GetFontSize()) : size.y;
		ImGui::BeginGroup();
		DrawTimelineStrip(ImVec2(size.x, stripHeight), false);
		if (gpu)
			DrawTimelineStrip(ImVec2(size.x, stripHeight), true);
		ImGui::EndGroup();
	}

	void PerformancePanel::DrawTimelineStrip(const ImVec2& size, bool gpu)
	{
		const FrameProfiler& profiler = FrameProfiler::Get();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const uint32_t capacity = profiler.GetCapacity();
		const uint32_t frames = profiler.GetRecordedFrameCount();
		const uint32_t stageCount = profiler.GetStageCount();

		// Scaled to the usual frames, not the slowest one (a hitch would flatten the rest for 600 frames): taller frames reach
		// the top edge, where their spike markers are.
		const float scale = GetTimelineScale(*m_Summary);
		const ImVec2 min = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton(gpu ? "GPUTimeline" : "Timeline", size);
		UI::ItemProbe::Record(gpu ? "Performance.GPUTimeline" : "Performance.Timeline");
		const bool hovered = ImGui::IsItemHovered();
		const ImVec2 max(min.x + size.x, min.y + size.y);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(min, max, UI::ToColorU32(colors.Chrome), ImGui::GetStyle().FrameRounding);
		drawList->PushClipRect(min, max, true);

		// Newest frame at the right edge, one column per frame of the ring.
		const float columnWidth = size.x / static_cast<float>(capacity);
		const float pixelsPerMs = size.y / scale;
		const float spikeMs = GetSpikeThreshold(*m_Summary);
		const float markerSize = std::max(ImGui::GetFontSize() * 0.25f, 2.0f);
		const ImU32 untimed = UI::ToColorU32(GetUntimedColor());
		for (uint32_t age = 0; age < frames; age++)
		{
			const FrameSample frame = profiler.GetFrame(age);
			// Frames whose GPU times did not arrive (yet) stay blank in the GPU's strip.
			if (gpu && !frame.HasGPU)
				continue;
			const float right = max.x - columnWidth * static_cast<float>(age);
			const float left = right - std::max(columnWidth, 1.0f);
			float y = max.y;
			float timed = 0.0f;
			for (FrameStageID stage = 0; stage < stageCount; stage++)
			{
				const bool gpuStage = profiler.GetStageKind(stage) == FrameStageKind::GPU;
				const float ms = gpu ? (gpuStage ? frame.GPUStageMs[stage] : 0.0f) : frame.StageMs[stage];
				if (ms <= 0.0f || (!gpu && !profiler.IsStageInWorkTime(stage)))
					continue;
				timed += ms;
				const float height = ms * pixelsPerMs;
				// Slivers below half a pixel are not drawn; they still count for the rest of the frame above them.
				if (height >= 0.5f)
					drawList->AddRectFilled(ImVec2(left, y - height), ImVec2(right, y), UI::ToColorU32(GetStageColor(stage)));
				y -= height;
			}
			if (gpu)
				continue;
			const float rest = std::max(frame.WorkMs - timed, 0.0f) * pixelsPerMs;
			if (rest >= 0.5f)
				drawList->AddRectFilled(ImVec2(left, y - rest), ImVec2(right, y), untimed);
			if (frame.WorkMs > spikeMs)
			{
				const float center = (left + right) * 0.5f;
				drawList->AddTriangleFilled(ImVec2(center - markerSize, min.y), ImVec2(center + markerSize, min.y), ImVec2(center, min.y + markerSize * 1.5f),
					UI::ToColorU32(colors.Error));
			}
		}

		// The frame times of 60 and 30 frames per second, where they fit; a label only where it does not cover the one below.
		float labelBottom = max.y + ImGui::GetFontSize();
		for (const float reference : c_ReferenceFrameMs)
		{
			if (reference > scale)
				continue;
			const float lineY = max.y - reference * pixelsPerMs;
			drawList->AddLine(ImVec2(min.x, lineY), ImVec2(max.x, lineY), UI::ToColorU32(colors.TextSecondary, 0.5f));
			if (labelBottom - lineY < ImGui::GetFontSize())
				continue;
			const std::string label = fmt::format("{:.1f} ms", reference);
			drawList->AddText(ImVec2(min.x + ImGui::GetStyle().FramePadding.x, lineY - ImGui::GetFontSize()), UI::ToColorU32(colors.TextSecondary), label.c_str());
			labelBottom = lineY;
		}
		const std::string top = fmt::format("{}{:g} ms", gpu ? "GPU \xC2\xB7 " : "CPU \xC2\xB7 ", scale);
		drawList->AddText(ImVec2(max.x - ImGui::CalcTextSize(top.c_str()).x - ImGui::GetStyle().FramePadding.x, min.y), UI::ToColorU32(colors.TextSecondary), top.c_str());
		drawList->PopClipRect();

		if (!hovered)
			return;
		const float mouseX = ImGui::GetIO().MousePos.x;
		const uint32_t age = static_cast<uint32_t>(std::max(0.0f, (max.x - mouseX) / columnWidth));
		if (age >= frames)
			return;
		const FrameSample frame = profiler.GetFrame(age);
		const float right = max.x - columnWidth * static_cast<float>(age);
		drawList->AddRectFilled(ImVec2(right - std::max(columnWidth, 1.0f), min.y), ImVec2(right, max.y), UI::ToColorU32(colors.Text, 0.15f));

		std::vector<std::pair<float, FrameStageID>> stages;
		std::vector<std::pair<float, FrameStageID>> gpuStages;
		float timed = 0.0f;
		for (FrameStageID stage = 0; stage < stageCount; stage++)
		{
			if (profiler.GetStageKind(stage) == FrameStageKind::GPU)
			{
				if (frame.HasGPU && frame.GPUStageMs[stage] > 0.0f)
					gpuStages.emplace_back(frame.GPUStageMs[stage], stage);
				continue;
			}
			if (frame.StageMs[stage] <= 0.0f)
				continue;
			stages.emplace_back(frame.StageMs[stage], stage);
			if (profiler.IsStageInWorkTime(stage))
				timed += frame.StageMs[stage];
		}
		const auto byTime = [](const auto& a, const auto& b) { return a.first > b.first; };
		std::sort(stages.begin(), stages.end(), byTime);
		std::sort(gpuStages.begin(), gpuStages.end(), byTime);
		ImGui::BeginTooltip();
		ImGui::Text("Frame %llu: %.3f ms%s", static_cast<unsigned long long>(frame.Index), frame.WorkMs, frame.WorkMs > spikeMs ? " (spike)" : "");
		ImGui::TextColored(colors.TextSecondary, "%.2f ms after the previous frame \xC2\xB7 workers %.0f%% \xC2\xB7 I/O %.0f%% \xC2\xB7 background %.0f%% busy",
			frame.IntervalMs, frame.WorkerBusyPercent, frame.IOBusyPercent, frame.BackgroundBusyPercent);
		for (const auto& [ms, stage] : stages)
		{
			Swatch(GetStageColor(stage));
			ImGui::SameLine();
			ImGui::Text("%s  %.3f ms%s", profiler.GetStageName(stage).c_str(), ms, profiler.IsStageInWorkTime(stage) ? "" : " (after the frame)");
		}
		Swatch(GetUntimedColor());
		ImGui::SameLine();
		ImGui::Text("Untimed  %.3f ms", std::max(frame.WorkMs - timed, 0.0f));
		if (frame.HasGPU)
		{
			ImGui::Text("GPU %.3f ms busy", frame.GPUBusyMs);
			ImGui::SameLine();
			ImGui::TextColored(colors.TextSecondary, "(known %u frames later)", frame.GPULatencyFrames);
			for (const auto& [ms, stage] : gpuStages)
			{
				Swatch(GetStageColor(stage));
				ImGui::SameLine();
				ImGui::Text("%s  %.3f ms", profiler.GetStageName(stage).c_str(), ms);
			}
		}
		else if (profiler.AreGPUTimesExpected())
		{
			ImGui::TextColored(colors.TextSecondary, "GPU: no times for this frame (yet)");
		}
		ImGui::EndTooltip();
	}

	void PerformancePanel::DrawStageTable()
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const FrameProfileSummary& summary = *m_Summary;
		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY
			| ImGuiTableFlags_NoSavedSettings;
		// The rest of the panel's height (the timeline left it room), so the panel itself never scrolls beside the table.
		if (!ImGui::BeginTable("Stages", 5, flags, ImVec2(0.0f, 0.0f)))
			return;
		// The totals come first and stay under the header while the stages scroll.
		ImGui::TableSetupScrollFreeze(0, 3);
		ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		for (const char* column : { "Avg ms", "p50 ms", "p95 ms", "Max ms" })
			ImGui::TableSetupColumn(column, ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableHeadersRow();
		UI::ItemProbe::Record("Performance.StagesHeader");
		const auto row = [](const ImVec4& color, const std::string& name, const FrameTimeStats& time, const ImVec4& textColor)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			Swatch(color);
			ImGui::SameLine();
			ImGui::TextColored(textColor, "%s", name.c_str());
			const float values[] = { time.Average, time.P50, time.P95, time.Max };
			for (int column = 0; column < 4; column++)
			{
				ImGui::TableSetColumnIndex(column + 1);
				ImGui::TextColored(textColor, "%.3f", values[column]);
			}
		};
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		row(colors.Text, "Frame", summary.Work, colors.Text);
		UI::ItemProbe::Record("Performance.FrameRow");
		row(GetUntimedColor(), "Untimed (rest of the frame)", summary.Untimed, colors.TextSecondary);
		for (FrameStageID stage = 0; stage < summary.Stages.size(); stage++)
		{
			const FrameStageSummary& entry = summary.Stages[stage];
			if (entry.Kind != FrameStageKind::CPU)
				continue; // The GPU's stages come in their own section
			const bool ran = entry.Time.Max > 0.0f;
			row(GetStageColor(stage), entry.InWorkTime ? entry.Name : entry.Name + " (after the frame)", entry.Time, ran ? colors.Text : colors.TextSecondary);
		}
		// The GPU's section: what bounds the frames, the GPU's busy time per frame, and its stages over the frames with GPU
		// times (they arrive a few frames late).
		if (ShowsGPU(summary))
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextColored(colors.Text, "GPU \xC2\xB7 %u frames, known %.1f frames late", summary.GPU.Frames, summary.GPU.LatencyFrames);
			UI::ItemProbe::Record("Performance.GPUSection");
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			const std::string bound = DescribeBound(summary);
			ImGui::TextColored(summary.GPU.BoundBy == FrameBound::Unknown ? colors.TextSecondary : colors.Text, "%s", bound.c_str());
			UI::ItemProbe::Record("Performance.Bound", true, 0, bound);
			row(colors.Text, "GPU busy", summary.GPU.Busy, summary.GPU.Frames > 0 ? colors.Text : colors.TextSecondary);
			for (FrameStageID stage = 0; stage < summary.Stages.size(); stage++)
			{
				const FrameStageSummary& entry = summary.Stages[stage];
				if (entry.Kind != FrameStageKind::GPU)
					continue;
				const bool ran = entry.Time.Max > 0.0f;
				row(GetStageColor(stage), entry.Name, entry.Time, ran ? colors.Text : colors.TextSecondary);
			}
		}
		ImGui::PopFont();
		ImGui::EndTable();
	}

}
