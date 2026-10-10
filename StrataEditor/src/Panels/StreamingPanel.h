#pragma once

#include "UI/EditorPanelRegistry.h"

#include <Strata/Asset/AssetManager.h>

#include <imgui.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	// How full a memory pool is against its budget.
	enum class BudgetPressure : uint8_t
	{
		Unbudgeted = 0, // The pool has no budget
		Comfortable,    // Below c_HighBudgetPressure of the budget
		High,           // Up to the budget
		Over            // Beyond it (what is in use stays resident: streaming cannot evict it)
	};

	inline constexpr double c_HighBudgetPressure = 0.85;

	BudgetPressure GetBudgetPressure(uint64_t used, uint64_t budget);
	// The highest pressure among the pools that have a budget (Unbudgeted when none has).
	BudgetPressure GetWorstPoolPressure(const AssetManagerStats& stats);
	// Ochre (comfortable), Sulfur (high), Cinnabar (over); secondary text without a budget.
	const ImVec4& GetBudgetPressureColor(BudgetPressure pressure);

	// The asset streaming of the active asset manager, as asset.stats reports it: a budget bar per memory pool colored by
	// pressure, the loads in flight against their budget, the queue per priority, a sparkline of the bytes uploaded per frame,
	// evictions and cancellations, and a sortable table of the assets that are resident, loading, failed or pinned; clicking
	// a row reveals the asset in the Content Browser.
	//
	// The totals come from one AssetManagerBase::GetStats per frame, taken in OnUpdate (also while the panel is hidden, for
	// the status bar's assets pill) after the frame's commands ran, so they equal what asset.stats returned in that frame.
	// The table is read (GetResidencySamples, with names cached until GetMetadataVersion changes) at most every
	// c_TableRefreshSeconds while the panel is visible and drawn clipped, so it costs little with tens of thousands of assets.
	class StreamingPanel : public EditorPanel
	{
	public:
		static constexpr uint32_t c_UploadHistoryFrames = 120;
		static constexpr double c_TableRefreshSeconds = 0.25;

		void OnUpdate(EditorPanelContext& context) override;
		void OnImGuiRender(EditorPanelContext& context) override;

		// What a click on an asset does (the editor reveals it in the Content Browser).
		void SetRevealAssetHandler(std::function<void(AssetHandle)> handler) { m_RevealAsset = std::move(handler); }

		// The statistics of this frame's OnUpdate (nullopt without an active asset manager).
		const std::optional<AssetManagerStats>& GetStats() const { return m_Stats; }
		// Rows of the asset table as last read (unfiltered), for tests and tools.
		size_t GetTableRowCount() const { return m_Samples.size(); }
	private:
		// How an asset is named in the table: its path, else its name (memory assets).
		struct Label
		{
			std::string Text;
			std::string Lower; // For the filter and sorting by name
			bool Revealable = false; // A file of the project, which the Content Browser can show
		};

		void DrawPools(const AssetManagerStats& stats);
		void DrawStreaming(const AssetManagerStats& stats);
		void DrawUploads(const AssetManagerStats& stats);
		void DrawTable();
		void RefreshTable(const AssetManagerBase& manager);
		const Label& GetLabel(const AssetManagerBase& manager, AssetHandle handle);
		void SortRows();
	private:
		std::optional<AssetManagerStats> m_Stats;
		std::array<float, c_UploadHistoryFrames> m_UploadHistory = {}; // MB per frame, a ring
		uint32_t m_UploadCursor = 0;
		uint32_t m_UploadSamples = 0;
		uint64_t m_LastSampledFrame = 0;

		// The table: the residency of the listed assets (read without names, reusing the vectors), their labels, the order of
		// the rows and the rows the filter shows (indices into m_Samples). Names are cached per asset and read again only when
		// the manager's metadata changed (AssetManagerBase::GetMetadataVersion).
		std::vector<AssetResidencySample> m_Samples;
		std::vector<const Label*> m_SampleLabels;
		std::vector<uint32_t> m_Order;
		std::vector<uint32_t> m_Visible;
		std::unordered_map<AssetHandle, Label> m_Labels;
		const AssetManagerBase* m_LabelManager = nullptr; // Only compared: the manager the labels belong to
		uint64_t m_LabelVersion = 0;
		uint64_t m_SampleFrame = 0; // The manager's frame when m_Samples were read
		double m_LastTableRefresh = -1.0;
		bool m_RowsSorted = false;
		int m_SortColumn = 3; // The column's user id (StreamingPanel.cpp); 3: GPU memory
		bool m_SortAscending = false;
		std::string m_Filter;
		std::string m_VisibleFilter; // The filter m_Visible was built with
		bool m_VisibleDirty = true;  // The rows changed since m_Visible was built
		std::function<void(AssetHandle)> m_RevealAsset;
	};

}
