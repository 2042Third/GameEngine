#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/World/WorldGrid.h>
#include <Strata/World/WorldStreamer.h>

#include "UI/Widgets.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class EditorWorld;
	class WorldAsset;

	// The cell map of the Streaming panel's World section: the cells of a world's layers as squares on the ground plane
	// (+X right, -Z up, the way a top view looks down), one layer at a time or all overlaid (a chip row picks), each colored
	// by its state with the theme's meanings: unloaded (the surface), wanted (secondary text, faint), loading (Sulfur),
	// loaded (Ochre), unloading (secondary text), failed (Cinnabar); unsaved cells are outlined in the warning color and
	// pinned ones carry a pin. The streaming sources are dots with their radius circles and facing. Drag pans, the wheel
	// zooms around the mouse, a double click frames the sources; hovering names a cell (key, state, entities, bodies from
	// the cook's index), a click selects it and its context menu offers Load and pin, Unpin and Show in Content Browser.
	//
	// Its cells are copied from the streamer only when the streamer's StateVersion, its World asset object or the editor's
	// dirty marks changed (Update), and only the cells in view are drawn, as draw-list rectangles: no widget per cell. The
	// panel calls it only while the section is shown, so a closed panel costs nothing.
	class CellMap
	{
	public:
		// The map's height in font sizes, at most and at least (the space left in the panel decides between them).
		static constexpr float c_HeightInFontSizes = 18.0f;
		static constexpr float c_MinHeightInFontSizes = 4.0f;

		struct Cell
		{
			WorldCellKey Key;
			AssetHandle Scene = UUID::Null();
			size_t LayerIndex = 0; // In WorldData::Layers
			WorldCellStatus Status = WorldCellStatus::Unloaded;
			bool Wanted = false;
			bool Pinned = false;
			bool Dirty = false;
			uint64_t Entities = 0;
			std::optional<uint64_t> Bodies; // From the World asset's index, when cooked
			std::string Error;
		};

		// What the context menu asked for (the panel runs world.loadCells or reveals the cell's scene).
		struct Request
		{
			enum class Kind : uint8_t
			{
				LoadAndPin = 0,
				Unpin,
				Show
			};

			Kind Action = Kind::LoadAndPin;
			WorldCellKey Key;
			AssetHandle Scene = UUID::Null();
		};

		// Brings the cells up to date with `streamer`, and with `world`'s dirty marks when it is the editor's (null for a
		// game's streamer). Returns true when they were copied again.
		bool Update(const WorldStreamer& streamer, const EditorWorld* world);
		// Draws the layer chips, the map (the content region's width, at most `height` high) and its legend. With `editable` (the
		// editor's world, which world.loadCells pins in) the context menu offers loading and unpinning; otherwise only
		// showing the cell's scene. Returns what the context menu asked for this frame.
		std::optional<Request> Draw(const WorldStreamer& streamer, float height, bool editable);

		const std::vector<Cell>& GetCells() const { return m_Cells; }
		// How often the cells were copied (tests: a hidden map copies nothing).
		uint64_t GetRebuildCount() const { return m_Rebuilds; }
		// The cells the last Draw drew (those in view).
		size_t GetDrawnCellCount() const { return m_DrawnCells; }
		std::optional<WorldCellKey> GetSelectedCell() const { return m_Selected; }
		// The layer shown: an index into WorldData::Layers, or nullopt for every layer overlaid.
		std::optional<size_t> GetShownLayer() const { return m_ShownLayer; }
		void SetShownLayer(std::optional<size_t> layer) { m_ShownLayer = layer; }
		// The view: the ground point at the map's center and pixels per world unit (set by the first Draw: the sources framed).
		const glm::dvec2& GetCenter() const { return m_Center; }
		double GetScale() const { return m_Scale; }
		void SetView(const glm::dvec2& center, double scale);
	private:
		// The cell under a point of the ground plane: of the shown layer, or the finest layer that has one there.
		const Cell* FindCellAt(const WorldAsset& world, const glm::dvec2& point) const;
		const Cell* FindCell(const WorldCellKey& key) const;
		// Frames the sources (or, without any, the cells) in a map of `size` pixels.
		void FrameSources(const WorldStreamer& streamer, const WorldAsset& world, const ImVec2& size);
		void DrawChips(const WorldAsset& world);
		void DrawCells(ImDrawList* drawList, const WorldAsset& world, const ImVec2& min, const ImVec2& max, size_t layerIndex);
		void DrawSources(ImDrawList* drawList, const WorldStreamer& streamer, const WorldAsset& world, const ImVec2& min, const ImVec2& max) const;
		void DrawTooltip(const Cell& cell, const WorldAsset& world) const;
		std::optional<Request> DrawContextMenu(bool editable);
		void DrawLegend(const ImVec2& bottomLeft);
		ImVec2 ToScreen(const glm::dvec2& point, const ImVec2& min, const ImVec2& max) const;
		glm::dvec2 ToGround(const ImVec2& point, const ImVec2& min, const ImVec2& max) const;
	private:
		std::vector<Cell> m_Cells;
		std::unordered_map<uint64_t, size_t> m_CellIndex; // WorldCellKey::Pack -> index into m_Cells
		std::vector<WorldCellState> m_States;             // Scratch of Update
		std::array<uint32_t, 6> m_StateCounts = {};        // Per legend line, from the last copy
		uint64_t m_Rebuilds = 0;
		uint64_t m_StateVersion = 0;
		uint64_t m_DirtyVersion = 0;
		const EditorWorld* m_DirtyWorld = nullptr;
		std::weak_ptr<WorldAsset> m_WorldAsset; // The World asset object the cells were copied with
		const WorldStreamer* m_Streamer = nullptr;  // The streamer they were copied from (the editor's, then a game's)
		AssetHandle m_World = UUID::Null();         // Its world: another one frames its sources again
		bool m_HasCells = false;
		std::vector<UI::LegendLine> m_Legend;       // Built with the cells

		std::optional<size_t> m_ShownLayer;
		glm::dvec2 m_Center { 0.0 };
		double m_Scale = 0.0; // 0 until the first Draw frames the sources
		std::optional<WorldCellKey> m_Selected;
		std::optional<WorldCellKey> m_ContextCell; // The cell the open context menu is for
		bool m_ShowLegend = true;
		size_t m_DrawnCells = 0;
	};

}
