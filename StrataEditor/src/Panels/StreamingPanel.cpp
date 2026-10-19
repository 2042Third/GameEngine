#include "Panels/StreamingPanel.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorWorld.h"
#include "UI/EditorActions.h"
#include "UI/EditorFonts.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/StringUtils.h>
#include <Strata/Scene/Scene.h>
#include <Strata/World/WorldAsset.h>
#include <Strata/World/WorldStreamingSystem.h>

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <string_view>
#include <utility>

namespace Strata
{

	namespace
	{

		constexpr double c_BytesPerMB = 1024.0 * 1024.0;
		// The labels in front of the bars, in text heights.
		constexpr float c_BarLabelWidthInFontSizes = 7.0f;
		// The upload sparkline's height, in text heights, and the room above its highest value (the budget line stays inside).
		constexpr float c_SparklineHeightInFontSizes = 2.5f;
		constexpr float c_SparklineHeadroom = 1.1f;
		// From this width (in text heights) the overview goes beside the asset table, taking this share of the width.
		constexpr float c_SideBySideWidthInFontSizes = 60.0f;
		constexpr float c_OverviewShare = 0.42f;
		// The background of failed rows, as a share of the error color.
		constexpr float c_FailedRowAlpha = 0.14f;
		// The table's columns.
		constexpr int c_ColumnAsset = 0;
		constexpr int c_ColumnType = 1;
		constexpr int c_ColumnState = 2;
		constexpr int c_ColumnGpu = 3;
		constexpr int c_ColumnMip = 4;
		constexpr int c_ColumnCpu = 5;
		constexpr int c_ColumnLastUsed = 6;
		constexpr int c_ColumnPins = 7;
		constexpr int c_ColumnCount = 8;
		// Where the table is narrow these columns go first, in this order (the least telling first), so the asset's name keeps
		// at least c_MinAssetColumnInFontSizes text heights.
		constexpr std::array<int, 4> c_OptionalColumns = { c_ColumnPins, c_ColumnCpu, c_ColumnLastUsed, c_ColumnMip };
		constexpr float c_MinAssetColumnInFontSizes = 14.0f;
		// Between a sub-asset's file and its key ("›").
		constexpr std::string_view c_SubAssetSeparator = " \xE2\x80\xBA ";

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

		// The share of a budget in use, for ordering pools (a zero budget counts as full, and as infinitely over once used).
		double GetBudgetShare(uint64_t used, uint64_t budget)
		{
			if (budget == 0)
				return used > 0 ? std::numeric_limits<double>::infinity() : 1.0;
			return static_cast<double>(used) / static_cast<double>(budget);
		}

