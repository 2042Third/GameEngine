#pragma once

#include "Editor/EditorViewport.h"
#include "Editor/TransformEdit.h"
#include "UI/EditorPanelRegistry.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	class EditorCommandRegistry;
	class EditorContext;
	struct ViewportView;

	// The scene viewport: renders the active scene (EditorViewport) into a texture the size of the panel, shows it, and
	// handles the viewport's input while it is hovered or focused: the editor camera (Alt + left drag orbits, middle drag
	// pans, the wheel dollies, right drag flies with WASD/QE, Shift faster and the wheel setting the speed), clicks that
	// select entities (Ctrl toggles, Shift adds), the transform gizmo (W/E/R/Q; snapping per the toolbar's snap toggle,
	// inverted while Ctrl is held), F to frame the selection and Home to frame everything. A row of chips over the image's
	// top left holds the camera settings and the overlay toggles (grid, selection outline, scene gizmos, stats), and a strip
	// in the play state's color tops the image while the scene runs. While playing through the scene's camera the panel is
	// the game view: game input goes to the scene while it is focused and editor tools are off; of the chips only the stats
	// toggle remains, shown while the game does not have the input, so that clicks on the image always belong to the game.
	class ViewportPanel : public EditorPanel
	{
	public:
		EditorPanelWindowOptions GetWindowOptions(EditorPanelContext& context) override;
		void OnImGuiRender(EditorPanelContext& context) override;
		void OnHidden(EditorPanelContext& context) override;
		void OnDetach(EditorPanelContext& context) override;
		// A camera or gizmo drag runs.
		bool IsAnimating() const override;

		// Ends interactions that hold state (camera drags, gizmo drags, a locked cursor). Uses no ImGui functions, so it
		// also works after the ImGui context is gone.
		void Reset(EditorContext& context);
		// Where the image was drawn last (screen coordinates), e.g. for layout tests.
		const ViewportImageArea& GetImageArea() const { return m_Image; }
		// The notice of the game's window requests the last frame showed over the game view (empty when it showed none).
		std::string_view GetShownGameWindowNotice() const { return m_GameWindowNoticeShown ? std::string_view(m_GameWindowNotice) : std::string_view(); }
		// What the game view says while the game locked the cursor (how to free it and stop the game): shown for
		// c_MouseHintSeconds after the game took the mouse. Empty when the last frame showed none.
		std::string_view GetShownMouseHint() const { return m_MouseHintShown ? std::string_view(c_MouseHint) : std::string_view(); }
		static constexpr const char* c_MouseHint = "Shift+F1 frees the mouse \xC2\xB7 Ctrl+P stops the game";
		static constexpr double c_MouseHintSeconds = 5.0;
	private:
		enum class CameraDrag : uint8_t
		{
			None = 0,
			Orbit,
			Pan,
			Fly
		};

		void DrawChips(EditorContext& context);
		void DrawStatsChip(ViewportSettings& settings);
		// The debug view (StreamingView): the image as rendered, or colored by streaming state or level of detail.
		void DrawDebugViewChip(ViewportSettings& settings);
		// What the colors of the debug view mean, with how many entities in view have each, at the image's bottom left.
		void DrawDebugViewLegend(EditorContext& context);
		void HandleShortcuts(EditorPanelContext& context);
		void UpdateCamera(EditorContext& context, bool hovered);
		void UpdateGizmo(EditorContext& context, const ViewportView& view);
		void DrawOverlays(EditorContext& context, const ViewportView& view, float chipRowBottom, const EditorFrameStats& frame);
		// The renderer's statistics over the image: frame time, draws, the render scene's proxies and uploads, asset requests.
		static void DrawStatsOverlay(ImDrawList* drawList, const ImVec2& position, const ViewportRenderer& renderer, const ViewportView& view, const EditorFrameStats& frame);
		// What the game asked of its window (EditorGameWindow), at the bottom of the game view: play mode does not apply it.
		void DrawGameWindowNotice(EditorContext& context, const ViewportView& view, ImDrawList* drawList);
		void AcceptAssetDrops(EditorContext& context, const EditorCommandRegistry& commands);
		void EndCameraDrag();
		void EndGizmoDrag(EditorContext& context);
		// Routes game input (Input) to the running scene only while the game view is focused.
		void UpdateGameInput(EditorContext& context, bool gameView);
	private:
		bool m_Focused = false;
		bool m_Hovered = false;
		bool m_GameView = false;   // Playing through the scene's camera
		bool m_OverGizmo = false;  // The mouse was over the gizmo in the last frame (asked before the window begins)
		ViewportImageArea m_Image;
		CameraDrag m_CameraDrag = CameraDrag::None;
		bool m_CursorLocked = false; // By fly mode
		bool m_GameInputEnabled = false;
		bool m_GameOwnsMouse = false; // The game locked the cursor while the game view has the input: ImGui ignores the mouse
		double m_MouseHintUntil = 0.0; // ImGui's time until which the way out shows (c_MouseHint)
		bool m_MouseHintShown = false;
		std::optional<TransformDrag> m_GizmoDrag;
		std::string m_GameWindowNotice; // Rebuilt only when the game's window requests change
		uint64_t m_GameWindowNoticeVersion = 0;
		bool m_GameWindowNoticeShown = false;
	};

}
