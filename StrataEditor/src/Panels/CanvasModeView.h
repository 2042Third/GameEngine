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
	struct ViewportView;

	// The viewport's 2D canvas mode (ViewportSettings::CanvasMode, edit mode): the viewport shows the selection's UI canvas
	// (else the first root canvas) at its reference aspect ratio, letterboxed, through the scene's primary camera (else the
	// editor camera) with the game UI on, so a canvas that scales with the screen lays out exactly as at its reference
	// resolution. Over the image it outlines every element and draws the selected element's rect with eight handles and its
	// pivot; a click selects the topmost element, a drag of the selection moves it and a drag of a handle resizes it (one
	// undo step per drag, snapping to whole canvas units while the viewport's Snap is on; Ctrl inverts it). The wheel zooms
	// about the pointer and a middle drag pans (CanvasEdit::CanvasFraming: the layout stays the fit's, magnified), F frames
	// the selection and Home shows the whole canvas again. The transform gizmo and camera controls are off meanwhile.
	// ViewportPanel calls it in one place. Main thread only.
	class CanvasModeView
	{
	public:
		// Wheel notches multiply the zoom by this.
		static constexpr float c_WheelZoomStep = 1.15f;

		// Whether the viewport shows the canvas mode this frame: the setting is on and the scene is not playing.
		static bool IsActive(const EditorContext& context);

		// Frames the shown canvas in the panel's image area under the toolbar (`image` on entry, which no chip covers) and makes
		// `image` the part of it the viewport renders. Returns the canvas (invalid: the mode shows nothing).
		Entity Layout(EditorContext& context, ViewportImageArea& image);
		// The game screen the canvas is laid out for: the canvas at its fit, in pixels, however it is zoomed.
		glm::uvec2 GetScreenPixels() const { return m_Frame.ScreenPixels; }
		// Turns a view of the whole screen (a camera at the screen's aspect ratio) into the view of the part rendered.
		void ApplyView(ViewportView& view) const;

		// Clicks and drags over the image (hovered: the panel's image is hovered), then the outlines, the selection and its
		// handles, then zooming, panning and the framing keys (keyboard: the panel has the keyboard), in the frame Layout
		// made.
		void Update(EditorContext& context, bool hovered, bool keyboard);

		// Ends a drag or pan (the panel is hidden or the mode ends). The framing stays.
		void Reset(EditorContext& context);

		bool IsDragging() const { return m_Drag.has_value() || m_Panning; }
		const CanvasEdit::CanvasFraming& GetFraming() const { return m_Framing; }
	private:
		// The screen the canvas is laid out for: the fit's pixels at the editor window's content scale.
		UIScreen GetScreen(EditorContext& context) const;
		// Wheel zoom, middle-drag pan, F and Home (after the frame's drawing: they show from the next frame on).
		void UpdateFraming(EditorContext& context, const UICanvasView& view, bool hovered, bool keyboard);
	private:
		Entity m_Canvas;
		CanvasEdit::CanvasFraming m_Framing;
		CanvasEdit::CanvasFraming::Frame m_Frame;
		std::optional<CanvasEdit::CanvasRectDrag> m_Drag;
		bool m_PickOnRelease = false; // A press on the selection's rect, which selects what is on top if it does not move
		bool m_Panning = false;
		glm::vec2 m_PressPosition = glm::vec2(0.0f);
	};

}