		// Whether `known` refers to `manager`. Compared by ownership, so a manager created where a destroyed one was is never
		// taken for it.
		bool IsSameManager(const std::weak_ptr<AssetManagerBase>& known, const Ref<AssetManagerBase>& manager)
		{
			return manager && !known.owner_before(manager) && !manager.owner_before(known);
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

	std::optional<AssetMemoryPool> GetMostPressuredPool(const AssetManagerStats& stats)
	{
		std::optional<AssetMemoryPool> most;
		BudgetPressure mostPressure = BudgetPressure::Unbudgeted;
		double mostShare = 0.0;
		for (AssetMemoryPool pool : { AssetMemoryPool::GpuTextures, AssetMemoryPool::GpuBuffers, AssetMemoryPool::Cpu })
		{
			const uint64_t used = GetPoolBytes(stats.Resident, pool);
			const uint64_t budget = stats.Budgets.GetPoolBudget(pool);
			const BudgetPressure pressure = GetBudgetPressure(used, budget);
			if (pressure == BudgetPressure::Unbudgeted)
				continue;
			// Among comfortable pools the first one stays (GPU textures), so the figure does not hop between pools that
			// are all far from their budgets; among pools close to or over theirs, the fullest one leads.
			const double share = GetBudgetShare(used, budget);
			if (!most || pressure > mostPressure || (pressure == mostPressure && pressure != BudgetPressure::Comfortable && share > mostShare))
			{
				most = pool;
				mostPressure = pressure;
				mostShare = share;
			}
		}
		return most;
	}

	BudgetPressure GetWorstPoolPressure(const AssetManagerStats& stats)
	{
		const std::optional<AssetMemoryPool> pool = GetMostPressuredPool(stats);
		return pool ? GetBudgetPressure(GetPoolBytes(stats.Resident, *pool), stats.Budgets.GetPoolBudget(*pool)) : BudgetPressure::Unbudgeted;
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

	const ImVec4& GetAssetStateColor(AssetState state)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		switch (state)
		{
			case AssetState::Loading:  return colors.Warning;
			case AssetState::Failed:   return colors.Error;
			case AssetState::Ready:    return colors.Text;
			case AssetState::Unloaded: break;
		}
		return colors.TextSecondary;
	}

	const char* GetMemoryPoolName(AssetMemoryPool pool)
	{
		switch (pool)
		{
			case AssetMemoryPool::GpuTextures: return "GPU textures";
			case AssetMemoryPool::GpuBuffers:  return "GPU buffers";
			case AssetMemoryPool::Cpu:         break;
		}
		return "CPU";
	}

	const char* GetMemoryPoolShortName(AssetMemoryPool pool)
	{
		switch (pool)
		{
			case AssetMemoryPool::GpuTextures: return "textures";
			case AssetMemoryPool::GpuBuffers:  return "buffers";
			case AssetMemoryPool::Cpu:         break;
		}
		return "CPU";
	}

	StreamingPanel::StreamingPanel()
		: m_Table(CreateScope<TableBuild>()), m_NextTable(CreateScope<TableBuild>())
	{
	}

	StreamingPanel::~StreamingPanel()
	{
		// The job works on m_NextTable.
		WaitForTableBuild();
	}

	std::string StreamingPanel::GetAssetLabel(const AssetMetadata& metadata)
	{
		if (metadata.Path.empty())
			return metadata.Name;
		if (!metadata.IsSubAsset())
			return metadata.Path;
		// Sub-assets share their parent's path: the key tells them apart, the name (when it says more) what they are.
		const std::string& key = metadata.SubAssetKey.empty() ? metadata.Name : metadata.SubAssetKey;
		std::string label = metadata.Path;
		label += c_SubAssetSeparator;
		label += key;
		if (!metadata.Name.empty() && metadata.Name != key)
			label += " (" + metadata.Name + ")";
		return label;
	}

	const char* StreamingPanel::GetTypeLabel(AssetType type)
	{
		if (type == AssetType::AnimationClip)
			return "Animation";
		if (type == AssetType::AnimationController)
			return "Controller";
		return AssetTypeToString(type);
	}

	void StreamingPanel::OnUpdate(EditorPanelContext& context)
	{
		// Only a running game has physics; reading its capacity costs a service lookup.
		m_Physics.reset();
		if (context.Context.IsPlaying())
		{
			const Ref<Scene>& scene = context.Context.GetActiveScene();
			if (const IPhysicsCapacity* physics = scene && scene->IsRunning() ? scene->GetService<IPhysicsCapacity>() : nullptr)
				m_Physics = physics->GetCapacityInfo();
		}

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

	void StreamingPanel::OnDetach(EditorPanelContext&)
	{
		WaitForTableBuild();
	}

	void StreamingPanel::OnImGuiRender(EditorPanelContext& context)
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
		ImGui::TextColored(stats.LoadingAssets > 0 ? GetAssetStateColor(AssetState::Loading) : colors.Text, "%u streaming", stats.LoadingAssets);
		ImGui::SameLine();
		ImGui::TextColored(colors.TextSecondary, "\xC2\xB7");
		ImGui::SameLine();
		ImGui::TextColored(stats.FailedAssets > 0 ? GetAssetStateColor(AssetState::Failed) : colors.Text, "%u failed", stats.FailedAssets);
		ImGui::SameLine();
		ImGui::TextColored(colors.TextSecondary, "\xC2\xB7 %u pinned \xC2\xB7 frame %llu", stats.PinnedAssets, static_cast<unsigned long long>(stats.Frame));

		// Wide panels (the bottom dock) put the overview beside the asset table, which then gets the full height; narrow ones
		// stack them, also wide ones whose share for the table could not show the asset names beside the essential columns.
		UpdateColumnWidths();
		const float available = ImGui::GetContentRegionAvail().x;
		const bool sideBySide = available >= ImGui::GetFontSize() * c_SideBySideWidthInFontSizes
			&& available * (1.0f - c_OverviewShare) - ImGui::GetStyle().ItemSpacing.x >= GetTableWidth(0);
		if (sideBySide)
			ImGui::BeginChild("Overview", ImVec2(available * c_OverviewShare, 0.0f), ImGuiChildFlags_None);
		// The world first while one streams (the game's while playing, the editor's otherwise): the cells are what streams then.
		const WorldStreamer* streamer = nullptr;
		bool editable = false;
		if (context.Context.IsPlaying())
		{
			const Ref<Scene>& scene = context.Context.GetActiveScene();
			const WorldStreamingSystem* system = scene && scene->IsRunning() ? scene->GetSystem<WorldStreamingSystem>() : nullptr;
			streamer = system && system->GetStreamer() && system->GetStreamer()->GetWorld().IsValid() ? system->GetStreamer() : nullptr;
		}
		else if (const EditorWorld* world = context.Context.GetWorld())
		{
			streamer = &world->GetStreamer();
			editable = true;
		}
		if (streamer)
		{
			if (m_RevealWorld)
				ImGui::SetNextItemOpen(true);
			// Formatted into a buffer: an open world's frames allocate nothing for it.
			const WorldStreamingStats worldStats = streamer->GetStats();
			const Ref<WorldAsset> asset = streamer->GetWorldAsset();
			std::array<char, 160> summary {};
			std::snprintf(summary.data(), summary.size(), "%s \xC2\xB7 %u loaded \xC2\xB7 %u loading%s", asset ? asset->GetData().Name.c_str() : "World",
				worldStats.CellsLoaded, worldStats.CellsLoading, worldStats.CellsFailed > 0 ? " \xC2\xB7 failures" : "");
			if (UI::SectionHeader("Streaming.World", "World", ImGuiTreeNodeFlags_DefaultOpen, summary.data()))
				DrawWorld(context, *streamer, editable);
		}
		m_RevealWorld = false;
		if (UI::SectionHeader("Streaming.Memory", "Memory"))
			DrawPools(stats);
		if (UI::SectionHeader("Streaming.Loads", "Loads and uploads"))
		{
			DrawStreaming(stats);
			DrawUploads(stats);
		}
		if (m_Physics)
		{
			if (m_RevealPhysics)
				ImGui::SetNextItemOpen(true);
			m_RevealPhysics = false;
			if (UI::SectionHeader("Streaming.Physics", "Physics"))
				DrawPhysics(*m_Physics);
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

	void StreamingPanel::DrawWorld(EditorPanelContext& context, const WorldStreamer& streamer, bool editable)
	{
		// The cells in each state are the legend's counts; the header's summary has the world's name and its loaded cells.
		m_CellMap.Update(streamer, editable ? context.Context.GetWorld() : nullptr);
		const std::optional<CellMap::Request> request = m_CellMap.Draw(streamer, ImGui::GetFontSize() * CellMap::c_HeightInFontSizes, editable);
		if (!request)
			return;
		const Ref<WorldAsset> world = streamer.GetWorldAsset();
		const WorldLayer* layer = world ? world->GetData().FindLayer(request->Key.Layer) : nullptr;
		const nlohmann::json cell = { { "layer", layer ? nlohmann::json(layer->Name) : nlohmann::json(request->Key.Layer) }, { "x", request->Key.X }, { "z", request->Key.Z } };
		switch (request->Action)
		{
			case CellMap::Request::Kind::LoadAndPin:
				RunEditorCommand(context.Context, context.Commands, "world.loadCells", { { "cells", nlohmann::json::array({ cell }) } });
				break;
			case CellMap::Request::Kind::Unpin:
				RunEditorCommand(context.Context, context.Commands, "world.loadCells", { { "unpin", nlohmann::json::array({ cell }) } });
				break;
			case CellMap::Request::Kind::Show:
				if (m_RevealAsset)
					m_RevealAsset(request->Scene);
				break;
		}
	}

	void StreamingPanel::DrawPools(const AssetManagerStats& stats)
	{
		for (AssetMemoryPool pool : c_AssetMemoryPools)
		{
			const uint64_t used = GetPoolBytes(stats.Resident, pool);
			const uint64_t budget = stats.Budgets.GetPoolBudget(pool);
			ImGui::PushID(static_cast<int>(pool));
			DrawBar(GetMemoryPoolName(pool), GetFraction(used, budget), GetBudgetPressureColor(GetBudgetPressure(used, budget)), DescribePoolBar(used, budget));
			ImGui::PopID();
		}
	}

	void StreamingPanel::DrawStreaming(const AssetManagerStats& stats)
	{
		// The loads in flight in a neutral color, not by pressure: their limit throttles reads (a full bar while a burst waits
		// for room is the throttle at work, and a large load may take it past the limit), it is no memory budget to keep.
		const uint64_t limit = stats.Budgets.InFlightBytes;
		DrawBar("In flight", GetFraction(stats.InFlightBytes, limit), UI::GetThemeColors().Info,
			fmt::format("{:.1f} / {:.1f} MB \xC2\xB7 {} loads, {} reads", ToMB(stats.InFlightBytes), ToMB(limit), stats.InFlightLoads, stats.OutstandingReads));
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Bytes of loads being read and decoded against the limit that throttles new reads (asset.setBudget inFlightMB)");
		ImGui::Text("Queued: %u high \xC2\xB7 %u normal \xC2\xB7 %u low", stats.QueuedLoads[0], stats.QueuedLoads[1], stats.QueuedLoads[2]);
		ImGui::Text("Evictions %llu (%llu checked) \xC2\xB7 cancellations %llu \xC2\xB7 staging releases %llu", static_cast<unsigned long long>(stats.Evictions),
			static_cast<unsigned long long>(stats.EvictionChecks), static_cast<unsigned long long>(stats.Cancellations),
			static_cast<unsigned long long>(stats.StagingReleases));
	}

	void StreamingPanel::DrawPhysics(const PhysicsCapacityInfo& physics)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		if (physics.MaxBodies == 0)
		{
			ImGui::TextDisabled("The game has no physics bodies");
			return;
		}
		// Body slots against the capacity, colored like a memory budget: a world grows before it is full, so High says it
		// will grow soon and Over that it is at its growth limit with bodies waiting.
		const BudgetPressure pressure = physics.WaitingForSlot > 0 ? BudgetPressure::Over : GetBudgetPressure(physics.Bodies, physics.MaxBodies);
		DrawBar("Bodies", GetFraction(physics.Bodies, physics.MaxBodies), GetBudgetPressureColor(pressure),
			fmt::format("{} / {} ({:.0f}%)", physics.Bodies, physics.MaxBodies, 100.0 * static_cast<double>(physics.Bodies) / static_cast<double>(physics.MaxBodies)));
		UI::ItemProbe::Record("Streaming.PhysicsBodies");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Body slots in use (bodies of inactive entities and character controllers' presence bodies count) against the world's capacity, "
				"which grows before it is full (physics.stats)");
		if (physics.WaitingForSlot > 0)
		{
			ImGui::TextColored(colors.Error, "%u bodies without collision: the world is at its growth limit", physics.WaitingForSlot);
			UI::ItemProbe::Record("Streaming.PhysicsWaiting");
		}
		ImGui::Text("Last frame: +%u built \xC2\xB7 -%u destroyed", physics.AddedLastUpdate, physics.RemovedLastUpdate);
		ImGui::SameLine();
		ImGui::TextColored(physics.PendingOther > 0 ? colors.Warning : colors.TextSecondary, "\xC2\xB7 %u waiting for meshes or valid transforms", physics.PendingOther);
		if (physics.GrowthCount > 0)
			ImGui::Text("Grew %u %s, the last in %.1f ms", physics.GrowthCount, physics.GrowthCount == 1 ? "time" : "times", physics.LastGrowthMs);
		else
			ImGui::TextColored(colors.TextSecondary, "Never grew");
		ImGui::SameLine();
		ImGui::TextColored(colors.TextSecondary, "\xC2\xB7 reserves %.1f MB", ToMB(physics.ReservedBytes));
	}

	void StreamingPanel::DrawUploads(const AssetManagerStats& stats)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImGuiStyle& style = ImGui::GetStyle();
		const float budgetMB = static_cast<float>(ToMB(stats.Budgets.UploadBytesPerFrame));
		float peakMB = 0.0f;
		for (uint32_t sample = 0; sample < m_UploadSamples; sample++)
			peakMB = std::max(peakMB, m_UploadHistory[sample]);
		// The highest of the values and the budget, with room above it, so the budget line never lies on the edge.
		const float scaleMB = std::max(peakMB, budgetMB) * c_SparklineHeadroom;

		ImGui::TextColored(colors.TextSecondary, "Uploads per frame, last %u frames", c_UploadHistoryFrames);
		// Uploaded MB per frame, oldest at the left, against the frame's upload budget (the line).
		const ImVec2 min = ImGui::GetCursorScreenPos();
		const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize()), ImGui::GetFontSize() * c_SparklineHeightInFontSizes);
		ImGui::Dummy(size);
		const bool hovered = ImGui::IsItemHovered();
		const ImVec2 max(min.x + size.x, min.y + size.y);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(min, max, UI::ToColorU32(colors.Chrome), style.FrameRounding);
		const float columnWidth = size.x / static_cast<float>(c_UploadHistoryFrames);
		const uint32_t first = (m_UploadCursor + c_UploadHistoryFrames - m_UploadSamples) % c_UploadHistoryFrames;
		for (uint32_t sample = 0; sample < m_UploadSamples && scaleMB > 0.0f; sample++)
		{
			const float value = m_UploadHistory[(first + sample) % c_UploadHistoryFrames];
			if (value <= 0.0f)
				continue;
			const float x = min.x + columnWidth * static_cast<float>(c_UploadHistoryFrames - m_UploadSamples + sample);
			const float height = size.y * std::min(value / scaleMB, 1.0f);
			drawList->AddRectFilled(ImVec2(x, max.y - height), ImVec2(x + std::max(columnWidth - 1.0f, 1.0f), max.y), UI::ToColorU32(colors.Accent, 0.8f));
		}
		if (scaleMB > 0.0f)
		{
			// The budget line, labeled at its right end.
			const float budgetY = max.y - size.y * (budgetMB / scaleMB);
			drawList->AddLine(ImVec2(min.x, budgetY), ImVec2(max.x, budgetY), UI::ToColorU32(colors.TextSecondary, 0.6f));
			const std::string budgetLabel = fmt::format("{:.0f} MB budget", budgetMB);
			const ImVec2 labelSize = ImGui::CalcTextSize(budgetLabel.c_str());
			drawList->AddText(ImVec2(max.x - labelSize.x - style.FramePadding.x, std::min(budgetY + 1.0f, max.y - labelSize.y)), UI::ToColorU32(colors.TextSecondary),
				budgetLabel.c_str());
		}
		// After a load the line is empty: say so rather than show a box that looks unfinished.
		const std::string note = peakMB <= 0.0f ? fmt::format("No uploads in the last {} frames", c_UploadHistoryFrames) : std::string();
		if (!note.empty())
		{
			const ImVec2 noteSize = ImGui::CalcTextSize(note.c_str());
			drawList->AddText(ImVec2(min.x + (size.x - noteSize.x) * 0.5f, max.y - noteSize.y - style.FramePadding.y), UI::ToColorU32(colors.TextSecondary),
				note.c_str());
		}
		UI::ItemProbe::Record("Streaming.Uploads", true, 0, note);
		if (hovered)
			ImGui::SetTooltip("GPU megabytes uploaded per frame over the last %u frames; the line is the frame's upload budget", c_UploadHistoryFrames);

