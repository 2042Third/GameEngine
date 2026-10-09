#include "Panels/ViewportPanel.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorViewport.h"
#include "Editor/ViewportRenderer.h"
#include "Panels/SceneHierarchyPanel.h"
#include "UI/PropertyWidgets.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/Log.h>
#include <Strata/Input/Input.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Scene.h>

#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	namespace
	{

		const ImVec4 c_ActiveButtonColor = { 0.24f, 0.42f, 0.70f, 1.0f };
		const ImVec4 c_PlayColor = { 0.30f, 0.60f, 1.0f, 1.0f };
		const ImVec4 c_SimulateColor = { 0.35f, 0.85f, 0.45f, 1.0f };
		const ImVec4 c_PausedColor = { 1.0f, 0.65f, 0.2f, 1.0f };
		const ImU32 c_OverlayBackground = IM_COL32(0, 0, 0, 160);

		// A button that shows whether its option is on.
		bool ToggleButton(const char* label, bool active, const char* tooltip)
		{
			if (active)
				ImGui::PushStyleColor(ImGuiCol_Button, c_ActiveButtonColor);
			const bool pressed = ImGui::Button(label);
			if (active)
				ImGui::PopStyleColor();
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", tooltip);
			return pressed;
		}

		ImTextureID ToTextureID(nvrhi::ITexture* texture)
		{
			return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture));
		}

		ImGuizmo::OPERATION ToImGuizmo(GizmoOperation operation)
		{
			switch (operation)
			{
				case GizmoOperation::Rotate: return ImGuizmo::ROTATE;
				case GizmoOperation::Scale:  return ImGuizmo::SCALE;
				case GizmoOperation::Translate:
				case GizmoOperation::None:   break;
			}
			return ImGuizmo::TRANSLATE;
		}

		// Lines of text over the image, on a dark box so they stay readable on any scene.
		void DrawTextBox(ImDrawList* drawList, const ImVec2& position, const std::vector<std::string>& lines, ImU32 color)
		{
			const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
			const ImVec2 padding = ImGui::GetStyle().FramePadding;
			float width = 0.0f;
			for (const std::string& line : lines)
				width = std::max(width, ImGui::CalcTextSize(line.c_str()).x);
			const ImVec2 size(width + padding.x * 2.0f, lineHeight * static_cast<float>(lines.size()) + padding.y * 2.0f);
			drawList->AddRectFilled(position, ImVec2(position.x + size.x, position.y + size.y), c_OverlayBackground, ImGui::GetStyle().FrameRounding);
			for (size_t index = 0; index < lines.size(); index++)
				drawList->AddText(ImVec2(position.x + padding.x, position.y + padding.y + lineHeight * static_cast<float>(index)), color, lines[index].c_str());
		}

	}

	void ViewportPanel::OnImGuiRender(EditorContext& context, const EditorCommandRegistry& commands)
	{
		ImGuizmo::BeginFrame();
		EditorViewport& viewport = context.GetViewport();

		// ImGuizmo starts a drag only while no ImGui item is hovered or active, so over the gizmo (as of the last frame)
		// the image is not an item; the window then must not move with the mouse either (when it floats).
		const bool overGizmo = ImGuizmo::IsOver();
		ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		if (overGizmo)
			windowFlags |= ImGuiWindowFlags_NoMove;
		// While the game has the input, arrows, Space and Enter belong to it, not to keyboard navigation of the toolbar.
		if (context.IsGameInputActive())
			windowFlags |= ImGuiWindowFlags_NoNavInputs;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		const bool visible = ImGui::Begin("Viewport", nullptr, windowFlags);
		ImGui::PopStyleVar();
		if (!visible)
		{
			// Hidden behind another tab or collapsed: nothing is rendered and held interactions end.
			Reset(context);
			ImGui::End();
			return;
		}
		m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

		DrawToolbar(context);

		// The image fills the rest of the panel, rendered in framebuffer pixels (Retina displays have more than one per
		// unit). Viewports only get a framebuffer scale of their own with multi-viewport support; like ImGui's renderer
		// data, the panel falls back to the display's otherwise.
		const ImVec2 available = ImGui::GetContentRegionAvail();
		const ImVec2 imageMin = ImGui::GetCursorScreenPos();
		const ImVec2 viewportScale = ImGui::GetWindowViewport()->FramebufferScale;
		const ImVec2 displayScale = ImGui::GetIO().DisplayFramebufferScale;
		m_Image.Min = glm::vec2(imageMin.x, imageMin.y);
		m_Image.Size = glm::max(glm::vec2(available.x, available.y), glm::vec2(0.0f));
		m_Image.PixelScale = ViewportImageArea::ChoosePixelScale(glm::vec2(viewportScale.x, viewportScale.y), glm::vec2(displayScale.x, displayScale.y));
		const glm::uvec2 size = m_Image.GetPixelSize();
		viewport.SetSize(size);
		if (size.x == 0 || size.y == 0)
		{
			Reset(context);
			ImGui::End();
			return;
		}

		// Elsewhere an item covering the image takes the clicks, so dragging in the viewport never moves a floating window.
		if (overGizmo)
			ImGui::Dummy(available);
		else
			ImGui::InvisibleButton("SceneImage", available, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
		m_Hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		AcceptAssetDrops(context, commands);

		const float aspectRatio = static_cast<float>(size.x) / static_cast<float>(size.y);
		std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Automatic, aspectRatio);
		m_GameView = view && view->FromScene;
		if (!m_GameView)
		{
			HandleShortcuts(context);
			UpdateCamera(context, m_Hovered);

			// A plain click picks what is under the cursor in the image that is shown; clicks on the gizmo are its own.
			const ImGuiIO& io = ImGui::GetIO();
			if (m_Hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing())
			{
				const ViewportPickMode mode = io.KeyCtrl ? ViewportPickMode::Toggle : (io.KeyShift ? ViewportPickMode::Add : ViewportPickMode::Replace);
				if (const std::optional<glm::uvec2> pixel = m_Image.ToPixel(glm::vec2(io.MousePos.x, io.MousePos.y)))
					viewport.RequestPick(*pixel, mode);
			}
			// The camera may have moved this frame.
			view = ResolveViewportView(context, ViewportCameraSource::Automatic, aspectRatio);
		}
		else
		{
			EndCameraDrag();
			EndGizmoDrag(context);
		}
		UpdateGameInput(context, m_GameView);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 imageMax(imageMin.x + available.x, imageMin.y + available.y);
		ViewportRenderer* renderer = viewport.GetRenderer();
		const bool rendered = renderer && view && renderer->Render(context, size, *view, viewport.GetSettings(), view->EditorOverlays);
		if (rendered)
		{
			drawList->AddImage(ToTextureID(renderer->GetOutputTexture()), imageMin, imageMax);
		}
		else
		{
			drawList->AddRectFilled(imageMin, imageMax, IM_COL32(20, 20, 24, 255));
			drawList->AddText(ImVec2(imageMin.x + 8.0f, imageMin.y + 8.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
				renderer ? "The scene could not be rendered (see the Console)" : "No GPU: the viewport cannot render");
		}

		if (view)
		{
			if (!m_GameView)
				UpdateGizmo(context, *view);
			DrawOverlays(context, *view);
		}
		ImGui::End();
	}

	void ViewportPanel::Reset(EditorContext& context)
	{
		m_Focused = false;
		m_Hovered = false;
		EndCameraDrag();
		EndGizmoDrag(context);
		UpdateGameInput(context, false);
		context.GetViewport().SetSize(glm::uvec2(0, 0));
	}

	////////////////////////////////////////////////////////////////////////////////
	// Toolbar
	////////////////////////////////////////////////////////////////////////////////

	void ViewportPanel::DrawToolbar(EditorContext& context)
	{
		EditorViewport& viewport = context.GetViewport();
		ViewportSettings& settings = viewport.GetSettings();
		EditorCamera& camera = viewport.GetCamera();
		const ImGuiStyle& style = ImGui::GetStyle();
		// The window has no padding (the image reaches its edges); the toolbar keeps some room.
		const ImVec2 start = ImGui::GetCursorPos();
		ImGui::SetCursorPos(ImVec2(start.x + style.ItemSpacing.x, start.y + style.ItemSpacing.y));

		auto gizmoButton = [&settings](const char* label, GizmoOperation operation, const char* tooltip)
		{
			if (ToggleButton(label, settings.Gizmo == operation, tooltip))
				settings.Gizmo = operation;
			ImGui::SameLine();
		};
		gizmoButton("Select", GizmoOperation::None, "No transform gizmo (Q)");
		gizmoButton("Move", GizmoOperation::Translate, "Move the selection (W)");
		gizmoButton("Rotate", GizmoOperation::Rotate, "Rotate the selection (E)");
		gizmoButton("Scale", GizmoOperation::Scale, "Scale the selection (R)");
		if (ToggleButton(settings.Space == GizmoSpace::World ? "World" : "Local", false,
			"Gizmo axes: the world's or the entity's own (scaling always uses the entity's)"))
		{
			settings.Space = settings.Space == GizmoSpace::World ? GizmoSpace::Local : GizmoSpace::World;
		}
		ImGui::SameLine();
		if (ImGui::Button("Snap"))
			ImGui::OpenPopup("SnapSettings");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Snapping steps, used while Ctrl is held");
		if (ImGui::BeginPopup("SnapSettings"))
		{
			ImGui::SetNextItemWidth(120.0f);
			ImGui::DragFloat("Move (units)", &settings.TranslateSnap, 0.05f, 0.001f, ViewportSettings::c_MaxTranslateSnap, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SetNextItemWidth(120.0f);
			ImGui::DragFloat("Rotate (degrees)", &settings.RotateSnap, 0.5f, 0.1f, ViewportSettings::c_MaxRotateSnap, "%.1f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::SetNextItemWidth(120.0f);
			ImGui::DragFloat("Scale (factor)", &settings.ScaleSnap, 0.01f, 0.001f, ViewportSettings::c_MaxScaleSnap, "%.3f", ImGuiSliderFlags_AlwaysClamp);
			ImGui::EndPopup();
		}

		ImGui::SameLine(0.0f, style.ItemSpacing.x * 3.0f);
		if (ToggleButton("Grid", settings.ShowGrid, "Ground grid"))
			settings.ShowGrid = !settings.ShowGrid;
		ImGui::SameLine();
		if (ToggleButton("Outline", settings.ShowSelectionOutline, "Outline the selection"))
			settings.ShowSelectionOutline = !settings.ShowSelectionOutline;
		ImGui::SameLine();
		if (ToggleButton("Gizmos", settings.ShowSceneGizmos, "Light, camera and collider shapes"))
			settings.ShowSceneGizmos = !settings.ShowSceneGizmos;
		ImGui::SameLine();
		if (ToggleButton("Stats", settings.ShowStats, "Frame time, draw calls and loading assets"))
			settings.ShowStats = !settings.ShowStats;
		ImGui::SameLine();
		if (ImGui::Button("Camera"))
			ImGui::OpenPopup("CameraSettings");
		if (ImGui::BeginPopup("CameraSettings"))
		{
			float fov = camera.GetFOV();
			ImGui::SetNextItemWidth(160.0f);
			if (ImGui::SliderFloat("Field of view", &fov, 10.0f, 120.0f, "%.0f deg"))
				camera.SetFOV(fov);
			float clip[2] = { camera.GetNear(), camera.GetFar() };
			ImGui::SetNextItemWidth(160.0f);
			if (ImGui::DragFloat2("Near / far", clip, 0.1f, EditorCamera::c_MinNear, EditorCamera::c_MaxFar, "%.3g") && !camera.SetClipPlanes(clip[0], clip[1]))
				ST_WARN("The near clip plane must be closer than the far one");
			float speed = camera.GetFlySpeed();
			ImGui::SetNextItemWidth(160.0f);
			if (ImGui::DragFloat("Fly speed", &speed, 0.1f, EditorCamera::c_MinFlySpeed, EditorCamera::c_MaxFlySpeed, "%.2f", ImGuiSliderFlags_Logarithmic))
				camera.SetFlySpeed(speed);
			ImGui::Separator();
			if (ImGui::MenuItem("Frame Selection", "F", false, !context.GetSelection().empty()))
			{
				std::vector<Entity> selection;
				for (UUID id : context.GetSelection())
				{
					if (Entity entity = context.GetActiveScene()->GetEntityByUUID(id))
						selection.push_back(entity);
				}
				viewport.Focus(context, selection);
			}
			if (ImGui::MenuItem("Frame All", "Home"))
				viewport.Focus(context, {});
			if (ImGui::MenuItem("Reset View"))
				camera.Reset();
			ImGui::EndPopup();
		}

		// The play state on the right.
		const SceneState state = context.GetSceneState();
		const bool paused = context.IsPaused();
		const char* label = state == SceneState::Edit ? "Editing" : (state == SceneState::Play ? "Playing" : "Simulating");
		const std::string text = paused ? fmt::format("{} (paused)", label) : std::string(label);
		const ImVec4 color = state == SceneState::Edit ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled) : (paused ? c_PausedColor : (state == SceneState::Play ? c_PlayColor : c_SimulateColor));
		const float textWidth = ImGui::CalcTextSize(text.c_str()).x;
		ImGui::SameLine(std::max(ImGui::GetWindowWidth() - textWidth - style.ItemSpacing.x * 2.0f, ImGui::GetCursorPosX()));
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored(color, "%s", text.c_str());

		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.ItemSpacing.y * 0.5f);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Input
	////////////////////////////////////////////////////////////////////////////////

	void ViewportPanel::HandleShortcuts(EditorContext& context)
	{
		// Only while the viewport is the target of the keyboard, never while typing or flying (WASD/QE move the camera).
		// Nor during a gizmo drag, whose operation must not change under it.
		const ImGuiIO& io = ImGui::GetIO();
		if (!(m_Hovered || m_Focused) || io.WantTextInput || m_CameraDrag == CameraDrag::Fly || m_GizmoDrag || ImGuizmo::IsUsing() || io.KeyCtrl || io.KeyAlt
			|| io.KeySuper)
		{
			return;
		}

		ViewportSettings& settings = context.GetViewport().GetSettings();
		if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
			settings.Gizmo = GizmoOperation::None;
		if (ImGui::IsKeyPressed(ImGuiKey_W, false))
			settings.Gizmo = GizmoOperation::Translate;
		if (ImGui::IsKeyPressed(ImGuiKey_E, false))
			settings.Gizmo = GizmoOperation::Rotate;
		if (ImGui::IsKeyPressed(ImGuiKey_R, false))
			settings.Gizmo = GizmoOperation::Scale;
		if (ImGui::IsKeyPressed(ImGuiKey_F, false) && !io.KeyShift)
		{
			std::vector<Entity> selection;
			for (UUID id : context.GetSelection())
			{
				if (Entity entity = context.GetActiveScene()->GetEntityByUUID(id))
					selection.push_back(entity);
			}
			if (!selection.empty())
				context.GetViewport().Focus(context, selection);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
			context.GetViewport().Focus(context, {});
	}

	void ViewportPanel::UpdateCamera(EditorContext& context, bool hovered)
	{
		const ImGuiIO& io = ImGui::GetIO();
		// Drags start on the image (not on the gizmo) and continue wherever the mouse goes until the button is released.
		if (m_CameraDrag == CameraDrag::None && hovered && !ImGuizmo::IsUsing())
		{
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
				m_CameraDrag = CameraDrag::Fly;
			else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
				m_CameraDrag = CameraDrag::Pan;
			else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.KeyAlt && !ImGuizmo::IsOver())
				m_CameraDrag = CameraDrag::Orbit;
			if (m_CameraDrag != CameraDrag::None)
				ImGui::SetWindowFocus();
		}
		const bool held = (m_CameraDrag == CameraDrag::Fly && ImGui::IsMouseDown(ImGuiMouseButton_Right))
			|| (m_CameraDrag == CameraDrag::Pan && ImGui::IsMouseDown(ImGuiMouseButton_Middle))
			|| (m_CameraDrag == CameraDrag::Orbit && ImGui::IsMouseDown(ImGuiMouseButton_Left));
		if (m_CameraDrag != CameraDrag::None && !held)
			EndCameraDrag();

		// Fly mode hides the cursor and frees it from the screen edges; it comes back where it was.
		if (m_CameraDrag == CameraDrag::Fly && !m_CursorLocked)
		{
			Input::SetCursorMode(CursorMode::Locked);
			m_CursorLocked = true;
		}

		EditorCameraInput input;
		// The first frame of a drag has no meaningful delta (the mouse may just have entered the window).
		if (m_CameraDrag != CameraDrag::None && !ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsMouseClicked(ImGuiMouseButton_Right)
			&& !ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
		{
			input.MouseDelta = glm::vec2(io.MouseDelta.x, io.MouseDelta.y);
		}
		if (!std::isfinite(input.MouseDelta.x) || !std::isfinite(input.MouseDelta.y))
			input.MouseDelta = glm::vec2(0.0f);
		input.Orbit = m_CameraDrag == CameraDrag::Orbit;
		input.Pan = m_CameraDrag == CameraDrag::Pan;
		input.Fly = m_CameraDrag == CameraDrag::Fly;
		if (hovered || input.Fly)
			input.Scroll = io.MouseWheel;
		if (input.Fly)
		{
			input.MoveForward = ImGui::IsKeyDown(ImGuiKey_W);
			input.MoveBackward = ImGui::IsKeyDown(ImGuiKey_S);
			input.MoveLeft = ImGui::IsKeyDown(ImGuiKey_A);
			input.MoveRight = ImGui::IsKeyDown(ImGuiKey_D);
			input.MoveUp = ImGui::IsKeyDown(ImGuiKey_E);
			input.MoveDown = ImGui::IsKeyDown(ImGuiKey_Q);
			input.Fast = io.KeyShift;
		}
		context.GetViewport().GetCamera().Update(input, io.DeltaTime, m_Image.Size);
	}

	void ViewportPanel::EndCameraDrag()
	{
		m_CameraDrag = CameraDrag::None;
		if (m_CursorLocked)
		{
			Input::SetCursorMode(CursorMode::Normal);
			m_CursorLocked = false;
		}
	}

	void ViewportPanel::UpdateGameInput(EditorContext& context, bool gameView)
	{
		// Shift+F1 leaves the game view (and frees a cursor the game locked or hid) without stopping the game. Only asked
		// while the game view is shown: Reset runs this without an ImGui frame (when the editor detaches).
		if (gameView && m_Focused && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_F1, false))
		{
			ImGui::SetWindowFocus(nullptr);
			m_Focused = false;
		}

		// Scripts read Input: only the focused game view feeds it, with coordinates relative to the image.
		const bool enabled = gameView && m_Focused && context.GetSceneState() == SceneState::Play;
		// A game that hid or locked the cursor gives it back when it loses the input.
		if (m_GameInputEnabled && !enabled && !m_CursorLocked && Input::GetCursorMode() != CursorMode::Normal)
			Input::SetCursorMode(CursorMode::Normal);
		m_GameInputEnabled = enabled;
		context.SetGameInputActive(enabled);
		Input::SetEnabled(enabled);
		if (enabled)
		{
			const ImVec2 origin = ImGui::GetMainViewport()->Pos;
			Input::SetViewport(glm::vec2(m_Image.Min.x - origin.x, m_Image.Min.y - origin.y), m_Image.Size);
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Gizmo
	////////////////////////////////////////////////////////////////////////////////

	void ViewportPanel::UpdateGizmo(EditorContext& context, const ViewportView& view)
	{
		const ViewportSettings& settings = context.GetViewport().GetSettings();
		Entity primary = context.GetPrimarySelection();
		if (!primary || settings.Gizmo == GizmoOperation::None)
		{
			EndGizmoDrag(context);
			return;
		}

		// A selection changed under a drag (a late pick, automation) ends it: the gizmo now belongs to another entity.
		if (m_GizmoDrag && !m_GizmoDrag->IsForSelection(context))
			EndGizmoDrag(context);

		// Shown but inert while the camera is being dragged.
		ImGuizmo::Enable(m_CameraDrag == CameraDrag::None);
		ImGuizmo::SetOrthographic(view.Camera.Orthographic);
		ImGuizmo::SetDrawlist();
		ImGuizmo::SetRect(m_Image.Min.x, m_Image.Min.y, m_Image.Size.x, m_Image.Size.y);

		const float snapStep = settings.Gizmo == GizmoOperation::Translate ? settings.TranslateSnap
			: (settings.Gizmo == GizmoOperation::Rotate ? settings.RotateSnap : settings.ScaleSnap);
		const float snap[3] = { snapStep, snapStep, snapStep };
		const bool snapping = ImGui::GetIO().KeyCtrl;
		glm::mat4 world = context.GetActiveScene()->GetWorldTransform(primary);
		const ImGuizmo::MODE mode = settings.Space == GizmoSpace::World ? ImGuizmo::WORLD : ImGuizmo::LOCAL;
		const bool changed = ImGuizmo::Manipulate(&view.Camera.View[0][0], &view.Camera.Projection[0][0], ToImGuizmo(settings.Gizmo), mode, &world[0][0], nullptr,
			snapping ? snap : nullptr);

		// The frame the mouse is released still carries the last movement (and snap step): apply it, then end the drag.
		const bool dragging = ImGuizmo::IsUsing();
		if ((dragging || changed) && !m_GizmoDrag)
			m_GizmoDrag = TransformDrag::Begin(context, settings.Gizmo);
		std::string error;
		if (changed && m_GizmoDrag && !m_GizmoDrag->Update(context, world, &error))
		{
			ST_WARN("Transform gizmo: {}", error);
			EndGizmoDrag(context);
			return;
		}
		if (!dragging)
			EndGizmoDrag(context);
	}

	void ViewportPanel::EndGizmoDrag(EditorContext& context)
	{
		// Also cancels ImGuizmo's own drag, which would otherwise stay active while no Manipulate call sees the mouse
		// released (the entity was deleted or the gizmo hidden); the gizmo is enabled again before its next Manipulate.
		if (ImGuizmo::IsUsing())
			ImGuizmo::Enable(false);
		if (!m_GizmoDrag)
			return;
		m_GizmoDrag->End(context);
		m_GizmoDrag.reset();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Overlays and drops
	////////////////////////////////////////////////////////////////////////////////

	void ViewportPanel::DrawOverlays(EditorContext& context, const ViewportView& view)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 imageMin(m_Image.Min.x, m_Image.Min.y);
		const ImVec2 imageMax(m_Image.Min.x + m_Image.Size.x, m_Image.Min.y + m_Image.Size.y);
		const float margin = ImGui::GetStyle().ItemSpacing.x;

		// A colored frame while the scene runs: changes made now are discarded when it stops.
		const SceneState state = context.GetSceneState();
		if (state != SceneState::Edit)
		{
			const ImVec4 color = context.IsPaused() ? c_PausedColor : (state == SceneState::Play ? c_PlayColor : c_SimulateColor);
			drawList->AddRect(imageMin, imageMax, ImGui::ColorConvertFloat4ToU32(color), 0.0f, 2.0f);
		}

		if (!view.Notice.empty())
		{
			const float width = ImGui::CalcTextSize(view.Notice.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
			DrawTextBox(drawList, ImVec2(std::max(imageMin.x + (m_Image.Size.x - width) * 0.5f, imageMin.x), imageMin.y + margin), { view.Notice },
				ImGui::ColorConvertFloat4ToU32(c_PausedColor));
		}

		EditorViewport& viewport = context.GetViewport();
		ViewportRenderer* renderer = viewport.GetRenderer();
		if (viewport.GetSettings().ShowStats && renderer)
		{
			const SceneRendererStats& stats = renderer->GetStats();
			const ImGuiIO& io = ImGui::GetIO();
			const glm::uvec2 size = renderer->GetSize();
			const std::vector<std::string> lines = {
				fmt::format("{:.2f} ms ({:.0f} FPS)", io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f, io.Framerate),
				fmt::format("{} x {} pixels", size.x, size.y),
				fmt::format("{} draw calls, {} instances", stats.DrawCalls, stats.Instances),
				fmt::format("{} triangles, {} lights", stats.Triangles, stats.Lights),
				fmt::format("{} assets loading", stats.PendingAssets),
				view.FromScene ? std::string("Scene camera") : std::string("Editor camera")
			};
			DrawTextBox(drawList, ImVec2(imageMin.x + margin, imageMin.y + margin), lines, IM_COL32(230, 230, 230, 255));
		}
	}

	void ViewportPanel::AcceptAssetDrops(EditorContext& context, const EditorCommandRegistry& commands)
	{
		if (!ImGui::BeginDragDropTarget())
			return;
		const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Asset);
		if (payload && payload->DataSize == static_cast<int>(sizeof(AssetHandle)))
		{
			// Prefabs and models are placed at the camera's target, the point the view centers on.
			AssetHandle asset = UUID::Null();
			std::memcpy(&asset, payload->Data, sizeof(AssetHandle));
			const AssetType type = AssetManager::GetAssetType(asset);
			if (type == AssetType::Prefab || type == AssetType::Model)
			{
				const glm::vec3 target = context.GetViewport().GetCamera().GetTarget();
				const nlohmann::json created = RunEditorCommand(context, commands, "prefab.instantiate", { { "prefab", UUIDToJson(asset) },
					{ "components", { { "Transform", { { "Translation", { target.x, target.y, target.z } } } } } } });
				const auto entities = created.is_object() ? created.find("entities") : created.end();
				if (entities != created.end() && entities->is_array() && !entities->empty())
				{
					if (const std::optional<UUID> root = UUIDFromJson(entities->front()))
						context.Select(*root);
				}
			}
			else
			{
				ST_WARN("Only prefabs and models can be dropped into the viewport ({} is a {})", asset.ToString(), AssetTypeToString(type));
			}
		}
		ImGui::EndDragDropTarget();
	}

}
