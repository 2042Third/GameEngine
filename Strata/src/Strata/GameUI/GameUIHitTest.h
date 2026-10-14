#pragma once

#include "Strata/GameUI/GameUIScene.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Strata
{

	// An element under a point: its entity, the canvas view (index into the views given to the tester) and the element.
	struct UIHitResult
	{
		entt::entity Entity = entt::null;
		uint32_t View = 0;
		uint32_t Element = 0;
	};

	// Finds the game UI element under a point of a screen: the topmost raycast target (an element with a UIImage or UIText
	// that is a raycast target, or a UISelectable) that is visible and whose rect contains the point (half-open), in canvases
	// that receive input, searched from the highest sort order down and within a canvas from the last element drawn. Each
	// canvas keeps a uniform grid of its targets' rects, rebuilt when what hit tests see changed (GameUIScene::GetHitStamp),
	// so a query looks at the targets of one cell. Main thread only.
	class GameUIHitTester
	{
	public:
		// Brings the grids up to date for the views of one EnsureLayout (in draw order). Returns whether anything changed.
		bool Update(GameUIScene& ui, std::span<const UICanvasView> views);
		// The topmost raycast target at a point in screen pixels, among canvases that receive input (`anyCanvas`: all of them).
		std::optional<UIHitResult> HitTest(const glm::vec2& pixel, bool anyCanvas = false) const;
		// Every visible element whose rect contains the point, raycast target or not, topmost first.
		void HitTestAll(GameUIScene& ui, std::span<const UICanvasView> views, const glm::vec2& pixel, std::vector<UIHitResult>& outHits) const;
		// The targets the latest HitTest compared with the point (at most a cell's).
		uint32_t GetLastCandidateCount() const { return m_LastCandidates; }
	private:
		struct Target
		{
			uint32_t Element = 0;
			entt::entity Entity = entt::null;
			UIBox Box;
		};

		struct CanvasGrid
		{
			uint64_t LayoutId = 0;
			uint64_t HitStamp = 0;
			uint32_t CanvasIndex = ~0u;
			float Scale = 1.0f;
			bool ReceivesInput = true;
			glm::uvec2 Cells = glm::uvec2(1);
			glm::vec2 CellSize = glm::vec2(1.0f); // Canvas units
			std::vector<Target> Targets;          // In element (draw) order
			std::vector<uint32_t> CellStarts;     // Per cell, into CellTargets (one more entry than cells)
			std::vector<uint32_t> CellTargets;    // Target indices, ascending within each cell
		};

		void Build(GameUIScene& ui, const UICanvasView& view, CanvasGrid& grid);
	private:
		std::vector<CanvasGrid> m_Grids; // By view, in draw order
		mutable uint32_t m_LastCandidates = 0;
	};

}