		ImGui::Text("Uploads %.2f MB last frame \xC2\xB7 at most %.2f MB of %.0f MB \xC2\xB7 finalize %.2f ms (at most %.2f of %.1f ms)", ToMB(stats.UploadedBytesLastFrame),
			ToMB(stats.UploadedBytesWindowMax), budgetMB, stats.FinalizeMsLastFrame, stats.FinalizeMsWindowMax, stats.Budgets.FinalizeMsPerFrame);
		ImGui::Text("Upload steps %.2f ms last frame \xC2\xB7 at most %.2f of %.1f ms", stats.UploadMsLastFrame, stats.UploadMsWindowMax,
			stats.Budgets.UploadMsPerFrame);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Asset table
	////////////////////////////////////////////////////////////////////////////////

	size_t StreamingPanel::GetTableRowCount() const
	{
		return m_Table->Built ? m_Table->Samples.size() : 0;
	}

	std::vector<std::string> StreamingPanel::GetShownLabels() const
	{
		std::vector<std::string> labels;
		if (!m_Table->Built)
			return labels;
		labels.reserve(m_Table->Rows.size());
		for (const uint32_t index : m_Table->Rows)
			labels.push_back(m_Table->SampleLabels[index]->Text);
		return labels;
	}

	bool StreamingPanel::IsTableShown(const Ref<AssetManagerBase>& manager) const
	{
		return m_Table->Built && IsSameManager(m_Table->Manager, manager);
	}

