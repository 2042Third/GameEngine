#pragma once

#include "Editor/EditorViewport.h"
#include "Editor/TransformEdit.h"
#include "UI/EditorPanelRegistry.h"
#include "UI/Widgets.h"

#include <Strata/Math/AABB.h>

#include <glm/glm.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strata
{

	class EditorCommandRegistry;
	class EditorContext;
	struct ViewportView;

	// The Scene view (panel id "Viewport", titled "Scene"): renders the active scene through the editor camera
	// (EditorViewport) into a texture the size of the panel, shows it, and handles the view's input while it is hovered or
	// focused: the editor camera (Alt + left drag orbits, middle drag pans, the wheel dollies, right drag flies with WASD/QE,
	// Shift faster and the wheel setting the speed), clicks that select entities (Ctrl toggles, Shift adds), the transform
	// gizmo (W/E/R/Q; snapping per the toolbar's snap toggle, inverted while Ctrl is held), F to frame the selection and Home
	// to frame everything. It stays an editor view while the game plays: the camera, picking, gizmos and outline work on
	// the running scene (edits apply to its running copy), and the Game view (GameViewPanel) shows the game as it is; with
	// "Game camera" (F8, ViewportSettings::PilotGameCamera) it looks through the scene's primary camera with the editor's
	// overlays. A toolbar strip above the image (ViewportToolbar) holds the chips: the camera settings, the overlay toggles
	// (grid, selection outline, scene gizmos, stats), the editor-view settings, the debug view and the game camera; its
	// bottom border takes the play state's color while the scene runs. Agents show here too: a chip at the toolbar's right
	// names the latest mutating command of an automation client ("Agent: entity.create · 3 s ago"; a click shows it in the
	// Activity panel), and for c_AgentFlashSeconds the entities it changed are outlined in the accent, fading out.
	class ViewportPanel : public EditorPanel
	{
	public:
		EditorPanelWindowOptions GetWindowOptions(EditorPanelContext& context) override;
		void OnImGuiRender(EditorPanelContext& context) override;
		void OnHidden(EditorPanelContext& context) override;
		void OnDetach(EditorPanelContext& context) override;
		// A camera or gizmo drag runs, or the outline of what an agent changed fades.
		bool IsAnimating() const override;

		// How long the outline of what an automation command changed shows.
		static constexpr double c_AgentFlashSeconds = 0.6;
		// The chip shows the latest automation command while a client is connected, and this long after it ran.
		static constexpr double c_AgentChipSeconds = 60.0;

		// The clock the agent chip and the outline go by: std::chrono::steady_clock's unless one is given (tests give their
		// own, so that what they see does not depend on how long their frames take). AutomationActivity::Finished is a time
		// of steady_clock.
		using Clock = std::function<std::chrono::steady_clock::time_point()>;
		void SetClock(Clock clock) { m_Clock = std::move(clock); }
		// Whether the outline of what an agent changed shows at `now`: the entities' bounds were taken (the panel was drawn
		// after the command) and c_AgentFlashSeconds have not passed since it finished.
		bool IsFlashing(std::chrono::steady_clock::time_point now) const;

		// Ends interactions that hold state (camera drags, gizmo drags, a locked cursor). Uses no ImGui functions, so it
		// also works after the ImGui context is gone.
		void Reset(EditorContext& context);
		// Where the image was drawn last (screen coordinates), e.g. for layout tests.
		const ViewportImageArea& GetImageArea() const { return m_Image; }
		// Whether the last frame looked through the scene's primary camera ("Game camera").
		bool IsPiloting() const { return m_Piloting; }
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
		void DrawOverlays(EditorContext& context, const ViewportView& view, const EditorFrameStats& frame);
		// While a scene is being opened (EditorContext::IsSceneLoading): a card in the middle of the image naming the scene, with
		// the load's progress. The edited scene stays visible (and read-only) behind it.
		void DrawSceneLoadOverlay(EditorContext& context);
		// The latest automation command, as a chip right-aligned at `top` against `right`.
		void DrawAgentChip(EditorPanelContext& context, float right, float top);
		// While animation.preview poses animators of the edited scene: a chip naming what it shows, so that the pose is never
		// taken for scene data; a click stops every preview (animation.preview {stop: true}).
		void DrawPreviewChip(EditorPanelContext& context);
		// The projected bounds of the entities the latest automation command changed, outlined in the accent while they fade.
		void DrawAgentFlash(EditorContext& context, const ViewportView& view);
		std::chrono::steady_clock::time_point Now() const { return m_Clock ? m_Clock() : std::chrono::steady_clock::now(); }
		void AcceptAssetDrops(EditorContext& context, const EditorCommandRegistry& commands);
		void EndCameraDrag();
		void EndGizmoDrag(EditorContext& context);
	private:
		bool m_Focused = false;
		bool m_Hovered = false;
		bool m_Piloting = false;   // Looking through the scene's primary camera ("Game camera")
		bool m_OverGizmo = false;  // The mouse was over the gizmo in the last frame (asked before the window begins)
		ViewportImageArea m_Image;
		CameraDrag m_CameraDrag = CameraDrag::None;
		bool m_CursorLocked = false; // By fly mode
		std::optional<TransformDrag> m_GizmoDrag;
		// The agent chip's text for the activity and age (in whole seconds) it was made for.
		// The toolbar's chips across frames: the last ones that do not fit with their labels are drawn as icons.
		UI::ChipStripState m_ChipStrip;
		std::string m_AgentChipText;
		uint64_t m_AgentChipActivity = 0;
		int64_t m_AgentChipSeconds = -1;
		// The flash of the activity m_FlashActivity: world bounds taken when it began (dropped when the panel is hidden).
		uint64_t m_FlashActivity = 0;
		std::vector<AABB> m_FlashBounds;
		std::chrono::steady_clock::time_point m_FlashStart;
		Clock m_Clock;
	};

}
