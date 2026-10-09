#pragma once

#include "Editor/EditorViewport.h"
#include "Editor/TransformEdit.h"

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
	// select entities (Ctrl toggles, Shift adds), the transform gizmo (W/E/R/Q, snapping while Ctrl is held), F to frame
	// the selection and Home to frame everything. While playing through the scene's camera the panel is the game view:
	// game input goes to the scene and editor tools are off.
	class ViewportPanel
	{
	public:
		void OnImGuiRender(EditorContext& context, const EditorCommandRegistry& commands);
		// Ends interactions that hold state (camera drags, gizmo drags, a locked cursor); call when the editor detaches.
		// Uses no ImGui functions, so it also works after the ImGui context is gone.
		void Reset(EditorContext& context);
	private:
		enum class CameraDrag : uint8_t
		{
			None = 0,
			Orbit,
			Pan,
			Fly
		};

		void DrawToolbar(EditorContext& context);
		void HandleShortcuts(EditorContext& context);
		void UpdateCamera(EditorContext& context, bool hovered);
		void UpdateGizmo(EditorContext& context, const ViewportView& view);
		void DrawOverlays(EditorContext& context, const ViewportView& view);
		void AcceptAssetDrops(EditorContext& context, const EditorCommandRegistry& commands);
		void EndCameraDrag();
		void EndGizmoDrag(EditorContext& context);
		// Routes game input (Input) to the running scene only while the game view is focused.
		void UpdateGameInput(EditorContext& context, bool gameView);
	private:
		bool m_Focused = false;
		bool m_Hovered = false;
		bool m_GameView = false;  // Playing through the scene's camera
		ViewportImageArea m_Image;
		CameraDrag m_CameraDrag = CameraDrag::None;
		bool m_CursorLocked = false;     // By fly mode
		bool m_GameInputEnabled = false;
		std::optional<TransformDrag> m_GizmoDrag;
	};

}