	void StreamingPanel::UpdateTable(const Ref<AssetManagerBase>& manager)
	{
		if (m_TableJob.IsValid() && m_TableJob.IsComplete())
			FinishTableBuild();
		if (m_TableJob.IsValid())
			return; // One build at a time: a change waits for the running build, then starts the next

		const double now = ImGui::GetTime();
		const bool due = m_LastTableRefresh < 0.0 || now - m_LastTableRefresh >= c_TableRefreshSeconds || now < m_LastTableRefresh;
		const bool changed = !IsTableShown(manager) || m_Table->SortColumn != m_SortColumn || m_Table->SortAscending != m_SortAscending
			|| m_Table->Filter != m_Filter;
		if (!due && !changed)
			return;
		StartTableBuild(manager);
		// Without job threads (JobSystem not running) the job ran in Submit: show its rows at once.
		if (m_TableJob.IsComplete())
			FinishTableBuild();
	}

	void StreamingPanel::StartTableBuild(const Ref<AssetManagerBase>& manager)
	{
		TableBuild& build = *m_NextTable;
		build.Manager = manager;
		build.Built = false;
		build.SortColumn = m_SortColumn;
		build.SortAscending = m_SortAscending;
		build.Filter = m_Filter;
		// The version before the names: an asset registered meanwhile changes it, so the next build reads them again.
		const uint64_t version = manager->GetMetadataVersion();
		if (m_Labels && IsSameManager(m_LabelsManager, manager) && version == m_LabelsVersion)
		{
			build.ReusedLabels = m_Labels;
			build.Metadata.reset();
		}
		else
		{
			build.ReusedLabels.reset();
			build.Metadata = manager->GetAllMetadata();
		}
		build.MetadataVersion = version;
		// Last uses count back from the frame the samples were read in, not the frame they are shown in.
		build.Frame = manager->GetFrameIndex();
		manager->GetResidencySamples(build.Samples);

		m_TableBuilds++;
		m_LastTableRefresh = ImGui::GetTime();
		TableBuild* target = &build;
		m_TableJob = JobSystem::Submit([target]() { BuildTable(*target); });
	}

