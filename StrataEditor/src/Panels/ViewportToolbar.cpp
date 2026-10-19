#include "Panels/ViewportToolbar.h"

#include "Editor/EditorContext.h"
#include "Editor/ViewportRenderer.h"
#include "UI/EditorFonts.h"
#include "UI/EditorPanelRegistry.h"
#include "UI/Theme.h"

#include <Strata/Core/FrameProfiler.h>
#include <Strata/Renderer/SceneRenderer.h>

#include <nvrhi/nvrhi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace Strata::ViewportToolbar
{

	namespace
	{

		// The toolbar's bottom border, in text heights (2 pixels at the base text size).
		constexpr float c_BorderInFontSizes = 1.0f / 7.0f;

	}

	Layout Compute()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const ImVec2 panelMin = ImGui::GetCursorScreenPos();
		const ImVec2 panelSize = ImGui::GetContentRegionAvail();
		// The chips' row with half the item spacing above and below: the toolbar takes little of the image's height.
		const float height = ImGui::GetFrameHeight() + style.ItemSpacing.y;
		const float border = std::max(1.0f, std::floor(ImGui::GetFontSize() * c_BorderInFontSizes));
		Layout layout;
		layout.Min = panelMin;
		layout.Max = ImVec2(panelMin.x + panelSize.x, panelMin.y + height);
		layout.ChipsMin = ImVec2(panelMin.x + style.ItemSpacing.x, panelMin.y + style.ItemSpacing.y * 0.5f);
		layout.ImageMin = ImVec2(panelMin.x, layout.Max.y + border);
		layout.ImageSize = ImVec2(std::max(panelSize.x, 0.0f), std::max(panelSize.y - height - border, 0.0f));
		return layout;
	}

	void DrawBackground(ImDrawList* drawList, const Layout& layout, const EditorContext& context)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		drawList->AddRectFilled(layout.Min, layout.Max, ImGui::GetColorU32(colors.Chrome));
		drawList->AddRectFilled(ImVec2(layout.Min.x, layout.Max.y), ImVec2(layout.Max.x, layout.ImageMin.y),
			ImGui::GetColorU32(context.IsPlaying() ? GetPlayStateColor(context) : colors.Border));
	}

	const ImVec4& GetPlayStateColor(const EditorContext& context)
	{
		const UI::ThemeColors::PlayStateColors& colors = UI::GetThemeColors().PlayState;
		if (context.IsPaused())
			return colors.Paused;
		switch (context.GetSceneState())
		{
			case SceneState::Play:     return colors.Play;
			case SceneState::Simulate: return colors.Simulate;
			case SceneState::Edit:     break;
		}
		return colors.Edit;
	}

	void DrawTextBox(ImDrawList* drawList, const ImVec2& position, const std::vector<std::string>& lines, const ImVec4& color)
	{
		const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
		const ImVec2 padding = ImGui::GetStyle().FramePadding;
		float width = 0.0f;
		for (const std::string& line : lines)
			width = std::max(width, ImGui::CalcTextSize(line.c_str()).x);
		const ImVec2 size(width + padding.x * 2.0f, lineHeight * static_cast<float>(lines.size()) + padding.y * 2.0f);
		drawList->AddRectFilled(position, ImVec2(position.x + size.x, position.y + size.y), ImGui::GetColorU32(UI::GetThemeColors().Overlay),
			ImGui::GetStyle().FrameRounding);
		for (size_t index = 0; index < lines.size(); index++)
			drawList->AddText(ImVec2(position.x + padding.x, position.y + padding.y + lineHeight * static_cast<float>(index)), ImGui::GetColorU32(color),
				lines[index].c_str());
	}

	ImTextureID ToTextureID(nvrhi::ITexture* texture)
	{
		return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture));
	}

	void DrawStatsOverlay(ImDrawList* drawList, const ImVec2& position, const ViewportRenderer& renderer, const ViewportView& view,
		const EditorFrameStats& frame)
	{
		const glm::uvec2 size = renderer.GetSize();
		// The GPU's time of the latest frame it is known for (2-3 frames late): the total and the three largest stages.
		const SceneRendererGPUStats& gpu = renderer.GetGPUStats();
		// The GPU-driven path's counts are the GPU's, as late as its times.
		const SceneRendererStats stats = AddGPUCounts(renderer.GetStats(), gpu);
		std::string gpuLine = "GPU: no times (yet)";
		if (gpu.HasTimes)
		{
			std::vector<GPUStageTime> largest = gpu.StageMs;
			const size_t shown = std::min<size_t>(largest.size(), 3);
			std::partial_sort(largest.begin(), largest.begin() + static_cast<std::ptrdiff_t>(shown), largest.end(),
				[](const GPUStageTime& a, const GPUStageTime& b) { return a.Milliseconds > b.Milliseconds; });
			gpuLine = fmt::format("GPU {:.2f} ms", gpu.BusyMs);
			for (size_t index = 0; index < shown; index++)
			{
				// The stage's name without its "GPU." prefix.
				const std::string& name = FrameProfiler::Get().GetStageName(largest[index].Stage);
				gpuLine += fmt::format("{}{} {:.2f}", index == 0 ? " (" : ", ", name.starts_with("GPU.") ? name.substr(4) : name, largest[index].Milliseconds);
			}
			if (shown > 0)
				gpuLine += ")";
		}
		// The frame's own time: while the editor idles, its frame rate is capped, which says nothing about what frames cost.
		const std::vector<std::string> lines = {
			fmt::format("{:.2f} ms ({:.0f} FPS{})", frame.WorkMilliseconds, frame.FramesPerSecond, frame.Idle ? ", idle" : ""),
			fmt::format("{} x {} pixels", size.x, size.y),
			gpuLine,
			fmt::format("{} draw calls ({} opaque, {} shadow), {} instances", stats.DrawCalls, stats.OpaqueDrawCalls, stats.ShadowDrawCalls, stats.Instances),
			fmt::format("{} draw commands{}, {} occluded, {} occluded casters", stats.DrawCommands, stats.GPUDriven ? " (culled by the GPU)" : "",
				stats.OccludedProxies, stats.OccludedCasters),
			fmt::format("{} triangles, {} lights, {} materials", stats.Triangles, stats.Lights, stats.Materials),
			fmt::format("{} visible of {} proxies, {} updated", stats.VisibleProxies, stats.VisibleProxies + stats.CulledInstances, stats.ProxyUpdates),
			fmt::format("extract {:.2f} ms, sort {:.2f} ms", stats.ExtractMs, stats.SortMs),
			fmt::format("{:.1f} KB instances, {:.1f} KB materials uploaded", static_cast<double>(stats.InstanceBytesUploaded) / 1024.0,
				static_cast<double>(stats.MaterialBytesUploaded) / 1024.0),
			fmt::format("{} assets requested{}, {} prefetched, {} loading", stats.RequestsByVisibility, stats.FeedbackRequests ? " (GPU feedback)" : "",
				stats.PrefetchRequests, stats.PendingAssets),
			view.FromScene ? std::string("Scene camera") : std::string("Editor camera")
		};
		// Numbers read best in the monospaced font.
		const UI::ThemeColors& colors = UI::GetThemeColors();
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		DrawTextBox(drawList, position, lines, colors.Text);
		ImGui::PopFont();
	}

}
