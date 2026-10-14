#pragma once

#include "Strata/GameUI/GameUIScene.h"

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Strata
{

	class Scene;

	// An element under a point: its entity, the canvas view (index into the views given to the tester) and the element.
	struct UIHitResult
	{
		entt::entity Entity = entt::null;
		uint32_t View = 0;
		uint32_t Element = 0;
	};

	// The pointer's ray into the scene, from a camera through a pixel of the screen it renders: what world-space canvases are
	// hit by.
	struct UIPointerRay
	{
		glm::vec3 Origin = glm::vec3(0.0f);
		glm::vec3 Direction = glm::vec3(0.0f, 0.0f, -1.0f);
	};

	namespace GameUIPointer
	{

		// The game camera's view-projection for a screen: the scene's primary camera at the screen's aspect ratio (nullopt
		// without one).
		std::optional<glm::mat4> GetGameViewProjection(Scene& scene, const UIScreen& screen);
		// The ray through a pixel (+Y down) of a screen of `screenSize` pixels from a view-projection (reversed-Z clip space).
		std::optional<UIPointerRay> MakeRay(const glm::mat4& viewProjection, const glm::vec2& screenSize, const glm::vec2& pixel);
		// Where a ray crosses a world-space canvas (canvasToWorld: GameUIScene::GetCanvasWorldMatrix): the canvas point in
		// units and the distance along the ray; nullopt when it runs along the plane or the plane is behind it.
		std::optional<std::pair<glm::vec2, float>> IntersectCanvas(const glm::mat4& canvasToWorld, const UIPointerRay& ray);
		// The pixel of a screen a point of a world-space canvas shows at (nullopt behind the camera).
		std::optional<glm::vec2> ProjectCanvasPoint(const glm::mat4& viewProjection, const glm::mat4& canvasToWorld, const glm::vec2& screenSize,
			const glm::vec2& point);

	}

	// Finds the game UI element under a point of a screen: the topmost raycast target (an element with a UIImage or UIText
	// that is a raycast target, or a UISelectable) that is visible and whose rect, as far as masks leave it
	// (GameUIScene::GetVisibleBox), contains the point (half-open), in canvases
	// that receive input, searched from the highest sort order down and within a canvas from the last element drawn. Each
	// canvas keeps a uniform grid of its targets' rects, rebuilt when what hit tests see changed (GameUIScene::GetHitStamp),
	// so a query looks at the targets of one cell. World-space canvases come after every screen-space one: with a ray, the
	// nearest canvas it crosses first. Main thread only.
	class GameUIHitTester
	{
	public:
		// Brings the grids up to date for the views of one EnsureLayout (in draw order). Returns whether anything changed.
		bool Update(GameUIScene& ui, std::span<const UICanvasView> views);
		// The topmost raycast target at a point in screen pixels, among canvases that receive input (`anyCanvas`: all of them);
		// with `ray` (through the same pixel) the world-space canvases are searched after the screen-space ones.
		std::optional<UIHitResult> HitTest(const glm::vec2& pixel, bool anyCanvas = false, const UIPointerRay* ray = nullptr) const;
		// Every visible element whose rect (as far as masks leave it) contains the point, raycast target or not, topmost first
		// (with `ray`, then those of the world-space canvases it crosses, nearest first).
		void HitTestAll(GameUIScene& ui, std::span<const UICanvasView> views, const glm::vec2& pixel, std::vector<UIHitResult>& outHits,
			const UIPointerRay* ray = nullptr) const;
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
			bool WorldSpace = false;
			glm::mat4 CanvasToWorld = glm::mat4(1.0f); // World-space canvases: GameUIScene::GetCanvasWorldMatrix
			glm::uvec2 Cells = glm::uvec2(1);
			glm::vec2 CellSize = glm::vec2(1.0f); // Canvas units
			std::vector<Target> Targets;          // In element (draw) order
			std::vector<uint32_t> CellStarts;     // Per cell, into CellTargets (one more entry than cells)
			std::vector<uint32_t> CellTargets;    // Target indices, ascending within each cell
		};

		// Where a ray crosses a world-space canvas.
		struct WorldCrossing
		{
			float Distance = 0.0f;
			uint32_t View = 0;
			glm::vec2 Point = glm::vec2(0.0f); // Canvas units
		};

		void Build(GameUIScene& ui, const UICanvasView& view, CanvasGrid& grid);
		// The topmost target of a canvas at a point in its units.
		std::optional<UIHitResult> HitTestGrid(uint32_t viewIndex, const glm::vec2& point) const;
		// The world-space canvases a ray crosses, nearest first (into m_Crossings).
		void FindWorldCrossings(const UIPointerRay& ray, bool anyCanvas) const;
	private:
		std::vector<CanvasGrid> m_Grids; // By view, in draw order
		mutable std::vector<WorldCrossing> m_Crossings; // The latest FindWorldCrossings (kept: queries allocate nothing)
		mutable uint32_t m_LastCandidates = 0;
	};

}