	void StreamingPanel::FinishTableBuild()
	{
		m_TableJob = JobHandle();
		std::swap(m_Table, m_NextTable);
		m_Labels = m_Table->Labels;
		m_LabelsManager = m_Table->Manager;
		m_LabelsVersion = m_Table->MetadataVersion;
	}

	void StreamingPanel::WaitForTableBuild()
	{
		if (!m_TableJob.IsValid())
			return;
		m_TableJob.Wait();
		m_TableJob = JobHandle();
	}

	void StreamingPanel::BuildTable(TableBuild& build)
	{
		// Names: those of an earlier build, or read again from the metadata (dropping this buffer's previous names here, off
		// the main thread).
		if (build.ReusedLabels && !build.Metadata)
		{
			build.Labels = std::move(build.ReusedLabels);
		}
		else
		{
			Ref<LabelMap> labels = CreateRef<LabelMap>();
			if (build.Metadata)
			{
				labels->reserve(build.Metadata->size());
				for (const AssetMetadata& metadata : *build.Metadata)
				{
					Label& label = (*labels)[metadata.Handle];
					label.Text = GetAssetLabel(metadata);
					label.Lower = StringUtils::ToLower(label.Text);
					label.Revealable = !metadata.Path.empty() && !metadata.IsBuiltin();
				}
			}
			build.Labels = std::move(labels);
		}
		build.Metadata.reset();
		build.ReusedLabels.reset();

		const std::vector<AssetResidencySample>& samples = build.Samples;
		build.Unlisted.clear();
		build.SampleLabels.resize(samples.size());
		for (size_t index = 0; index < samples.size(); index++)
		{
			const AssetHandle handle = samples[index].Handle;
			if (const auto it = build.Labels->find(handle); it != build.Labels->end())
			{
				build.SampleLabels[index] = &it->second;
				continue;
			}
			// Registered after the names were read (references into an unordered_map stay valid as it grows).
			Label& label = build.Unlisted[handle];
			label.Text = handle.ToString();
			label.Lower = StringUtils::ToLower(label.Text);
			build.SampleLabels[index] = &label;
		}

		// Failures lead in every order (they are what the assets pill sends people here for), then the column's order, then
		// the handle, so equal rows keep their places between builds.
		build.Order.resize(samples.size());
		std::iota(build.Order.begin(), build.Order.end(), 0u);
		const bool ascending = build.SortAscending;
		const auto sortBy = [&](const auto& key)
		{
			std::sort(build.Order.begin(), build.Order.end(), [&](uint32_t first, uint32_t second)
			{
				const AssetResidencySample& a = samples[first];
				const AssetResidencySample& b = samples[second];
				const bool aFailed = a.State == AssetState::Failed;
				const bool bFailed = b.State == AssetState::Failed;
				if (aFailed != bFailed)
					return aFailed;
				const auto& x = key(first);
				const auto& y = key(second);
				if (x < y)
					return ascending;
				if (y < x)
					return !ascending;
				return a.Handle < b.Handle;
			});
		};
		const std::vector<const Label*>& labels = build.SampleLabels;
		switch (build.SortColumn)
		{
			case c_ColumnAsset:    sortBy([&](uint32_t index) -> const std::string& { return labels[index]->Lower; }); break;
			case c_ColumnType:     sortBy([&](uint32_t index) { return static_cast<int>(samples[index].Type); }); break;
			case c_ColumnState:    sortBy([&](uint32_t index) { return static_cast<int>(samples[index].State); }); break;
			case c_ColumnCpu:      sortBy([&](uint32_t index) { return samples[index].Usage.Cpu; }); break;
			case c_ColumnLastUsed: sortBy([&](uint32_t index) { return samples[index].LastRequestedFrame; }); break;
			case c_ColumnPins:     sortBy([&](uint32_t index) { return samples[index].PinCount; }); break;
			case c_ColumnMip:
				// The levels a texture lacks, then its resident level; assets that do not stream come last either way up.
				sortBy([&](uint32_t index)
				{
					const std::optional<AssetDetailLevels>& detail = samples[index].Detail;
					if (!detail)
						return std::pair<int64_t, int64_t>(build.SortAscending ? std::numeric_limits<int64_t>::max() : -1, 0);
					return std::pair<int64_t, int64_t>(static_cast<int64_t>(detail->Resident) - static_cast<int64_t>(std::min(detail->Wanted, detail->Resident)),
						detail->Resident);
				});
				break;
			case c_ColumnGpu:
			default:               sortBy([&](uint32_t index) { return samples[index].Usage.GetGpu(); }); break;
		}

		const std::string filter = StringUtils::ToLower(build.Filter);
		build.Rows.clear();
		for (const uint32_t index : build.Order)
		{
			if (filter.empty() || labels[index]->Lower.find(filter) != std::string::npos)
				build.Rows.push_back(index);
		}
		build.Built = true;
	}

