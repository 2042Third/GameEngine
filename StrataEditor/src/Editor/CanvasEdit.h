#pragma once

#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/GameUI/GameUIScene.h>
#include <Strata/Renderer/SceneOverlay.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Scene/Entity.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	class EditorContext;
	class Scene;

	// What a drag in the viewport's 2D canvas mode grabs of the selected element: its body (moves it), an edge or a corner
	// (resizes it from there).
	enum class CanvasHandle : uint8_t
	{
		None = 0,
		Move,
		Left, Right, Top, Bottom,
		TopLeft, TopRight, BottomLeft, BottomRight
	};

	namespace CanvasEdit
	{

		const char* GetHandleName(CanvasHandle handle);
		// The handles of a box (canvas units): the eight on its edges and corners, where they are drawn.
		struct HandlePoint
		{
			CanvasHandle Handle = CanvasHandle::None;
			glm::vec2 Position = glm::vec2(0.0f);
		};
		std::vector<HandlePoint> GetHandles(const UIBox& box);

		// The box a drag of `handle` by `delta` makes of `box` (canvas units): a move shifts it, an edge or corner moves those
		// sides (never past the opposite one). With `snap` the sides that move land on whole canvas units.
		UIBox DragBox(const UIBox& box, CanvasHandle handle, const glm::vec2& delta, bool snap);

		// The rect values that put a rect at `box` in `parent` (canvas units), keeping its anchors and pivot (and its size
		// along axes it fits to its content).
		UIRectComponent PlaceRect(const UIRectComponent& rect, const UIBox& box, const UIBox& parent);

		// The canvas the 2D canvas mode shows: the selection's (its root canvas), else the scene's first root canvas in draw
		// order, of the screen-space ones (a world-space canvas is edited in the scene); invalid when the scene has none.
		Entity FindShownCanvas(EditorContext& context);

		// The aspect ratio (width / height) the canvas mode shows a canvas at: its reference resolution's when it scales with
		// the screen, else `fallback` (the viewport's).
		float GetCanvasAspect(Entity canvas, float fallback);

		// What a click at a pixel of a screen selects in canvas mode: the topmost visible element there (any element but the
		// root canvas, drawn or not) where no mask clips it, in the canvas `canvas`. Invalid when there is none.
		Entity PickElement(Scene& scene, Entity canvas, const UIScreen& screen, const glm::vec2& pixel);

		// The canvas mode's zoom and pan of the canvas it shows. At zoom 1 without a pan the canvas fills the panel at its
		// aspect ratio, letterboxed: the fit, the game's screen the canvas is laid out for. Zooming magnifies that layout
		// (it never lays the canvas out again) about a point, panning moves it, and however far it is panned a part of the
		// canvas stays in the panel. Positions are in the panel's units (ImGui's); the pan is kept in fractions of the fit's
		// size, so a resized panel shows the same part of the canvas.
		class CanvasFraming
		{
		public:
			static constexpr float c_MinZoom = 0.1f;
			static constexpr float c_MaxZoom = 32.0f;
			// What stays of the canvas in the panel on each axis however far it is panned: this fraction of the panel (or
			// the whole canvas, when it is smaller).
			static constexpr float c_KeepVisible = 0.25f;
			// How much of the panel a framed box fills (F).
			static constexpr float c_FrameFill = 0.8f;

			struct Frame
			{
				UIBox Panel;   // The panel's image area
				UIBox Fit;     // The whole canvas at zoom 1 without a pan
				UIBox Display; // The zoomed and panned canvas (it may reach past the panel)
				UIBox Visible; // The part of Display inside the panel, its edges on whole pixels: what the viewport renders
				glm::vec2 PixelScale = glm::vec2(1.0f);  // Pixels per panel unit
				glm::uvec2 ScreenPixels = glm::uvec2(0); // The game screen the canvas is laid out for: the fit, in pixels
				float Zoom = 1.0f;                       // Displayed pixels per screen pixel

				bool IsEmpty() const { return ScreenPixels.x == 0 || ScreenPixels.y == 0 || !(Visible.Size.x > 0.0f && Visible.Size.y > 0.0f); }
				// A point of the panel in the screen's pixels, and back.
				glm::vec2 ToScreenPixels(const glm::vec2& panelPoint) const;
				glm::vec2 ToPanel(const glm::vec2& screenPixels) const;
			};

			// The frame for a panel area (panel units, `pixelScale` pixels per unit) and a canvas aspect ratio (width / height).
			Frame Compute(const UIBox& panel, float aspect, const glm::vec2& pixelScale) const;

			// Zooms by `factor` keeping the canvas point under `point` (panel units) where it is.
			void ZoomAbout(const Frame& frame, const glm::vec2& point, float factor);
			// Moves the canvas by `delta` panel units.
			void Pan(const Frame& frame, const glm::vec2& delta);
			// Zooms and centers so that `fraction` (a box in fractions of the canvas, 0-1) fills c_FrameFill of the panel.
			void FrameBox(const Frame& frame, const UIBox& fraction);
			// Back to the fit.
			void Reset();

			float GetZoom() const { return m_Zoom; }
			glm::vec2 GetPan() const { return m_Pan; }
			bool IsFit() const { return m_Zoom == 1.0f && m_Pan == glm::vec2(0.0f); }
		private:
			// The pan that puts the display's top-left corner at `displayMin` for a display of `displaySize`.
			void SetDisplayMin(const Frame& frame, const glm::vec2& displayMin, const glm::vec2& displaySize);
		private:
			float m_Zoom = 1.0f;
			glm::vec2 m_Pan = glm::vec2(0.0f); // The display's center from the fit's, in fit sizes
		};

		// The camera of a frame's visible part: `camera` shows the whole display, and the result shows the part `visible` of
		// it, the same picture magnified (its projection cropped). Its VerticalFOV is widened to a frustum symmetric about the
		// view axis that holds the crop, which is what the renderer's shadow cascades, levels of detail and texture streaming
		// assume (zoomed off the center they pick coarser detail than the magnified picture could show, never less coverage).
		SceneCamera CropCamera(const SceneCamera& camera, const UIBox& display, const UIBox& visible);
		// Where a frame puts the game's screen in the rendered image (the visible part): SceneRenderOptions::ScreenView.
		SceneScreenView GetScreenView(const CanvasFraming::Frame& frame);

		// A drag of a handle of an element in canvas mode: every update is an edit of its rect that merges with the drag's
		// earlier ones, so the whole drag is one undo step (End breaks the merge). Main thread only.
		class CanvasRectDrag
		{
		public:
			// Starts a drag at `pointer` (canvas units) at the screen the canvas mode lays the canvas out for. Nullopt (outError
			// says why) for an element that is not laid out, a root canvas, or a scene that cannot be edited.
			static std::optional<CanvasRectDrag> Begin(EditorContext& context, Entity element, CanvasHandle handle, const glm::vec2& pointer,
				const UIScreen& screen, std::string* outError = nullptr);

			// Moves the grabbed handle to `pointer` (canvas units). False (nothing changed) when the element is gone.
			bool Update(EditorContext& context, const glm::vec2& pointer, bool snap, std::string* outError = nullptr);
			void End(EditorContext& context);

			UUID GetElement() const { return m_Element; }
			CanvasHandle GetHandle() const { return m_Handle; }
		private:
			CanvasRectDrag() = default;
		private:
			UUID m_Element = UUID::Null();
			CanvasHandle m_Handle = CanvasHandle::None;
			glm::vec2 m_Start = glm::vec2(0.0f);
			UIBox m_StartBox;
			UIBox m_Parent;
			std::string m_MergeKey;
		};

	}

}
