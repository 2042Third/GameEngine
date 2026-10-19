#include "stpch.h"
#include "Strata/GameUI/GameUIHitTest.h"

#include "Strata/Core/Profiling.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

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
			grid.WorldSpace = view.WorldSpace;
			// Where a world-space canvas is follows its entity (and so may move without changing the canvas's layout).
			if (view.WorldSpace)
			{
				const glm::mat4 canvasToWorld = ui.GetCanvasWorldMatrix(view);
				changed = changed || canvasToWorld != grid.CanvasToWorld;
				grid.CanvasToWorld = canvasToWorld;
			}
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
			// What the masks above it leave of it: the pointer hits only what is drawn.
			const UIBox box = ui.GetVisibleBox(view, element);
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

	std::optional<UIHitResult> GameUIHitTester::HitTestGrid(uint32_t viewIndex, const glm::vec2& point) const
	{
		const CanvasGrid& grid = m_Grids[viewIndex];
		// Points beyond the canvas fall in its border cells, which hold the rects that reach out of it.
		const glm::uvec2 cellPosition = glm::uvec2(glm::clamp(glm::floor(point / grid.CellSize), glm::vec2(0.0f), glm::vec2(grid.Cells) - 1.0f));
		const uint32_t cell = cellPosition.y * grid.Cells.x + cellPosition.x;
		// The last drawn first: the cell's targets are in draw order.
		for (uint32_t index = grid.CellStarts[cell + 1]; index-- > grid.CellStarts[cell];)
		{
			m_LastCandidates++;
			const Target& target = grid.Targets[grid.CellTargets[index]];
			if (target.Box.Contains(point))
				return UIHitResult { target.Entity, viewIndex, target.Element };
		}
		return std::nullopt;
	}

	void GameUIHitTester::FindWorldCrossings(const UIPointerRay& ray, bool anyCanvas) const
	{
		m_Crossings.clear();
		for (uint32_t viewIndex = 0; viewIndex < m_Grids.size(); viewIndex++)
		{
			const CanvasGrid& grid = m_Grids[viewIndex];
			if (!grid.WorldSpace || (!grid.ReceivesInput && !anyCanvas))
				continue;
			if (const std::optional<std::pair<glm::vec2, float>> crossing = GameUIPointer::IntersectCanvas(grid.CanvasToWorld, ray))
				m_Crossings.push_back(WorldCrossing { crossing->second, viewIndex, crossing->first });
		}
		std::stable_sort(m_Crossings.begin(), m_Crossings.end(), [](const WorldCrossing& a, const WorldCrossing& b) { return a.Distance < b.Distance; });
	}

	std::optional<UIHitResult> GameUIHitTester::HitTest(const glm::vec2& pixel, bool anyCanvas, const UIPointerRay* ray) const
	{
		m_LastCandidates = 0;
		// From the topmost screen-space canvas down.
		for (size_t viewIndex = m_Grids.size(); viewIndex-- > 0;)
		{
			const CanvasGrid& grid = m_Grids[viewIndex];
			if (grid.WorldSpace || (!grid.ReceivesInput && !anyCanvas) || grid.Targets.empty())
				continue;
			if (const std::optional<UIHitResult> hit = HitTestGrid(static_cast<uint32_t>(viewIndex), pixel / grid.Scale))
				return hit;
		}
		// Then the canvases in the scene, the nearest first.
		if (!ray)
			return std::nullopt;
		FindWorldCrossings(*ray, anyCanvas);
		for (const WorldCrossing& crossing : m_Crossings)
		{
			if (m_Grids[crossing.View].Targets.empty())
				continue;
			if (const std::optional<UIHitResult> hit = HitTestGrid(crossing.View, crossing.Point))
				return hit;
		}
		return std::nullopt;
	}

	void GameUIHitTester::HitTestAll(GameUIScene& ui, std::span<const UICanvasView> views, const glm::vec2& pixel, std::vector<UIHitResult>& outHits,
		const UIPointerRay* ray) const
	{
		outHits.clear();
		auto collect = [&](size_t viewIndex, const glm::vec2& point)
		{
			const UICanvasView& view = views[viewIndex];
			for (uint32_t element = ui.GetElementCount(view); element-- > 0;)
			{
				if (ui.IsElementVisible(view, element) && ui.GetVisibleBox(view, element).Contains(point))
					outHits.push_back(UIHitResult { ui.GetElementEntity(view, element), static_cast<uint32_t>(viewIndex), element });
			}
		};
		for (size_t viewIndex = views.size(); viewIndex-- > 0;)
		{
			if (!views[viewIndex].WorldSpace)
				collect(viewIndex, pixel / views[viewIndex].Scale);
		}
		if (!ray || m_Grids.size() != views.size())
			return;
		FindWorldCrossings(*ray, true);
		for (const WorldCrossing& crossing : m_Crossings)
			collect(crossing.View, crossing.Point);
	}

	namespace GameUIPointer
	{

		std::optional<glm::mat4> GetGameViewProjection(Scene& scene, const UIScreen& screen)
		{
			const Entity camera = scene.GetPrimaryCameraEntity();
			if (!camera || !(screen.Size.x > 0.0f && screen.Size.y > 0.0f))
				return std::nullopt;
			const SceneCamera view = SceneCamera::FromEntity(scene, camera, screen.Size.x / screen.Size.y);
			return view.Projection * view.View;
		}

		std::optional<UIPointerRay> MakeRay(const glm::mat4& viewProjection, const glm::vec2& screenSize, const glm::vec2& pixel)
		{
			if (!(screenSize.x > 0.0f && screenSize.y > 0.0f))
				return std::nullopt;
			// Pixel centers to normalized device coordinates (+Y up); reversed-Z: depth 1 is near, 0 far.
			const glm::vec2 ndc((pixel.x / screenSize.x) * 2.0f - 1.0f, 1.0f - (pixel.y / screenSize.y) * 2.0f);
			const glm::mat4 inverse = glm::inverse(viewProjection);
			glm::vec4 nearPoint = inverse * glm::vec4(ndc, 1.0f, 1.0f);
			glm::vec4 farPoint = inverse * glm::vec4(ndc, 0.5f, 1.0f); // Halfway: finite also for infinite far planes
			if (!(std::abs(nearPoint.w) > 1e-12f) || !(std::abs(farPoint.w) > 1e-12f))
				return std::nullopt;
			const glm::vec3 from = glm::vec3(nearPoint) / nearPoint.w;
			const glm::vec3 to = glm::vec3(farPoint) / farPoint.w;
			const glm::vec3 direction = to - from;
			const float length = glm::length(direction);
			if (!(length > 1e-12f) || !std::isfinite(length))
				return std::nullopt;
			return UIPointerRay { from, direction / length };
		}

		std::optional<std::pair<glm::vec2, float>> IntersectCanvas(const glm::mat4& canvasToWorld, const UIPointerRay& ray)
		{
			// In the canvas's own space the plane is z = 0, x and y its units.
			const glm::mat4 worldToCanvas = glm::inverse(canvasToWorld);
			const glm::vec3 origin = glm::vec3(worldToCanvas * glm::vec4(ray.Origin, 1.0f));
			const glm::vec3 direction = glm::vec3(worldToCanvas * glm::vec4(ray.Direction, 0.0f));
			if (!(std::abs(direction.z) > 1e-12f) || !std::isfinite(origin.z))
				return std::nullopt;
			const float t = -origin.z / direction.z;
			if (!(t >= 0.0f))
				return std::nullopt;
			const glm::vec3 point = origin + direction * t;
			// The distance along the world ray (direction keeps its length through the inverse's scale).
			const float distance = glm::length(glm::vec3(canvasToWorld * glm::vec4(point, 1.0f)) - ray.Origin);
			return std::pair<glm::vec2, float>(glm::vec2(point), distance);
		}

		std::optional<glm::vec2> ProjectCanvasPoint(const glm::mat4& viewProjection, const glm::mat4& canvasToWorld, const glm::vec2& screenSize,
			const glm::vec2& point)
		{
			const glm::vec4 clip = viewProjection * canvasToWorld * glm::vec4(point, 0.0f, 1.0f);
			if (!(clip.w > 1e-6f))
				return std::nullopt;
			const glm::vec2 ndc = glm::vec2(clip) / clip.w;
			return glm::vec2((ndc.x * 0.5f + 0.5f) * screenSize.x, (0.5f - ndc.y * 0.5f) * screenSize.y);
		}

	}

}