	void StreamingPanel::UpdateColumnWidths()
	{
		// The fixed columns hold their header with its sort arrow and their widest usual value; the asset's name takes the
		// rest. Measured again only when the text size changes (the UI scale).
		if (ImGui::GetFontSize() != m_ColumnFontSize)
		{
			m_ColumnFontSize = ImGui::GetFontSize();
			const float sortArrow = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x;
			const auto widest = [](std::initializer_list<const char*> texts)
			{
				float width = 0.0f;
				for (const char* text : texts)
					width = std::max(width, ImGui::CalcTextSize(text).x);
				return width;
			};
			// Every asset type's label: the types are numbered from Scene on without gaps, so the first value without a name of
			// its own ends them (a type added later widens the column without a change here).
			float typeWidth = ImGui::CalcTextSize("Type").x;
			for (uint32_t value = static_cast<uint32_t>(AssetType::Scene); value <= std::numeric_limits<uint16_t>::max(); value++)
			{
				const AssetType type = static_cast<AssetType>(value);
				if (AssetTypeFromString(AssetTypeToString(type)) != type)
					break;
				typeWidth = std::max(typeWidth, ImGui::CalcTextSize(GetTypeLabel(type)).x);
			}
			m_ColumnWidths[c_ColumnType] = typeWidth + sortArrow;
			m_ColumnWidths[c_ColumnState] = widest({ "State", "Unloaded", "Loading", "Ready", "Failed" }) + sortArrow;
			m_ColumnWidths[c_ColumnGpu] = widest({ "GPU MB", "9999.99" }) + sortArrow;
			m_ColumnWidths[c_ColumnMip] = widest({ "Mip", FormatMipLevels(AssetDetailLevels { 12, 10, 13, 0 }).c_str() }) + sortArrow;
			m_ColumnWidths[c_ColumnCpu] = widest({ "CPU MB", "9999.99" }) + sortArrow;
			m_ColumnWidths[c_ColumnLastUsed] = widest({ "Last used", "99999 frames ago" }) + sortArrow;
			m_ColumnWidths[c_ColumnPins] = widest({ "Pins", "999" }) + sortArrow;
		}
	}

