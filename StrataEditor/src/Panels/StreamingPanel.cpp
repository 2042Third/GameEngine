#include "Panels/StreamingPanel.h"

#include "UI/EditorFonts.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/StringUtils.h>

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <string_view>

namespace Strata
{

	namespace
	{

		constexpr double c_BytesPerMB = 1024.0 * 1024.0;
		// The labels in front of the bars, in text heights.
		constexpr float c_BarLabelWidthInFontSizes = 7.0f;
		// The upload sparkline's height, in text heights.
		constexpr float c_SparklineHeightInFontSizes = 2.5f;
		// From this width (in text heights) the overview goes beside the asset table, taking this share of the width.
		constexpr float c_SideBySideWidthInFontSizes = 60.0f;
		constexpr float c_OverviewShare = 0.42f;
		// The table's columns.
		constexpr int c_ColumnAsset = 0;
		constexpr int c_ColumnType = 1;
		constexpr int c_ColumnState = 2;
		constexpr int c_ColumnGpu = 3;
		constexpr int c_ColumnCpu = 4;
		constexpr int c_ColumnLastUsed = 5;
		constexpr int c_ColumnPins = 6;

		double ToMB(uint64_t bytes)
		{
			return static_cast<double>(bytes) / c_BytesPerMB;
		}

