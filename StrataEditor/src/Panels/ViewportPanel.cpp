#include "Panels/ViewportPanel.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorViewport.h"
#include "Editor/ViewportRenderer.h"
#include "Panels/SceneHierarchyPanel.h"
#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/PropertyWidgets.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/Log.h>
#include <Strata/Input/Input.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/SceneRenderer.h>
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

		// Widths of the camera popup's fields, in text heights.
		constexpr float c_CameraFieldWidthInFontSizes = 11.0f;
		// The play-state strip over the image, in text heights (2 pixels at the base text size).
		constexpr float c_PlayStripInFontSizes = 1.0f / 7.0f;

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
		void DrawTextBox(ImDrawList* drawList, const ImVec2& position, const std::vector<std::string>& lines, const ImVec4& color)
		{
			const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
			const ImVec2 padding = ImGui::GetStyle().FramePadding;
			float width = 0.0f;
			for (const std::string& line : lines)
				width = std::max(width, ImGui::CalcTextSize(line.c_str()).x);
			const ImVec2 size(width + padding.x * 2.0f, lineHeight * static_cast<float>(lines.size()) + padding.y * 2.0f);
			drawList->AddRectFilled(position, ImVec2(position.x + size.x, position.y + size.y), ImGui::GetColorU32(UI::GetThemeColors().Overlay),
				ImGui::GetStyle().FrameRounding);
			for (size_t index = 0; index < lines.size(); index++)
				drawList->AddText(ImVec2(position.x + padding.x, position.y + padding.y + lineHeight * static_cast<float>(index)), ImGui::GetColorU32(color),
					lines[index].c_str());
		}

		// The transform gizmo in the theme's axis colors, its lines as thick (relative to the text) at every UI scale.
		void ApplyGizmoStyle()
		{
			const UI::ThemeColors& colors = UI::GetThemeColors();
			const float scale = ImGui::GetStyle().FontScaleDpi;
			const ImGuizmo::Style defaults;
			ImGuizmo::Style& style = ImGuizmo::GetStyle();
			style.TranslationLineThickness = defaults.TranslationLineThickness * scale;
			style.TranslationLineArrowSize = defaults.TranslationLineArrowSize * scale;
			style.RotationLineThickness = defaults.RotationLineThickness * scale;
			style.RotationOuterLineThickness = defaults.RotationOuterLineThickness * scale;
			style.ScaleLineThickness = defaults.ScaleLineThickness * scale;
			style.ScaleLineCircleSize = defaults.ScaleLineCircleSize * scale;
			style.HatchedAxisLineThickness = defaults.HatchedAxisLineThickness * scale;
			style.CenterCircleSize = defaults.CenterCircleSize * scale;
			style.Colors[ImGuizmo::DIRECTION_X] = colors.AxisX;
			style.Colors[ImGuizmo::DIRECTION_Y] = colors.AxisY;
			style.Colors[ImGuizmo::DIRECTION_Z] = colors.AxisZ;
			style.Colors[ImGuizmo::PLANE_X] = UI::WithAlpha(colors.AxisX, defaults.Colors[ImGuizmo::PLANE_X].w);
			style.Colors[ImGuizmo::PLANE_Y] = UI::WithAlpha(colors.AxisY, defaults.Colors[ImGuizmo::PLANE_Y].w);
			style.Colors[ImGuizmo::PLANE_Z] = UI::WithAlpha(colors.AxisZ, defaults.Colors[ImGuizmo::PLANE_Z].w);
			style.Colors[ImGuizmo::SELECTION] = UI::WithAlpha(colors.AccentHover, defaults.Colors[ImGuizmo::SELECTION].w);
		}

		const ImVec4& GetPlayStateColor(const EditorContext& context)
		{
			const UI::ThemeColors::PlayStateColors& colors = UI::GetThemeColors().PlayState;
			if (context.IsPaused())
				return colors.Paused;
			switch (context.GetSceneState())
			{
				case SceneState::Play:     return colors.Play;
				case SceneState::Simulate: return colors.Simulate;
				case SceneState::Edit:     break;
			}
			return colors.Edit;
		}

	}

	EditorPanelWindowOptions ViewportPanel::GetWindowOptions(EditorPanelContext& context)
	{
		ImGuizmo::BeginFrame();

		// ImGuizmo starts a drag only while no ImGui item is hovered or active, so over the gizmo (as of the last frame)
		// the image is not an item; the window then must not move with the mouse either (when it floats).
		m_OverGizmo = ImGuizmo::IsOver();
		EditorPanelWindowOptions options;
		options.NoPadding = true;
		options.Flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		if (m_OverGizmo)
			options.Flags |= ImGuiWindowFlags_NoMove;
		// While the game has the input, arrows, Space and Enter belong to it, not to keyboard navigation of the chips.
		if (context.Context.IsGameInputActive())
			options.Flags |= ImGuiWindowFlags_NoNavInputs;
		return options;
	}

	void ViewportPanel::OnImGuiRender(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		EditorViewport& viewport = context.GetViewport();
		m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

		// The image fills the panel, rendered in framebuffer pixels (Retina displays have more than one per unit). Viewports
		// only get a framebuffer scale of their own with multi-viewport support; like ImGui's renderer data, the panel falls
		// back to the display's otherwise.
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
			return;
		}

		// Elsewhere an item covering the image takes the clicks, so dragging in the viewport never moves a floating window.
		// The chips drawn over it later take precedence where they are.
		if (m_OverGizmo)
		{
			ImGui::Dummy(available);
		}
		else
		{
			ImGui::SetNextItemAllowOverlap();
			ImGui::InvisibleButton("SceneImage", available, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
		}
		m_Hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
		AcceptAssetDrops(context, panelContext.Commands);

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
					viewport.RequestPick(*pixel, size, mode);
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

		const UI::ThemeColors& colors = UI::GetThemeColors();
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
			const ImVec2 padding = ImGui::GetStyle().WindowPadding;
			drawList->AddRectFilled(imageMin, imageMax, ImGui::GetColorU32(colors.Chrome));
			drawList->AddText(ImVec2(imageMin.x + padding.x, imageMax.y - padding.y - ImGui::GetFontSize()), ImGui::GetColorU32(colors.TextSecondary),
				renderer ? "The scene could not be rendered (see the Console)" : "No GPU: the viewport cannot render");
		}

		// A strip in the play state's color while the scene runs: changes made now are discarded when it stops.
		if (context.IsPlaying())
		{
			const float strip = std::max(1.0f, std::floor(ImGui::GetFontSize() * c_PlayStripInFontSizes));
			drawList->AddRectFilled(imageMin, ImVec2(imageMax.x, imageMin.y + strip), ImGui::GetColorU32(GetPlayStateColor(context)));
		}

		if (view && !m_GameView)
			UpdateGizmo(context, *view);

		// The chips go over the image's top left. The game view keeps only the stats toggle, and only while the game does not
		// have the input (before the view is clicked, or after Shift+F1): while it has, every click on the image is the game's.
		const ImGuiStyle& style = ImGui::GetStyle();
		const ImVec2 chipsMin(imageMin.x + style.ItemSpacing.x, imageMin.y + style.ItemSpacing.y);
		ImGui::SetCursorScreenPos(chipsMin);
		if (!m_GameView)
			DrawChips(context);
		else if (!m_GameInputEnabled)
			DrawStatsChip(viewport.GetSettings());
		// Text boxes go below the chip row, also while it is hidden, so they stay in place when it comes and goes.
		if (view)
			DrawOverlays(context, *view, chipsMin.y + ImGui::GetFrameHeight(), panelContext.Frame);
	}

	void ViewportPanel::OnHidden(EditorPanelContext& context)
	{
		// Hidden behind another tab, collapsed or closed: nothing is rendered and held interactions end.
		Reset(context.Context);
	}

	void ViewportPanel::OnDetach(EditorPanelContext& context)
	{
		Reset(context.Context);
	}

	bool ViewportPanel::IsAnimating() const
	{
		return m_CameraDrag != CameraDrag::None || m_GizmoDrag.has_value();
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
	// Chips
	////////////////////////////////////////////////////////////////////////////////

	void ViewportPanel::DrawChips(EditorContext& context)
	{
		EditorViewport& viewport = context.GetViewport();
		ViewportSettings& settings = viewport.GetSettings();
		EditorCamera& camera = viewport.GetCamera();

		if (UI::Chip("Viewport.Camera", Icons::Video, "Camera", "Field of view, clip planes, fly speed and framing"))
			ImGui::OpenPopup("CameraSettings");
		if (ImGui::BeginPopup("CameraSettings"))
		{
			const float fieldWidth = ImGui::GetFontSize() * c_CameraFieldWidthInFontSizes;
			float fov = camera.GetFOV();
			ImGui::SetNextItemWidth(fieldWidth);
			if (ImGui::SliderFloat("Field of view", &fov, 10.0f, 120.0f, "%.0f deg"))
				camera.SetFOV(fov);
			float clip[2] = { camera.GetNear(), camera.GetFar() };
			ImGui::SetNextItemWidth(fieldWidth);
			if (ImGui::DragFloat2("Near / far", clip, 0.1f, EditorCamera::c_MinNear, EditorCamera::c_MaxFar, "%.3g") && !camera.SetClipPlanes(clip[0], clip[1]))
				ST_WARN("The near clip plane must be closer than the far one");
			float speed = camera.GetFlySpeed();
			ImGui::SetNextItemWidth(fieldWidth);
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

		ImGui::SameLine();
		UI::ToggleChip("Viewport.Grid", Icons::Grid3x3, "Grid", &settings.ShowGrid, "The ground grid");
		ImGui::SameLine();
		UI::ToggleChip("Viewport.Outline", Icons::SquareDashed, "Outline", &settings.ShowSelectionOutline, "Outline the selection");
		ImGui::SameLine();
		UI::ToggleChip("Viewport.Gizmos", Icons::Shapes, "Gizmos", &settings.ShowSceneGizmos, "Light, camera and collider shapes");
		ImGui::SameLine();
		DrawStatsChip(settings);
		// View settings of the editor camera outside play mode only: the game always looks the way it will ship.
		ImGui::SameLine();
		const Ref<Scene>& scene = context.GetActiveScene();
		const bool previewNeeded = scene && SceneRenderer::NeedsPreviewLighting(*scene);
		const char* previewTooltip = !settings.PreviewLighting
			? "Light scenes without any light of their own with a preview sun and sky, so their shapes read (editor view only: the game stays unlit)"
			: (previewNeeded ? "This scene has no light of its own: a preview sun and sky light it here so its shapes read. The game will be unlit "
							   "until it gets lights (the scene camera shows it as the game will)."
							 : "This scene has lights of its own, so it shows as the game will. Preview lighting lights scenes without any light.");
		UI::ToggleChip("Viewport.PreviewLighting", Icons::SunMedium, "Preview lighting", &settings.PreviewLighting, previewTooltip, previewNeeded);
		ImGui::SameLine();
		UI::ToggleChip("Viewport.GameUI", Icons::Gamepad2, "Game UI", &settings.ShowGameUI,
			"Show the game's screen-space text (its HUD) in the editor view; the game and the scene camera always show it");
	}

	void ViewportPanel::DrawStatsChip(ViewportSettings& settings)
	{
		UI::ToggleChip("Viewport.Stats", Icons::Gauge, "Stats", &settings.ShowStats, "Frame time, draw calls and loading assets");
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
		ApplyGizmoStyle();
		ImGuizmo::Enable(m_CameraDrag == CameraDrag::None);
		ImGuizmo::SetOrthographic(view.Camera.Orthographic);
		ImGuizmo::SetDrawlist();
		ImGuizmo::SetRect(m_Image.Min.x, m_Image.Min.y, m_Image.Size.x, m_Image.Size.y);

		const float snapStep = settings.Gizmo == GizmoOperation::Translate ? settings.TranslateSnap
			: (settings.Gizmo == GizmoOperation::Rotate ? settings.RotateSnap : settings.ScaleSnap);
		const float snap[3] = { snapStep, snapStep, snapStep };
		// Ctrl inverts the snap toggle for as long as it is held.
		const bool snapping = settings.Snap != ImGui::GetIO().KeyCtrl;
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

	void ViewportPanel::DrawOverlays(EditorContext& context, const ViewportView& view, float chipRowBottom, const EditorFrameStats& frame)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 imageMin(m_Image.Min.x, m_Image.Min.y);
		const float margin = ImGui::GetStyle().ItemSpacing.x;
		// Text boxes go below the chips.
		const float top = chipRowBottom + margin;
		const UI::ThemeColors& colors = UI::GetThemeColors();

		if (!view.Notice.empty())
		{
			const float width = ImGui::CalcTextSize(view.Notice.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
			DrawTextBox(drawList, ImVec2(std::max(imageMin.x + (m_Image.Size.x - width) * 0.5f, imageMin.x), top), { view.Notice }, colors.Warning);
		}

		EditorViewport& viewport = context.GetViewport();
		ViewportRenderer* renderer = viewport.GetRenderer();
		if (viewport.GetSettings().ShowStats && renderer)
		{
			const SceneRendererStats& stats = renderer->GetStats();
			const glm::uvec2 size = renderer->GetSize();
			// The frame's own time: while the editor idles, its frame rate is capped, which says nothing about what frames cost.
			const std::vector<std::string> lines = {
				fmt::format("{:.2f} ms ({:.0f} FPS{})", frame.WorkMilliseconds, frame.FramesPerSecond, frame.Idle ? ", idle" : ""),
				fmt::format("{} x {} pixels", size.x, size.y),
				fmt::format("{} draw calls, {} instances", stats.DrawCalls, stats.Instances),
				fmt::format("{} triangles, {} lights", stats.Triangles, stats.Lights),
				fmt::format("{} assets loading", stats.PendingAssets),
				view.FromScene ? std::string("Scene camera") : std::string("Editor camera")
			};
			// Numbers read best in the monospaced font.
			UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
			DrawTextBox(drawList, ImVec2(imageMin.x + margin, top), lines, colors.Text);
			ImGui::PopFont();
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
