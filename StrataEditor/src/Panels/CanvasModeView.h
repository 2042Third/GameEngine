#pragma once

#include "Editor/CanvasEdit.h"
#include "Editor/EditorViewport.h"

#include <Strata/Scene/Entity.h>

#include <glm/glm.hpp>
#include <imgui.h>

#include <optional>

namespace Strata
{

	class EditorContext;

	// The viewport's 2D canvas mode (ViewportSettings::CanvasMode, edit mode): the viewport shows the selection's UI canvas
	// (else the first root canvas) at its reference aspect ratio, letterboxed, through the scene's primary camera (else the
	// editor camera) with the game UI on, so a canvas that scales with the screen lays out exactly as at its reference
	// resolution. Over the image it outlines every element and draws the selected element's rect with eight handles and its
	// pivot; a click selects the topmost element, a drag of the selection moves it and a drag of a handle resizes it (one
	// undo step per drag, snapping to whole canvas units while the viewport's Snap is on; Ctrl inverts it). The transform
	// gizmo and camera controls are off meanwhile. ViewportPanel calls it in one place. Main thread only.
	class CanvasModeView
	{
	public:
		// Whether the viewport shows the canvas mode this frame: the setting is on and the scene is not playing.
		static bool IsActive(const EditorContext& context);

		// Fits the image area into the panel's area at the shown canvas's aspect ratio (bars around it). Returns the canvas.
		Entity Layout(EditorContext& context, ViewportImageArea& image);

		// Clicks and drags over the image (hovered: the panel's image is hovered), then the outlines, the selection and its
		// handles. `image` is the area Layout made.
		void Update(EditorContext& context, const ViewportImageArea& image, bool hovered);

		// Ends a drag (the panel is hidden or the mode ends).
		void Reset(EditorContext& context);

		bool IsDragging() const { return m_Drag.has_value(); }
	private:
		// The screen the canvas is laid out for: the image's pixels at the editor window's content scale.
		static UIScreen GetScreen(EditorContext& context, const ViewportImageArea& image);
	private:
		Entity m_Canvas;
		std::optional<CanvasEdit::CanvasRectDrag> m_Drag;
		bool m_PickOnRelease = false; // A press on the selection's rect, which selects what is on top if it does not move
		glm::vec2 m_PressPosition = glm::vec2(0.0f);
	};

}
