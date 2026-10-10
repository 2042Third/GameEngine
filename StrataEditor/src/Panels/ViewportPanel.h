#pragma once

#include "Editor/EditorViewport.h"
#include "Editor/TransformEdit.h"
#include "UI/EditorPanelRegistry.h"

#include <glm/glm.hpp>

#include <optional>

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
	// the game view: game input goes to the scene and editor tools are off.
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
	private:
		enum class CameraDrag : uint8_t
		{
			None = 0,
			Orbit,
			Pan,
			Fly
		};

		void DrawChips(EditorContext& context);
		void HandleShortcuts(EditorContext& context);
		void UpdateCamera(EditorContext& context, bool hovered);
		void UpdateGizmo(EditorContext& context, const ViewportView& view);
		void DrawOverlays(EditorContext& context, const ViewportView& view, float chipRowBottom);
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
		std::optional<TransformDrag> m_GizmoDrag;
	};

}