	std::string StreamingPanel::FormatMipLevels(const AssetDetailLevels& levels)
	{
		return fmt::format("{} / {}", levels.Resident, levels.Wanted);
	}

	float StreamingPanel::GetTableWidth(int optionalColumns) const
	{
		// Each column adds its cell padding and the border between it and the next.
		const float columnExtra = ImGui::GetStyle().CellPadding.x * 2.0f + 1.0f;
		float width = ImGui::GetFontSize() * c_MinAssetColumnInFontSizes + columnExtra;
		for (const int column : { c_ColumnType, c_ColumnState, c_ColumnGpu })
			width += m_ColumnWidths[column] + columnExtra;
		for (size_t index = c_OptionalColumns.size() - static_cast<size_t>(optionalColumns); index < c_OptionalColumns.size(); index++)
			width += m_ColumnWidths[c_OptionalColumns[index]] + columnExtra;
		return width;
	}

	void StreamingPanel::SetupTableColumns()
	{
		UpdateColumnWidths();
		ImGui::TableSetupColumn("Asset", ImGuiTableColumnFlags_WidthStretch, 1.0f, c_ColumnAsset);
		ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, m_ColumnWidths[c_ColumnType], c_ColumnType);
		ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, m_ColumnWidths[c_ColumnState], c_ColumnState);
		ImGui::TableSetupColumn("GPU MB", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending,
			m_ColumnWidths[c_ColumnGpu], c_ColumnGpu);
		ImGui::TableSetupColumn("Mip", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, m_ColumnWidths[c_ColumnMip], c_ColumnMip);
		ImGui::TableSetupColumn("CPU MB", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, m_ColumnWidths[c_ColumnCpu], c_ColumnCpu);
		ImGui::TableSetupColumn("Last used", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, m_ColumnWidths[c_ColumnLastUsed],
			c_ColumnLastUsed);
		ImGui::TableSetupColumn("Pins", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, m_ColumnWidths[c_ColumnPins], c_ColumnPins);
	}

	void StreamingPanel::DrawTable()
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		if (manager)
			UpdateTable(manager);

		UI::TextField("Streaming.Filter", "Filter", m_Filter, "Part of the asset's path or name");

		// The sort is the session's (and the default one each start): restoring a sorted column from imgui.ini moves it to
		// the front in the vendored ImGui.
		const ImGuiTableFlags flags = ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable
			| ImGuiTableFlags_Hideable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
		// As many of the optional columns as leave the asset's name its least width.
		int optionalColumns = static_cast<int>(c_OptionalColumns.size());
		const float tableWidth = ImGui::GetContentRegionAvail().x;
		while (optionalColumns > 0 && tableWidth < GetTableWidth(optionalColumns))
			optionalColumns--;
		if (!ImGui::BeginTable("Assets", c_ColumnCount, flags, ImVec2(0.0f, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFrameHeight() * 4.0f))))
			return;
		ImGui::TableSetupScrollFreeze(0, 1);
		SetupTableColumns();
		if (optionalColumns != m_OptionalColumnsShown)
		{
			const size_t firstShown = c_OptionalColumns.size() - static_cast<size_t>(optionalColumns);
			for (size_t index = 0; index < c_OptionalColumns.size(); index++)
				ImGui::TableSetColumnEnabled(c_OptionalColumns[index], index >= firstShown);
			m_OptionalColumnsShown = optionalColumns;
		}
		ImGui::TableHeadersRow();

		if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs(); specs && specs->SpecsDirty)
		{
			if (specs->SpecsCount > 0)
			{
				m_SortColumn = static_cast<int>(specs->Specs[0].ColumnUserID);
				m_SortAscending = specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
			}
			specs->SpecsDirty = false;
		}

		// The rows of the latest build of this manager's table (a new sort or filter shows once its build finished).
		const bool shown = IsTableShown(manager);
		const TableBuild& table = *m_Table;
		AssetHandle reveal = UUID::Null();
		ImGuiListClipper clipper;
		clipper.Begin(shown ? static_cast<int>(table.Rows.size()) : 0);
		while (clipper.Step())
		{
			for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; line++)
			{
				const uint32_t index = table.Rows[static_cast<size_t>(line)];
				const AssetResidencySample& sample = table.Samples[index];
				const Label& label = *table.SampleLabels[index];
				// Only assets of the project's files can be shown in the Content Browser.
				const bool revealable = label.Revealable && !sample.IsMemoryAsset;
				ImGui::TableNextRow();
				if (sample.State == AssetState::Failed)
					ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, UI::ToColorU32(GetAssetStateColor(AssetState::Failed), c_FailedRowAlpha));
				ImGui::TableSetColumnIndex(c_ColumnAsset);
				ImGui::PushID(line);
				if (ImGui::Selectable(label.Text.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap) && revealable)
					reveal = sample.Handle;
				if (line == 0)
					UI::ItemProbe::Record("Streaming.FirstAsset", revealable);
				if (ImGui::IsItemHovered())
				{
					// The type in full (the Type column may shorten it), then the handle.
					ImGui::SetTooltip("%s\n%s \xC2\xB7 %s\n%s", label.Text.c_str(), AssetTypeToString(sample.Type), sample.Handle.ToString().c_str(),
						revealable ? "Click to show it in the Content Browser" : "A built-in or memory asset: it has no file in the project");
				}
				ImGui::PopID();
				ImGui::TableSetColumnIndex(c_ColumnType);
				ImGui::TextUnformatted(GetTypeLabel(sample.Type));
				ImGui::TableSetColumnIndex(c_ColumnState);
				ImGui::TextColored(GetAssetStateColor(sample.State), "%s", AssetStateToString(sample.State));
				ImGui::TableSetColumnIndex(c_ColumnGpu);
				ImGui::Text("%.2f", ToMB(sample.Usage.GetGpu()));
				ImGui::TableSetColumnIndex(c_ColumnMip);
				if (sample.Detail)
				{
					// Resident level / wanted level: coarser than wanted in the warning color while its levels stream in.
					const std::string levels = FormatMipLevels(*sample.Detail);
					if (sample.Detail->Resident > sample.Detail->Wanted)
						ImGui::TextColored(colors.Warning, "%s", levels.c_str());
					else
						ImGui::TextUnformatted(levels.c_str());
					if (UI::IsTooltipDue())
					{
						ImGui::SetTooltip("Mip level %u resident of %u (0 is the full resolution), %u wanted by its views; %.2f MB resident",
							sample.Detail->Resident, sample.Detail->Count, sample.Detail->Wanted, ToMB(sample.Detail->ResidentBytes));
					}
				}
				ImGui::TableSetColumnIndex(c_ColumnCpu);
				ImGui::Text("%.2f", ToMB(sample.Usage.Cpu));
				ImGui::TableSetColumnIndex(c_ColumnLastUsed);
				if (sample.LastRequestedFrame == 0)
					ImGui::TextColored(colors.TextSecondary, "never");
				else if (sample.LastRequestedFrame >= table.Frame)
					ImGui::TextUnformatted("this frame");
				else
					ImGui::Text("%llu frames ago", static_cast<unsigned long long>(table.Frame - sample.LastRequestedFrame));
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
