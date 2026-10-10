#pragma once

#include "UI/EditorPanelRegistry.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/JobSystem.h>

#include <imgui.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
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
	// The pool with a budget that is closest to it or furthest beyond it: the highest pressure, then (close to or over a
	// budget) the largest share of its budget in use; GPU textures, then GPU buffers, then CPU among equals and while every
	// pool is comfortable. Nothing when no pool has a budget. The assets pill shows this pool's figure in this pool's
	// color, so the two always agree.
	std::optional<AssetMemoryPool> GetMostPressuredPool(const AssetManagerStats& stats);
	// The pressure of GetMostPressuredPool (Unbudgeted when no pool has a budget).
	BudgetPressure GetWorstPoolPressure(const AssetManagerStats& stats);
	// Ochre (comfortable), Sulfur (high), Cinnabar (over); secondary text without a budget.
	const ImVec4& GetBudgetPressureColor(BudgetPressure pressure);
	// The color of an asset's state wherever the editor shows one (the Streaming panel, the viewport's streaming view):
	// Sulfur while loading, Cinnabar when failed, the text color when ready.
	const ImVec4& GetAssetStateColor(AssetState state);
	// The pool's name in the Streaming panel ("GPU textures", "GPU buffers", "CPU"), and the short one of the assets pill
	// ("textures", "buffers", "CPU").
	const char* GetMemoryPoolName(AssetMemoryPool pool);
	const char* GetMemoryPoolShortName(AssetMemoryPool pool);

	// The asset streaming of the active asset manager, as asset.stats reports it: a budget bar per memory pool colored by
	// pressure, the loads in flight against their limit, the queue per priority, a sparkline of the bytes uploaded per frame,
	// evictions and cancellations, and a sortable table of the assets that are resident, loading, failed or pinned (failed
	// ones first in any order); clicking a row reveals the asset in the Content Browser.
	//
	// The totals come from one AssetManagerBase::GetStats per frame, taken in OnUpdate (also while the panel is hidden, for
	// the status bar's assets pill) after the frame's commands ran, so they equal what asset.stats returned in that frame.
	// The table is read at most every c_TableRefreshSeconds while the panel is visible (and when its sort or filter
	// changes): the main thread only copies the residency (GetResidencySamples) and, when GetMetadataVersion changed, the
	// metadata; a job names, sorts and filters the rows (BuildTable), and the table shows them once it finished. So no frame
	// sorts tens of thousands of rows, and the rows are drawn clipped.
	class StreamingPanel : public EditorPanel
	{
	public:
		static constexpr uint32_t c_UploadHistoryFrames = 120;
		static constexpr double c_TableRefreshSeconds = 0.25;

		StreamingPanel();
		~StreamingPanel() override;

		StreamingPanel(const StreamingPanel&) = delete;
		StreamingPanel& operator=(const StreamingPanel&) = delete;

		void OnUpdate(EditorPanelContext& context) override;
		void OnImGuiRender(EditorPanelContext& context) override;
		void OnDetach(EditorPanelContext& context) override;

		// What a click on an asset does (the editor reveals it in the Content Browser).
		void SetRevealAssetHandler(std::function<void(AssetHandle)> handler) { m_RevealAsset = std::move(handler); }

		// The statistics of this frame's OnUpdate (nullopt without an active asset manager).
		const std::optional<AssetManagerStats>& GetStats() const { return m_Stats; }
		// The asset table as last built, for tests and tools: its rows (every listed asset, unfiltered), the labels of the
		// rows it shows (filtered, in their order), whether a build is running and how many builds started.
		size_t GetTableRowCount() const;
		std::vector<std::string> GetShownLabels() const;
		bool IsBuildingTable() const { return m_TableJob.IsValid(); }
		uint64_t GetTableBuildCount() const { return m_TableBuilds; }

		// How the table names an asset: its path; a sub-asset adds its key to its parent's path (and its name when that
		// differs), so the meshes and materials of one model can be told apart; assets without a file go by their name.
		static std::string GetAssetLabel(const AssetMetadata& metadata);
	private:
		struct Label
		{
			std::string Text;
			std::string Lower; // For the filter and sorting by name
			bool Revealable = false; // A file of the project, which the Content Browser can show
		};
		using LabelMap = std::unordered_map<AssetHandle, Label>;

		// One build of the asset table. The main thread fills the inputs (StartTableBuild); a job turns them into rows
		// (BuildTable) and touches nothing else; the main thread shows the rows once the job finished (FinishTableBuild).
		struct TableBuild
		{
			// Inputs, read on the main thread.
			std::weak_ptr<AssetManagerBase> Manager; // Only compared: the manager the rows belong to
			uint64_t Frame = 0;                      // The manager's frame when the samples were read
			std::vector<AssetResidencySample> Samples;
			std::optional<std::vector<AssetMetadata>> Metadata; // The names to read again, when they changed
			Ref<const LabelMap> ReusedLabels;                    // The names of an earlier build otherwise
			uint64_t MetadataVersion = 0;                        // Of the names
			int SortColumn = 0;
			bool SortAscending = false;
			std::string Filter;
			// Outputs of the job.
			bool Built = false;
			Ref<const LabelMap> Labels;
			LabelMap Unlisted; // Assets registered after the names were read, named by their handle until the next build
			std::vector<const Label*> SampleLabels;
			std::vector<uint32_t> Order; // Every sample, sorted
			std::vector<uint32_t> Rows;  // The samples the filter shows, in order
		};

		void DrawPools(const AssetManagerStats& stats);
		void DrawStreaming(const AssetManagerStats& stats);
		void DrawUploads(const AssetManagerStats& stats);
		void DrawTable();
		void SetupTableColumns();
		// Takes a finished build and starts the next one when the table is due for a refresh or its sort or filter changed.
		void UpdateTable(const Ref<AssetManagerBase>& manager);
		void StartTableBuild(const Ref<AssetManagerBase>& manager);
		void FinishTableBuild();
		void WaitForTableBuild();
		bool IsTableShown(const Ref<AssetManagerBase>& manager) const;
		static void BuildTable(TableBuild& build);
	private:
		std::optional<AssetManagerStats> m_Stats;
		std::array<float, c_UploadHistoryFrames> m_UploadHistory = {}; // MB per frame, a ring
		uint32_t m_UploadCursor = 0;
		uint32_t m_UploadSamples = 0;
		uint64_t m_LastSampledFrame = 0;

		// The table shown and the one being built (between builds, its buffers wait for the next one, so builds reuse their
		// capacity). The job only ever touches m_NextTable, and only while m_TableJob runs.
		Scope<TableBuild> m_Table;
		Scope<TableBuild> m_NextTable;
		JobHandle m_TableJob;
		uint64_t m_TableBuilds = 0;
		double m_LastTableRefresh = -1.0;
		// The names of the latest build, reused while the manager's metadata stays the same.
		Ref<const LabelMap> m_Labels;
		std::weak_ptr<AssetManagerBase> m_LabelsManager;
		uint64_t m_LabelsVersion = 0;

		int m_SortColumn = 3; // The column's user id (StreamingPanel.cpp); 3: GPU memory
		bool m_SortAscending = false;
		std::string m_Filter;
		// Widths of the fixed columns at m_ColumnFontSize (they hold their headers, sort arrows and values).
		std::array<float, 7> m_ColumnWidths = {};
		float m_ColumnFontSize = 0.0f;
		std::function<void(AssetHandle)> m_RevealAsset;
	};

}