		// A labeled bar: `fraction` of it filled in `color`, `value` written over it.
		void DrawBar(std::string_view label, float fraction, const ImVec4& color, std::string_view value)
		{
			const UI::ThemeColors& colors = UI::GetThemeColors();
			const ImGuiStyle& style = ImGui::GetStyle();
			const float labelWidth = ImGui::GetFontSize() * c_BarLabelWidthInFontSizes;
			const float startX = ImGui::GetCursorPosX();
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(label.data(), label.data() + label.size());
			ImGui::SameLine(startX + labelWidth);

			const ImVec2 min = ImGui::GetCursorScreenPos();
			const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize()), ImGui::GetFrameHeight());
			ImGui::Dummy(size);
			const ImVec2 max(min.x + size.x, min.y + size.y);
			ImDrawList* drawList = ImGui::GetWindowDrawList();
			drawList->AddRectFilled(min, max, UI::ToColorU32(colors.Control), style.FrameRounding);
			const float filled = size.x * std::clamp(fraction, 0.0f, 1.0f);
			if (filled > 0.0f)
				drawList->AddRectFilled(min, ImVec2(min.x + filled, max.y), UI::ToColorU32(color, 0.75f), style.FrameRounding);
			const ImVec2 textSize = ImGui::CalcTextSize(value.data(), value.data() + value.size());
			drawList->AddText(ImVec2(min.x + style.FramePadding.x, min.y + (size.y - textSize.y) * 0.5f), UI::ToColorU32(colors.Text), value.data(),
				value.data() + value.size());
		}

		std::string DescribePoolBar(uint64_t used, uint64_t budget)
		{
			if (budget == AssetResidencyBudgets::c_Unlimited)
				return fmt::format("{:.1f} MB (no budget)", ToMB(used));
			return fmt::format("{:.1f} / {:.1f} MB ({:.0f}%)", ToMB(used), ToMB(budget), budget > 0 ? 100.0 * static_cast<double>(used) / static_cast<double>(budget) : 100.0);
		}

		float GetFraction(uint64_t used, uint64_t budget)
		{
			if (budget == AssetResidencyBudgets::c_Unlimited || budget == 0)
				return used > 0 && budget == 0 ? 1.0f : 0.0f;
			return static_cast<float>(static_cast<double>(used) / static_cast<double>(budget));
		}

		const ImVec4& GetStateColor(AssetState state)
		{
			const UI::ThemeColors& colors = UI::GetThemeColors();
			switch (state)
			{
				case AssetState::Loading: return colors.Info;
				case AssetState::Failed:  return colors.Error;
				case AssetState::Ready:   return colors.Text;
				case AssetState::Unloaded: break;
			}
			return colors.TextSecondary;
		}

	}

	BudgetPressure GetBudgetPressure(uint64_t used, uint64_t budget)
	{
		if (budget == AssetResidencyBudgets::c_Unlimited)
			return BudgetPressure::Unbudgeted;
		if (used > budget)
			return BudgetPressure::Over;
		return static_cast<double>(used) >= c_HighBudgetPressure * static_cast<double>(budget) ? BudgetPressure::High : BudgetPressure::Comfortable;
	}

	BudgetPressure GetWorstPoolPressure(const AssetManagerStats& stats)
	{
		BudgetPressure worst = BudgetPressure::Unbudgeted;
		for (AssetMemoryPool pool : c_AssetMemoryPools)
			worst = std::max(worst, GetBudgetPressure(GetPoolBytes(stats.Resident, pool), stats.Budgets.GetPoolBudget(pool)));
		return worst;
	}

	const ImVec4& GetBudgetPressureColor(BudgetPressure pressure)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		switch (pressure)
		{
			case BudgetPressure::Comfortable: return colors.Accent;
			case BudgetPressure::High:        return colors.Warning;
			case BudgetPressure::Over:        return colors.Error;
			case BudgetPressure::Unbudgeted:  break;
		}
		return colors.TextSecondary;
	}

	void StreamingPanel::OnUpdate(EditorPanelContext&)
	{
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		if (!manager)
		{
			m_Stats.reset();
			return;
		}
		m_Stats = manager->GetStats();
		// One sample per frame of the manager (its Update advances the counter).
		if (m_Stats->Frame != m_LastSampledFrame)
		{
			m_LastSampledFrame = m_Stats->Frame;
			m_UploadHistory[m_UploadCursor] = static_cast<float>(ToMB(m_Stats->UploadedBytesLastFrame));
			m_UploadCursor = (m_UploadCursor + 1) % c_UploadHistoryFrames;
			m_UploadSamples = std::min(m_UploadSamples + 1, c_UploadHistoryFrames);
		}
	}

	void StreamingPanel::OnImGuiRender(EditorPanelContext&)
	{
		if (!m_Stats)
		{
			ImGui::TextDisabled("No asset manager is active");
			return;
		}
		const AssetManagerStats& stats = *m_Stats;
		const UI::ThemeColors& colors = UI::GetThemeColors();

		ImGui::Text("%u registered", stats.RegisteredAssets);
		ImGui::SameLine();
		ImGui::TextColored(colors.TextSecondary, "\xC2\xB7");
		ImGui::SameLine();
		ImGui::Text("%u ready", stats.LoadedAssets);
		ImGui::SameLine();
		ImGui::TextColored(colors.TextSecondary, "\xC2\xB7");
		ImGui::SameLine();
		ImGui::TextColored(stats.LoadingAssets > 0 ? colors.Info : colors.Text, "%u streaming", stats.LoadingAssets);
		ImGui::SameLine();
		ImGui::TextColored(colors.TextSecondary, "\xC2\xB7");
		ImGui::SameLine();
		ImGui::TextColored(stats.FailedAssets > 0 ? colors.Error : colors.Text, "%u failed", stats.FailedAssets);
		ImGui::SameLine();
		ImGui::TextColored(colors.TextSecondary, "\xC2\xB7 %u pinned \xC2\xB7 frame %llu", stats.PinnedAssets, static_cast<unsigned long long>(stats.Frame));

		// Wide panels (the bottom dock) put the overview beside the asset table, which then gets the full height; narrow ones
		// stack them.
		const float available = ImGui::GetContentRegionAvail().x;
		const bool sideBySide = available >= ImGui::GetFontSize() * c_SideBySideWidthInFontSizes;
		if (sideBySide)
			ImGui::BeginChild("Overview", ImVec2(available * c_OverviewShare, 0.0f), ImGuiChildFlags_None);
		if (UI::SectionHeader("Streaming.Memory", "Memory"))
			DrawPools(stats);
		if (UI::SectionHeader("Streaming.Loads", "Loads and uploads"))
		{
			DrawStreaming(stats);
			DrawUploads(stats);
		}
		if (sideBySide)
		{
			ImGui::EndChild();
			ImGui::SameLine();
			ImGui::BeginChild("AssetList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
			DrawTable();
			ImGui::EndChild();
		}
		else if (UI::SectionHeader("Streaming.Assets", "Assets"))
		{
			DrawTable();
		}
	}

	void StreamingPanel::DrawPools(const AssetManagerStats& stats)
	{
		for (AssetMemoryPool pool : c_AssetMemoryPools)
		{
			const uint64_t used = GetPoolBytes(stats.Resident, pool);
			const uint64_t budget = stats.Budgets.GetPoolBudget(pool);
			const char* label = pool == AssetMemoryPool::GpuTextures ? "GPU textures" : (pool == AssetMemoryPool::GpuBuffers ? "GPU buffers" : "CPU");
			ImGui::PushID(static_cast<int>(pool));
			DrawBar(label, GetFraction(used, budget), GetBudgetPressureColor(GetBudgetPressure(used, budget)), DescribePoolBar(used, budget));
			ImGui::PopID();
		}
	}

	void StreamingPanel::DrawStreaming(const AssetManagerStats& stats)
	{
		const uint64_t budget = stats.Budgets.InFlightBytes;
		DrawBar("In flight", GetFraction(stats.InFlightBytes, budget), GetBudgetPressureColor(GetBudgetPressure(stats.InFlightBytes, budget)),
			fmt::format("{:.1f} / {:.1f} MB \xC2\xB7 {} loads, {} reads", ToMB(stats.InFlightBytes), ToMB(budget), stats.InFlightLoads, stats.OutstandingReads));
		ImGui::Text("Queued: %u high \xC2\xB7 %u normal \xC2\xB7 %u low", stats.QueuedLoads[0], stats.QueuedLoads[1], stats.QueuedLoads[2]);
		ImGui::Text("Evictions %llu (%llu checked) \xC2\xB7 cancellations %llu \xC2\xB7 staging releases %llu", static_cast<unsigned long long>(stats.Evictions),
			static_cast<unsigned long long>(stats.EvictionChecks), static_cast<unsigned long long>(stats.Cancellations),
			static_cast<unsigned long long>(stats.StagingReleases));
	}

	void StreamingPanel::DrawUploads(const AssetManagerStats& stats)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const float budgetMB = static_cast<float>(ToMB(stats.Budgets.UploadBytesPerFrame));
		float peakMB = budgetMB;
		for (uint32_t sample = 0; sample < m_UploadSamples; sample++)
			peakMB = std::max(peakMB, m_UploadHistory[sample]);

		// Uploaded MB per frame, oldest at the left, against the frame's upload budget (the line).
		const ImVec2 min = ImGui::GetCursorScreenPos();
		const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize()), ImGui::GetFontSize() * c_SparklineHeightInFontSizes);
		ImGui::Dummy(size);
		const ImVec2 max(min.x + size.x, min.y + size.y);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(min, max, UI::ToColorU32(colors.Chrome), ImGui::GetStyle().FrameRounding);
		const float columnWidth = size.x / static_cast<float>(c_UploadHistoryFrames);
		const uint32_t first = (m_UploadCursor + c_UploadHistoryFrames - m_UploadSamples) % c_UploadHistoryFrames;
		for (uint32_t sample = 0; sample < m_UploadSamples; sample++)
		{
			const float value = m_UploadHistory[(first + sample) % c_UploadHistoryFrames];
			if (value <= 0.0f || peakMB <= 0.0f)
				continue;
			const float x = min.x + columnWidth * static_cast<float>(c_UploadHistoryFrames - m_UploadSamples + sample);
			const float height = size.y * std::min(value / peakMB, 1.0f);
			drawList->AddRectFilled(ImVec2(x, max.y - height), ImVec2(x + std::max(columnWidth - 1.0f, 1.0f), max.y), UI::ToColorU32(colors.Accent, 0.8f));
		}
		if (peakMB > 0.0f)
		{
			const float budgetY = max.y - size.y * std::min(budgetMB / peakMB, 1.0f);
			drawList->AddLine(ImVec2(min.x, budgetY), ImVec2(max.x, budgetY), UI::ToColorU32(colors.TextSecondary, 0.6f));
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("GPU megabytes uploaded per frame over the last %u frames; the line is the frame's upload budget", c_UploadHistoryFrames);

		ImGui::Text("Uploads %.2f MB last frame \xC2\xB7 at most %.2f MB of %.0f MB \xC2\xB7 finalize %.2f ms (at most %.2f of %.1f ms)", ToMB(stats.UploadedBytesLastFrame),
			ToMB(stats.UploadedBytesWindowMax), budgetMB, stats.FinalizeMsLastFrame, stats.FinalizeMsWindowMax, stats.Budgets.FinalizeMsPerFrame);
	}

	const StreamingPanel::Label& StreamingPanel::GetLabel(const AssetManagerBase& manager, AssetHandle handle)
	{
		auto [it, inserted] = m_Labels.try_emplace(handle);
		if (inserted)
		{
			// An asset registered since the names were read.
			if (const std::optional<AssetMetadata> metadata = manager.GetMetadata(handle))
			{
				it->second.Text = metadata->Path.empty() ? metadata->Name : metadata->Path;
				it->second.Revealable = !metadata->Path.empty() && !metadata->IsBuiltin();
			}
			it->second.Lower = StringUtils::ToLower(it->second.Text);
		}
		return it->second;
	}

	void StreamingPanel::RefreshTable(const AssetManagerBase& manager)
	{
		// The names, again only when the manager or its metadata changed: then all of them in one read.
		if (&manager != m_LabelManager || manager.GetMetadataVersion() != m_LabelVersion)
		{
			m_LabelManager = &manager;
			m_LabelVersion = manager.GetMetadataVersion();
			m_Labels.clear();
			for (const AssetMetadata& metadata : manager.GetAllMetadata())
			{
				Label& label = m_Labels[metadata.Handle];
				label.Text = metadata.Path.empty() ? metadata.Name : metadata.Path;
				label.Lower = StringUtils::ToLower(label.Text);
				label.Revealable = !metadata.Path.empty() && !metadata.IsBuiltin();
			}
		}

		// Last uses count back from the frame the samples were read in, not the frame they are shown in.
		m_SampleFrame = manager.GetFrameIndex();
		manager.GetResidencySamples(m_Samples);
		m_SampleLabels.resize(m_Samples.size());
		for (size_t index = 0; index < m_Samples.size(); index++)
			m_SampleLabels[index] = &GetLabel(manager, m_Samples[index].Handle);
		m_RowsSorted = false;
	}

	void StreamingPanel::SortRows()
	{
		m_Order.resize(m_Samples.size());
		for (uint32_t index = 0; index < m_Order.size(); index++)
			m_Order[index] = index;
		const int column = m_SortColumn;
		const bool ascending = m_SortAscending;
		std::sort(m_Order.begin(), m_Order.end(), [this, column, ascending](uint32_t first, uint32_t second)
		{
			const AssetResidencySample& a = m_Samples[first];
			const AssetResidencySample& b = m_Samples[second];
			int order = 0;
			const auto compare = [&order](const auto& x, const auto& y) { order = x < y ? -1 : (y < x ? 1 : 0); };
			switch (column)
			{
				case c_ColumnAsset:    compare(m_SampleLabels[first]->Lower, m_SampleLabels[second]->Lower); break;
				case c_ColumnType:     compare(static_cast<int>(a.Type), static_cast<int>(b.Type)); break;
				case c_ColumnState:    compare(static_cast<int>(a.State), static_cast<int>(b.State)); break;
				case c_ColumnGpu:      compare(a.Usage.GetGpu(), b.Usage.GetGpu()); break;
				case c_ColumnCpu:      compare(a.Usage.Cpu, b.Usage.Cpu); break;
				case c_ColumnLastUsed: compare(a.LastRequestedFrame, b.LastRequestedFrame); break;
				case c_ColumnPins:     compare(a.PinCount, b.PinCount); break;
				default: break;
			}
			if (order == 0)
				return a.Handle < b.Handle;
			return ascending ? order < 0 : order > 0;
		});
		m_RowsSorted = true;
		m_VisibleDirty = true;
	}

	void StreamingPanel::DrawTable()
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		if (manager && (m_LastTableRefresh < 0.0 || ImGui::GetTime() - m_LastTableRefresh >= c_TableRefreshSeconds || ImGui::GetTime() < m_LastTableRefresh))
		{
			RefreshTable(*manager);
			m_LastTableRefresh = ImGui::GetTime();
		}

		UI::TextField("Streaming.Filter", "Filter", m_Filter, "Part of the asset's path or name");

		// The sort is the session's (and the default one each start): restoring a sorted column from imgui.ini moves it to
		// the front in the vendored ImGui.
		const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
			| ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings;
		if (!ImGui::BeginTable("Assets", 7, flags, ImVec2(0.0f, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFrameHeight() * 4.0f))))
			return;
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 4.0f, c_ColumnAsset);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch, 1.2f, c_ColumnType);
		ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch, 1.2f, c_ColumnState);
		ImGui::TableSetupColumn("GPU MB", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 1.0f,
			c_ColumnGpu);
		ImGui::TableSetupColumn("CPU MB", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 1.0f, c_ColumnCpu);
		ImGui::TableSetupColumn("Last used", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 1.1f, c_ColumnLastUsed);
		ImGui::TableSetupColumn("Pins", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_PreferSortDescending, 0.7f, c_ColumnPins);
		ImGui::TableHeadersRow();

		if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty)
		{
			if (specs->SpecsCount > 0)
			{
				m_SortColumn = static_cast<int>(specs->Specs[0].ColumnUserID);
				m_SortAscending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
			}
			specs->SpecsDirty = false;
			m_RowsSorted = false;
		}
		if (!m_RowsSorted)
			SortRows();
		if (m_VisibleDirty || m_VisibleFilter != m_Filter)
		{
			const std::string filter = StringUtils::ToLower(m_Filter);
			m_Visible.clear();
			for (const uint32_t index : m_Order)
			{
				if (filter.empty() || m_SampleLabels[index]->Lower.find(filter) != std::string::npos)
					m_Visible.push_back(index);
			}
			m_VisibleFilter = m_Filter;
			m_VisibleDirty = false;
		}

		AssetHandle reveal = UUID::Null();
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(m_Visible.size()));
		while (clipper.Step())
		{
			for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; line++)
			{
				const uint32_t index = m_Visible[static_cast<size_t>(line)];
				const AssetResidencySample& sample = m_Samples[index];
				const Label& label = *m_SampleLabels[index];
				// Only assets of the project's files can be shown in the Content Browser.
				const bool revealable = label.Revealable && !sample.IsMemoryAsset;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(c_ColumnAsset);
				ImGui::PushID(line);
				if (ImGui::Selectable(label.Text.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) && revealable)
					reveal = sample.Handle;
				if (line == 0)
					UI::ItemProbe::Record("Streaming.FirstAsset", revealable);
				if (ImGui::IsItemHovered())
				{
					ImGui::SetTooltip("%s\n%s", sample.Handle.ToString().c_str(),
						revealable ? "Click to show it in the Content Browser" : "A built-in or memory asset: it has no file in the project");
				}
				ImGui::PopID();
				ImGui::TableSetColumnIndex(c_ColumnType);
				ImGui::TextUnformatted(AssetTypeToString(sample.Type));
				ImGui::TableSetColumnIndex(c_ColumnState);
				ImGui::TextColored(GetStateColor(sample.State), "%s", AssetStateToString(sample.State));
				ImGui::TableSetColumnIndex(c_ColumnGpu);
				ImGui::Text("%.2f", ToMB(sample.Usage.GetGpu()));
				ImGui::TableSetColumnIndex(c_ColumnCpu);
				ImGui::Text("%.2f", ToMB(sample.Usage.Cpu));
				ImGui::TableSetColumnIndex(c_ColumnLastUsed);
				if (sample.LastRequestedFrame == 0)
					ImGui::TextColored(colors.TextSecondary, "never");
				else if (sample.LastRequestedFrame >= m_SampleFrame)
					ImGui::TextUnformatted("this frame");
				else
					ImGui::Text("%llu frames ago", static_cast<unsigned long long>(m_SampleFrame - sample.LastRequestedFrame));
				ImGui::TableSetColumnIndex(c_ColumnPins);
				if (sample.PinCount > 0)
					ImGui::Text("%u", sample.PinCount);
			}
		}
		ImGui::EndTable();
		if (reveal.IsValid() && m_RevealAsset)
			m_RevealAsset(reveal);
	}

}
