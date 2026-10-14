#pragma once

#include "Editor/EditorViewport.h"
#include "UI/EditorPanelRegistry.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace Strata
{

	class EditorContext;

	// The Game view: the active scene through its primary camera as the game shows it (the edited scene's in edit mode, the
	// running scene's in play mode), with its HUD and without the editor's overlays, preview lighting, picking or gizmos.
	// Its toolbar strip holds the resolution menu (ViewportSettings::GameView: the panel's size, an aspect ratio letterboxed
	// in the panel, a fixed size rendered at its pixels and scaled to fit, or the resolution the game asked for) and the
	// stats toggle. While the game plays and the view has the keyboard focus it feeds the game's Input (positions in the
	// image's pixels), and a cursor the game locks stays in it until Shift+F1 leaves the view (Ctrl+P stops the game). It
	// renders through EditorViewport::GetGameRenderer and frees its targets while it is hidden, closed or detached.
	class GameViewPanel : public EditorPanel
	{
	public:
		EditorPanelWindowOptions GetWindowOptions(EditorPanelContext& context) override;
		void OnImGuiRender(EditorPanelContext& context) override;
		void OnHidden(EditorPanelContext& context) override;
		void OnDetach(EditorPanelContext& context) override;

		// Ends the game's input and a cursor the game took. Uses no ImGui functions without a context, so it also works after
		// the ImGui context is gone.
		void Reset(EditorContext& context);
		// Whether the panel was drawn in the latest frame (its tab shows).
		bool IsShown() const { return m_Shown; }
		// Where the image was drawn last (screen coordinates; inside the area under the toolbar, letterboxed for an aspect
		// ratio or a fixed size), and the pixels it was rendered at.
		const ViewportImageArea& GetImageArea() const { return m_Image; }
		const glm::uvec2& GetRenderSize() const { return m_RenderSize; }
		// The notice of the game's window requests the last frame showed (empty when it showed none).
		std::string_view GetShownGameWindowNotice() const { return m_GameWindowNoticeShown ? std::string_view(m_GameWindowNotice) : std::string_view(); }
		// What the view says while the game locked the cursor (how to free it and stop the game): shown for
		// c_MouseHintSeconds after the game took the mouse. Empty when the last frame showed none.
		std::string_view GetShownMouseHint() const { return m_MouseHintShown ? std::string_view(c_MouseHint) : std::string_view(); }
		static constexpr const char* c_MouseHint = "Shift+F1 frees the mouse \xC2\xB7 Ctrl+P stops the game";
		static constexpr double c_MouseHintSeconds = 5.0;
	private:
		void DrawResolutionChip(EditorContext& context);
		// Routes game input (Input) to the running scene only while the view is shown, focused and playing.
		void UpdateGameInput(EditorContext& context, bool shown);
		void DrawOverlays(EditorContext& context, ImDrawList* drawList);
		// What the game asked of its window (EditorGameWindow), at the bottom of the image: play mode does not apply it.
		void DrawGameWindowNotice(EditorContext& context, ImDrawList* drawList);
		// What the view shows without a primary camera: the message the game shows then (GameRenderer).
		void DrawNoCamera(EditorContext& context, ImDrawList* drawList, const ImVec2& areaMin, const ImVec2& areaSize);
	private:
		bool m_Shown = false;
		bool m_Focused = false;
		ViewportImageArea m_Image;
		glm::uvec2 m_RenderSize = { 0, 0 };
		bool m_GameInputEnabled = false;
		bool m_GameOwnsMouse = false;  // The game locked the cursor while the view has the input: ImGui ignores the mouse
		double m_MouseHintUntil = 0.0; // ImGui's time until which the way out shows (c_MouseHint)
		bool m_MouseHintShown = false;
		std::string m_GameWindowNotice; // Rebuilt only when the game's window requests change
		uint64_t m_GameWindowNoticeVersion = 0;
		bool m_GameWindowNoticeShown = false;
		// The custom size's fields while the resolution menu is open.
		int m_CustomWidth = 0;
		int m_CustomHeight = 0;
	};

}
