#include "stpch.h"
#include "Strata/GameUI/GameUIHitTest.h"

#include "Strata/Core/Profiling.h"

#include <algorithm>
#include <cmath>

namespace Strata
{

	namespace
	{

		// Grids aim at about this many targets per cell, at most c_MaxCellsPerAxis cells across.
		constexpr float c_TargetsPerCell = 2.0f;
		constexpr uint32_t c_MaxCellsPerAxis = 64;

	}

	bool GameUIHitTester::Update(GameUIScene& ui, std::span<const UICanvasView> views)
	{
		bool changed = m_Grids.size() != views.size();
		m_Grids.resize(views.size());
		for (size_t index = 0; index < views.size(); index++)
		{
			const UICanvasView& view = views[index];
			CanvasGrid& grid = m_Grids[index];
			grid.Scale = view.Scale;
			grid.ReceivesInput = view.ReceivesInput;
			if (grid.LayoutId == ui.GetLayoutId(view) && grid.HitStamp == ui.GetHitStamp(view) && grid.CanvasIndex == view.CanvasIndex)
				continue;
			Build(ui, view, grid);
			changed = true;
		}
		return changed;
	}

	void GameUIHitTester::Build(GameUIScene& ui, const UICanvasView& view, CanvasGrid& grid)
	{
		ST_PROFILE_FUNCTION();
		grid.LayoutId = ui.GetLayoutId(view);
		grid.HitStamp = ui.GetHitStamp(view);
		grid.CanvasIndex = view.CanvasIndex;
		grid.Targets.clear();
		const uint32_t count = ui.GetElementCount(view);
		for (uint32_t element = 0; element < count; element++)
		{
			if (!ui.IsElementVisible(view, element) || !ui.IsRaycastTarget(view, element))
				continue;
			const UIBox box = ui.GetElementBox(view, element);
			if (box.Size.x > 0.0f && box.Size.y > 0.0f)
				grid.Targets.push_back(Target { element, ui.GetElementEntity(view, element), box });
		}

		// Cells over the canvas's size in units, about c_TargetsPerCell targets each where they are spread evenly.
		const glm::vec2 extent = glm::max(view.UnitSize, glm::vec2(1.0f));
		const float cellsPerAxis = std::ceil(std::sqrt(static_cast<float>(grid.Targets.size()) / c_TargetsPerCell));
		const uint32_t cells = std::clamp(static_cast<uint32_t>(cellsPerAxis), 1u, c_MaxCellsPerAxis);
		grid.Cells = glm::uvec2(cells);
		grid.CellSize = extent / glm::vec2(grid.Cells);

		// Counting sort into the cells each target's rect overlaps (rects beyond the canvas go to its border cells).
		auto cellRange = [&](const UIBox& box, glm::uvec2& outMin, glm::uvec2& outMax)
		{
			const glm::vec2 min = glm::floor(box.Min / grid.CellSize);
			const glm::vec2 max = glm::floor((box.GetMax() - glm::vec2(1e-4f)) / grid.CellSize);
			const glm::vec2 last = glm::vec2(grid.Cells) - 1.0f;
			outMin = glm::uvec2(glm::clamp(min, glm::vec2(0.0f), last));
			outMax = glm::uvec2(glm::clamp(max, glm::vec2(0.0f), last));
		};
		const uint32_t cellCount = grid.Cells.x * grid.Cells.y;
		grid.CellStarts.assign(cellCount + 1, 0);
		for (const Target& target : grid.Targets)
		{
			glm::uvec2 min;
			glm::uvec2 max;
			cellRange(target.Box, min, max);
			for (uint32_t y = min.y; y <= max.y; y++)
			{
				for (uint32_t x = min.x; x <= max.x; x++)
					grid.CellStarts[y * grid.Cells.x + x + 1]++;
			}
		}
		for (uint32_t cell = 0; cell < cellCount; cell++)
			grid.CellStarts[cell + 1] += grid.CellStarts[cell];
		grid.CellTargets.assign(grid.CellStarts[cellCount], 0);
		std::vector<uint32_t> fill(grid.CellStarts.begin(), grid.CellStarts.end() - 1);
		for (uint32_t index = 0; index < grid.Targets.size(); index++)
		{
			glm::uvec2 min;
			glm::uvec2 max;
			cellRange(grid.Targets[index].Box, min, max);
			for (uint32_t y = min.y; y <= max.y; y++)
			{
				for (uint32_t x = min.x; x <= max.x; x++)
					grid.CellTargets[fill[y * grid.Cells.x + x]++] = index;
			}
		}
	}

	std::optional<UIHitResult> GameUIHitTester::HitTest(const glm::vec2& pixel, bool anyCanvas) const
	{
		m_LastCandidates = 0;
		// From the topmost canvas down.
		for (size_t viewIndex = m_Grids.size(); viewIndex-- > 0;)
		{
			const CanvasGrid& grid = m_Grids[viewIndex];
			if ((!grid.ReceivesInput && !anyCanvas) || grid.Targets.empty())
				continue;
			const glm::vec2 point = pixel / grid.Scale;
			// Points beyond the canvas fall in its border cells, which hold the rects that reach out of it.
			const glm::uvec2 cellPosition = glm::uvec2(glm::clamp(glm::floor(point / grid.CellSize), glm::vec2(0.0f), glm::vec2(grid.Cells) - 1.0f));
			const uint32_t cell = cellPosition.y * grid.Cells.x + cellPosition.x;
			// The last drawn first: the cell's targets are in draw order.
			for (uint32_t index = grid.CellStarts[cell + 1]; index-- > grid.CellStarts[cell];)
			{
				m_LastCandidates++;
				const Target& target = grid.Targets[grid.CellTargets[index]];
				if (target.Box.Contains(point))
					return UIHitResult { target.Entity, static_cast<uint32_t>(viewIndex), target.Element };
			}
		}
		return std::nullopt;
	}

	void GameUIHitTester::HitTestAll(GameUIScene& ui, std::span<const UICanvasView> views, const glm::vec2& pixel, std::vector<UIHitResult>& outHits) const
	{
		outHits.clear();
		for (size_t viewIndex = views.size(); viewIndex-- > 0;)
		{
			const UICanvasView& view = views[viewIndex];
			const glm::vec2 point = pixel / view.Scale;
			for (uint32_t element = ui.GetElementCount(view); element-- > 0;)
			{
				if (ui.IsElementVisible(view, element) && ui.GetElementBox(view, element).Contains(point))
					outHits.push_back(UIHitResult { ui.GetElementEntity(view, element), static_cast<uint32_t>(viewIndex), element });
			}
		}
	}

}
