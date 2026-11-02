#include "Panels/ViewportPanel.h"

#include "Editor/AssetBounds.h"
#include "Editor/CommandJournal.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorPlacement.h"
#include "Editor/EditorViewport.h"
#include "Editor/SceneBounds.h"
#include "Editor/UIRectEdit.h"
#include "Editor/ViewportRenderer.h"
#include "Panels/ViewportToolbar.h"
#include "UI/EditorActions.h"
#include "UI/EditorFonts.h"
#include "UI/EditorShell.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/PropertyWidgets.h"
#include "UI/TextFormat.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Animation/AnimationPreview.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Input/Input.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/RendererQuality.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Runtime/GameGraphicsService.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	namespace
	{

		// Widths of the camera popup's fields, in text heights.
		constexpr float c_CameraFieldWidthInFontSizes = 11.0f;
		// The scene load card's width, in text heights.
		constexpr float c_SceneLoadCardInFontSizes = 22.0f;
		// The agent flash's outline, in text heights (2 pixels at the base text size), and how many entities it outlines.
		constexpr float c_AgentFlashThicknessInFontSizes = 1.0f / 7.0f;
		constexpr size_t c_MaxAgentFlashEntities = 64;

		// A length of shadow texels for people: millimeters, centimeters or meters.
		std::string FormatTexelSize(float meters)
		{
			if (meters < 0.01f)
				return fmt::format("{:.1f} mm", meters * 1000.0f);
			if (meters < 1.0f)
				return fmt::format("{:.1f} cm", meters * 100.0f);
			return fmt::format("{:.2f} m", meters);
		}

		// The shadows view's ramp color at a ratio of screen pixels per shadow texel (as Overlay/ShadowView.frag mixes it).
		ImVec4 GetShadowRatioColor(const std::array<glm::vec4, 3>& ramp, float pixelsPerTexel)
		{
			const float t = std::clamp(std::log2(std::max(pixelsPerTexel, 1e-6f)), 0.0f, 2.0f);
			const glm::vec4 color = t < 1.0f ? glm::mix(ramp[0], ramp[1], t) : glm::mix(ramp[1], ramp[2], t - 1.0f);
			return ImVec4(color.r, color.g, color.b, 1.0f);
		}

		// The shadows view's legend: the ramp's three colors with their shares of the covered receivers (each pixel counted at
		// the color nearest its ratio, half a stop either side), then a line per cascade with its slice, texel size and median.
		void AddShadowViewLegend(const ShadowViewStats& stats, const std::array<glm::vec4, 3>& ramp, std::vector<UI::LegendLine>& lines)
		{
			// Bins from the ratio's half stops between the ramp's colors (1.41 and 2.83 pixels per texel).
			constexpr uint32_t c_YellowFirstBin = (4 * 2 + 1) * RenderData::c_ShadowViewBinsPerStop / 2;
			constexpr uint32_t c_RedFirstBin = (4 * 2 + 3) * RenderData::c_ShadowViewBinsPerStop / 2;
			std::array<uint64_t, 3> pixels = {};
			for (uint32_t cascade = 0; cascade < stats.CascadeCount; cascade++)
			{
				for (uint32_t bin = 0; bin < RenderData::c_ShadowViewBins; bin++)
					pixels[bin < c_YellowFirstBin ? 0 : (bin < c_RedFirstBin ? 1 : 2)] += stats.Bins[cascade][bin];
			}
			const double covered = static_cast<double>(std::max(stats.Covered.Pixels, 1u));
			constexpr std::array<const char*, 3> names = { "1 px per texel or finer", "2 px", "4 px or coarser" };
			for (size_t step = 0; step < names.size(); step++)
			{
				const ImVec4 color(ramp[step].r, ramp[step].g, ramp[step].b, 1.0f);
				lines.push_back({ color, names[step], fmt::format("{:.0f}%", 100.0 * static_cast<double>(pixels[step]) / covered) });
			}
			for (uint32_t cascade = 0; cascade < stats.CascadeCount; cascade++)
			{
				const ShadowCascadeStats& layout = stats.Cascades[cascade];
				const ShadowViewRatioStats& ratio = stats.PerCascade[cascade];
				lines.push_back({ GetShadowRatioColor(ramp, ratio.MedianPixelsPerTexel), fmt::format("C{} {:.1f}-{:.1f} m", cascade, layout.Near, layout.Far),
					ratio.Pixels > 0 ? fmt::format("{}, median {:.2f}", FormatTexelSize(layout.WorldPerTexel), ratio.MedianPixelsPerTexel)
						: fmt::format("{}, no receivers", FormatTexelSize(layout.WorldPerTexel)) });
			}
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

	}

	EditorPanelWindowOptions ViewportPanel::GetWindowOptions(EditorPanelContext&)
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
		return options;
	}

	void ViewportPanel::OnImGuiRender(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		EditorViewport& viewport = context.GetViewport();
		ViewportSettings& settings = viewport.GetSettings();
		m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

		// The toolbar strip at the top holds the chips (ViewportToolbar), so none of them ever covers the image. The image
		// fills the rest of the panel, rendered in framebuffer pixels (Retina displays have more than one per unit). Viewports
		// only get a framebuffer scale of their own with multi-viewport support; like ImGui's renderer data, the panel falls
		// back to the display's otherwise.
		const ImGuiStyle& style = ImGui::GetStyle();
		// As many rows as the chips took in the last frame (a narrow view wraps them).
		const ViewportToolbar::Layout layout = ViewportToolbar::Compute(m_ChipStrip.Rows);
		const ImVec2 imageMin = layout.ImageMin;
		const ImVec2 available = layout.ImageSize;
		ImGui::SetCursorScreenPos(imageMin);
		const ImVec2 viewportScale = ImGui::GetWindowViewport()->FramebufferScale;
		const ImVec2 displayScale = ImGui::GetIO().DisplayFramebufferScale;
		m_Image.Min = glm::vec2(imageMin.x, imageMin.y);
		m_Image.Size = glm::max(glm::vec2(available.x, available.y), glm::vec2(0.0f));
		m_Image.PixelScale = ViewportImageArea::ChoosePixelScale(glm::vec2(viewportScale.x, viewportScale.y), glm::vec2(displayScale.x, displayScale.y));
		// The 2D canvas mode shows the canvas at its reference aspect ratio: the image is letterboxed in the area under the
		// toolbar (whose chips never cover it).
		const bool canvasMode = CanvasModeView::IsActive(context) && m_CanvasMode.Layout(context, m_Image);
		if (!canvasMode)
			m_CanvasMode.Reset(context);
		const glm::uvec2 size = m_Image.GetPixelSize();
		// What the view shows of the game is its screen: in canvas mode the canvas at its fit, however it is zoomed (ui.layout,
		// the Inspector's pixels and captures use it).
		const glm::uvec2 screenSize = canvasMode ? m_CanvasMode.GetScreenPixels() : size;
		viewport.SetSize(screenSize);
		if (size.x == 0 || size.y == 0)
		{
			Reset(context);
			return;
		}

		// Elsewhere an item covering the image takes the clicks, so dragging in the viewport never moves a floating window.
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
		UI::ItemProbe::RecordRect("Viewport.Image", imageMin, ImVec2(imageMin.x + available.x, imageMin.y + available.y));
		AcceptAssetDrops();

		// The editor camera, on the edited scene and on the running game alike (the Game view shows the game as it is); or,
		// with "Game camera", the scene's primary camera with the editor's overlays, which the editor camera's controls
		// leave alone.
		const float aspectRatio = static_cast<float>(screenSize.x) / static_cast<float>(screenSize.y);
		std::optional<ViewportView> view;
		if (canvasMode)
		{
			// The canvas over the scene as the game shows it: through the scene's camera, else the editor camera's view, of
			// the part of the screen the zoomed and panned canvas shows.
			view = ResolveViewportView(context, ViewportCameraSource::Scene, aspectRatio);
			if (!view)
				view = ResolveViewportView(context, ViewportCameraSource::Editor, aspectRatio);
			if (view)
				m_CanvasMode.ApplyView(*view);
			m_Piloting = false;
			EndCameraDrag();
			EndGizmoDrag(context);
			if (!m_CanvasMode.IsDragging())
				HandleSelectionShortcuts(panelContext);
		}
		else
		{
			view = ResolveSceneView(context, aspectRatio);
			m_Piloting = view && view->FromScene;
			HandleShortcuts(panelContext);
			if (!m_Piloting)
				UpdateCamera(context, m_Hovered);
			else
				EndCameraDrag();
			// A plain click picks what is under the cursor in the image that is shown (the running scene's entities while the
			// game plays); clicks on the gizmo are its own.
			const ImGuiIO& io = ImGui::GetIO();
			if (m_Hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing())
			{
				const ViewportPickMode mode = io.KeyCtrl ? ViewportPickMode::Toggle : (io.KeyShift ? ViewportPickMode::Add : ViewportPickMode::Replace);
				if (const std::optional<glm::uvec2> pixel = m_Image.ToPixel(glm::vec2(io.MousePos.x, io.MousePos.y)))
				{
					// The game UI the view shows is in front of the scene (its screen-space canvases while Game UI is on, the
					// canvases in the scene always): a click on one of its elements selects it.
					const UIScreen screen { glm::vec2(size), context.GetGameWindow().GetState().ContentScale };
					const glm::mat4 viewProjection = view ? view->Camera.Projection * view->Camera.View : glm::mat4(1.0f);
					const Entity element = UIRectEdit::PickElement(*context.GetActiveScene(), screen, glm::vec2(*pixel) + glm::vec2(0.5f),
						settings.ShowGameUI, view ? &viewProjection : nullptr);
					if (element)
						EditorViewport::ApplyPick(context, element, mode);
					else
						viewport.RequestPick(*pixel, size, mode);
				}
			}
			// The camera may have moved this frame.
			view = ResolveSceneView(context, aspectRatio);
		}
		// An asset dragged over the image: where it would land through the view's final camera (the ghost the renderer draws
		// next), and the drop.
		UpdatePlacement(panelContext, view, canvasMode);

		const UI::ThemeColors& colors = UI::GetThemeColors();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 imageMax(imageMin.x + available.x, imageMin.y + available.y);
		ViewportRenderer* renderer = viewport.GetRenderer();
		// The canvas mode draws the game UI, without the editor's overlays, in the letterboxed area.
		ViewportSettings renderSettings = settings;
		renderSettings.ShowGameUI = renderSettings.ShowGameUI || canvasMode;
		// The editor camera at the Scene view's own quality setting (the project's by default), the scene's camera at the
		// game's (the project's, with the players' options while playing).
		const bool rendered = renderer && view && renderer->Render(context, size, *view, renderSettings, view->EditorOverlays && !canvasMode,
			GetSceneViewQuality(*view, settings.Quality, context.GetGameQuality()), &viewport.GetPlacementPreview());
		if (rendered)
		{
			if (canvasMode)
				drawList->AddRectFilled(imageMin, imageMax, ImGui::GetColorU32(colors.Chrome));
			drawList->AddImage(ViewportToolbar::ToTextureID(renderer->GetOutputTexture()), ImVec2(m_Image.Min.x, m_Image.Min.y),
				ImVec2(m_Image.Min.x + m_Image.Size.x, m_Image.Min.y + m_Image.Size.y));
		}
		else
		{
			const ImVec2 padding = ImGui::GetStyle().WindowPadding;
			drawList->AddRectFilled(imageMin, imageMax, ImGui::GetColorU32(colors.Chrome));
			drawList->AddText(ImVec2(imageMin.x + padding.x, imageMax.y - padding.y - ImGui::GetFontSize()), ImGui::GetColorU32(colors.TextSecondary),
				renderer ? "The scene could not be rendered (see the Console)" : "No GPU: the viewport cannot render");
		}
		// The canvas's outlines and handles, also without an image (a headless editor still edits the layout).
		if (canvasMode)
			m_CanvasMode.Update(context, m_Hovered, m_Hovered || m_Focused);
		ViewportToolbar::DrawBackground(drawList, layout, context);

		if (view)
			DrawAgentFlash(context, *view);
		if (view && !canvasMode)
			UpdateGizmo(context, *view);

		// The chips, in the toolbar from its left; the agent's at its right. The last chips that do not fit with their labels
		// are drawn as icons (their labels in their tooltips), and those that do not fit even so on the rows under the first.
		ImGui::PushClipRect(layout.Min, layout.Max, true);
		ImGui::SetCursorScreenPos(layout.ChipsMin);
		const float agentWidth = PrepareAgentChip(panelContext);
		{
			const float agentRoom = agentWidth > 0.0f ? agentWidth + style.ItemSpacing.x * 2.0f : 0.0f;
			UI::ChipStrip strip(m_ChipStrip, layout.Max.x - layout.ChipsMin.x - agentRoom);
			DrawChips(panelContext, view && view->FromScene);
			DrawPreviewChip(panelContext);
		}
		if (agentWidth > 0.0f)
			DrawAgentChip(panelContext, layout.Max.x - style.ItemSpacing.x, layout.ChipsMin.y, agentWidth);
		ImGui::PopClipRect();
		if (rendered && settings.DebugView != ViewportDebugView::None)
			DrawDebugViewLegend(context);
		if (view)
			DrawOverlays(context, *view, panelContext.Frame);
		DrawPlacementLabel(context, imageMin, imageMax);
		if (context.IsSceneLoading())
			DrawSceneLoadOverlay(context);
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
		return m_CameraDrag != CameraDrag::None || m_GizmoDrag.has_value() || m_CanvasMode.IsDragging() || (!m_FlashBounds.empty() && IsFlashing(Now()));
	}

	bool ViewportPanel::IsFlashing(std::chrono::steady_clock::time_point now) const
	{
		// From the clock, not from the last draw: a viewport that stops being drawn mid-flash must not keep the editor at its
		// full frame rate.
		return !m_FlashBounds.empty() && std::chrono::duration<double>(now - m_FlashStart).count() < c_AgentFlashSeconds;
	}

	void ViewportPanel::Reset(EditorContext& context)
	{
		m_Focused = false;
		m_Hovered = false;
		m_FlashBounds.clear();
		EndCameraDrag();
		EndGizmoDrag(context);
		m_CanvasMode.Reset(context);
		m_Drag.reset();
		context.GetViewport().GetPlacementPreview().Clear();
		context.GetViewport().SetSize(glm::uvec2(0, 0));
	}

	////////////////////////////////////////////////////////////////////////////////
	// Chips
	////////////////////////////////////////////////////////////////////////////////

	void ViewportPanel::DrawChips(EditorPanelContext& panelContext, bool sceneCamera)
	{
		EditorContext& context = panelContext.Context;
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
			// What skinned characters of the edited scene show while it does not play (ViewportSettings::EditPose).
			ImGui::SeparatorText("Characters while editing");
			if (ImGui::MenuItem("Rest pose", nullptr, settings.EditPose == AnimationEditPose::Rest))
				settings.EditPose = AnimationEditPose::Rest;
			UI::ItemProbe::Record("Viewport.EditPose.Rest");
			ImGui::SetItemTooltip("Skinned characters stand in their skeleton's rest pose");
			if (ImGui::MenuItem("First frame of their animation", nullptr, settings.EditPose == AnimationEditPose::ClipStart))
				settings.EditPose = AnimationEditPose::ClipStart;
			UI::ItemProbe::Record("Viewport.EditPose.ClipStart");
			ImGui::SetItemTooltip("Skinned characters show what they play on start at its first frame (their controller's default state, else "
				"their Animator's clip), as the game shows them before its first update");
			ImGui::EndPopup();
		}

		ImGui::SameLine();
		UI::ToggleChip("Viewport.Grid", Icons::Grid3x3, "Grid", &settings.ShowGrid, "The ground grid");
		ImGui::SameLine();
		UI::ToggleChip("Viewport.Outline", Icons::SquareDashed, "Outline", &settings.ShowSelectionOutline, "Outline the selection");
		ImGui::SameLine();
		UI::ToggleChip("Viewport.Gizmos", Icons::Shapes, "Gizmos", &settings.ShowSceneGizmos, "Light, camera and collider shapes");
		ImGui::SameLine();
		// While a world is open: its cells' squares around the camera.
		if (context.GetWorld())
		{
			UI::ToggleChip("Viewport.CellBounds", Icons::Grid2X2, "Cells", &settings.CellBounds,
				"The squares of the world's cells around the camera: loading (Sulfur), loaded (Ochre) and unloading (gray)");
			ImGui::SameLine();
		}
		DrawStatsChip(settings);
		// View settings of the Scene view's editor camera: the Game view always looks the way the game will ship.
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
			"Show the game's screen-space UI (its HUD text and UI canvases) in the editor view, where a click selects a UI element; "
			"the game and the scene camera always show it");
		ImGui::SameLine();
		UI::ToggleChip("Viewport.CanvasMode", Icons::Frame, "2D", &settings.CanvasMode,
			"Show the selection's UI canvas in 2D at its reference aspect ratio, to place and size its elements with handles");
		ImGui::SameLine();
		DrawDebugViewChip(settings);
		ImGui::SameLine();
		DrawQualityChip(panelContext, settings, sceneCamera);
		ImGui::SameLine();
		UI::ToggleChip("Viewport.GameCamera", Icons::Camera, "Game camera", &settings.PilotGameCamera,
			"Look through the scene's primary camera with the editor's overlays (F8); the editor camera stays where it was");
	}

	void ViewportPanel::DrawPreviewChip(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		const AnimationPreview* preview = context.FindAnimationPreview();
		if (!preview || preview->GetCount() == 0 || context.IsPlaying())
			return;
		// Previews are few and rare: the text is made while one shows.
		const std::vector<std::pair<entt::entity, AnimationPreview::Request>> requests = preview->GetRequests();
		std::string text;
		if (requests.size() == 1)
		{
			const auto& [handle, request] = requests.front();
			const Entity animator(handle, context.GetEditScene().get());
			std::string what = request.State;
			if (what.empty() && request.Clip.IsValid())
			{
				const std::optional<AssetMetadata> clip = AssetManager::HasActive() ? AssetManager::GetActive()->GetMetadata(request.Clip) : std::nullopt;
				what = clip ? clip->Name : request.Clip.ToString();
			}
			const std::string name = animator.IsValid() ? animator.GetName() : std::string("an animator");
			text = what.empty() ? fmt::format("Previewing {} at {:.2f} s", name, request.Time)
				: fmt::format("Previewing '{}' at {:.2f} s \xC2\xB7 {}", what, request.Time, name);
		}
		else
		{
			text = fmt::format("Previewing {} animators", requests.size());
		}
		ImGui::SameLine();
		const bool clicked = UI::Chip("Viewport.AnimationPreview", Icons::Bone, text.c_str(), nullptr);
		if (UI::IsTooltipDue())
			ImGui::SetTooltip("%s", "Poses shown by animation.preview: the scene is unchanged.\nClick to stop the preview.");
		if (clicked)
			RunEditorCommand(context, panelContext.Commands, "animation.preview", { { "stop", true } });
	}

	float ViewportPanel::PrepareAgentChip(EditorPanelContext& panelContext)
	{
		const AutomationActivity& activity = panelContext.Context.GetJournal().GetLatestAutomationActivity();
		if (activity.Id == 0)
			return 0.0f;
		const double age = std::chrono::duration<double>(Now() - activity.Finished).count();
		const bool connected = panelContext.Shell && panelContext.Shell->GetAutomationState().Clients > 0;
		if (!connected && age >= c_AgentChipSeconds)
			return 0.0f;

		// The text changes once a second at most: it is made again only then.
		const int64_t seconds = static_cast<int64_t>(age);
		if (activity.Id != m_AgentChipActivity || seconds != m_AgentChipSeconds)
		{
			m_AgentChipActivity = activity.Id;
			m_AgentChipSeconds = seconds;
			m_AgentChipText = fmt::format("Agent: {} \xC2\xB7 {}", activity.Command, UI::DescribeSecondsAgo(age));
		}
		return UI::ChipWidth(Icons::Bot, m_AgentChipText.c_str());
	}

	void ViewportPanel::DrawAgentChip(EditorPanelContext& panelContext, float right, float top, float width)
	{
		const AutomationActivity& activity = panelContext.Context.GetJournal().GetLatestAutomationActivity();
		const double age = std::chrono::duration<double>(Now() - activity.Finished).count();
		ImGui::SetCursorScreenPos(ImVec2(right - width, top));
		const bool clicked = UI::Chip("Viewport.Agent", Icons::Bot, m_AgentChipText.c_str(), nullptr);
		if (UI::IsTooltipDue())
		{
			const std::string client = activity.Client.empty() ? std::string("An automation client") : activity.Client;
			ImGui::SetTooltip("%s", fmt::format("{} ran {} {}{}.\nClick to show it in the Activity panel.", client, activity.Command, UI::DescribeSecondsAgo(age),
				activity.Succeeded ? "" : " (it failed)").c_str());
		}
		if (clicked && panelContext.Shell)
			panelContext.Shell->ShowActivity(activity.Id);
	}

	void ViewportPanel::DrawAgentFlash(EditorContext& context, const ViewportView& view)
	{
		const AutomationActivity& activity = context.GetJournal().GetLatestAutomationActivity();
		if (activity.Id != m_FlashActivity)
		{
			// A new command: its entities' bounds now (at most c_MaxAgentFlashEntities of them). Whether they show depends on
			// the time alone (IsFlashing): a command that finished c_AgentFlashSeconds ago shows nothing.
			m_FlashActivity = activity.Id;
			m_FlashBounds.clear();
			m_FlashStart = activity.Finished;
			Scene& scene = *context.GetActiveScene();
			for (size_t index = 0; index < activity.Entities.size() && m_FlashBounds.size() < c_MaxAgentFlashEntities; index++)
			{
				if (Entity entity = scene.GetEntityByUUID(activity.Entities[index]))
				{
					const AABB bounds = SceneBounds::GetEntityBounds(scene, entity, false);
					if (bounds.IsValid())
						m_FlashBounds.push_back(bounds);
				}
			}
		}
		const auto now = Now();
		if (!IsFlashing(now))
			return;
		const double elapsed = std::chrono::duration<double>(now - m_FlashStart).count();

		const ImVec2 imageMin(m_Image.Min.x, m_Image.Min.y);
		const ImVec2 imageMax(m_Image.Min.x + m_Image.Size.x, m_Image.Min.y + m_Image.Size.y);
		const glm::mat4 viewProjection = view.Camera.Projection * view.Camera.View;
		const float alpha = static_cast<float>(1.0 - elapsed / c_AgentFlashSeconds);
		const ImU32 color = UI::ToColorU32(UI::GetThemeColors().Accent, alpha);
		const float thickness = std::max(1.0f, std::floor(ImGui::GetFontSize() * c_AgentFlashThicknessInFontSizes));
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(imageMin, imageMax, true);
		bool recorded = false;
		for (const AABB& bounds : m_FlashBounds)
		{
			// The screen rectangle of the box's corners; boxes reaching behind the camera are left out.
			glm::vec2 low(std::numeric_limits<float>::max());
			glm::vec2 high(std::numeric_limits<float>::lowest());
			bool visible = true;
			for (int corner = 0; corner < 8 && visible; corner++)
			{
				const glm::vec3 point((corner & 1) ? bounds.Max.x : bounds.Min.x, (corner & 2) ? bounds.Max.y : bounds.Min.y, (corner & 4) ? bounds.Max.z : bounds.Min.z);
				const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
				if (clip.w <= 0.0f)
				{
					visible = false;
					break;
				}
				const glm::vec2 ndc = glm::vec2(clip) / clip.w;
				const glm::vec2 screen(m_Image.Min.x + (ndc.x * 0.5f + 0.5f) * m_Image.Size.x, m_Image.Min.y + (0.5f - ndc.y * 0.5f) * m_Image.Size.y);
				low = glm::min(low, screen);
				high = glm::max(high, screen);
			}
			if (!visible)
				continue;
			drawList->AddRect(ImVec2(low.x, low.y), ImVec2(high.x, high.y), color, thickness, thickness);
			if (!recorded)
			{
				UI::ItemProbe::RecordRect("Viewport.AgentFlash", ImVec2(low.x, low.y), ImVec2(high.x, high.y), true, color);
				recorded = true;
			}
		}
		drawList->PopClipRect();
	}

	void ViewportPanel::DrawStatsChip(ViewportSettings& settings)
	{
		UI::ToggleChip("Viewport.Stats", Icons::Gauge, "Stats", &settings.ShowStats, "Frame time, draw calls and loading assets");
	}

	void ViewportPanel::DrawDebugViewChip(ViewportSettings& settings)
	{
		// Lit while a debug view colors the image; a click opens the choice rather than toggling.
		const bool active = settings.DebugView != ViewportDebugView::None;
		bool clicked = active;
		// Named after what the image shows, like the menu's items.
		const char* label = "Lit";
		if (settings.DebugView == ViewportDebugView::Streaming)
			label = "Streaming";
		else if (settings.DebugView == ViewportDebugView::LOD)
			label = "LOD";
		else if (settings.DebugView == ViewportDebugView::Mip)
			label = "Mip";
		else if (settings.DebugView == ViewportDebugView::Motion)
			label = "Motion";
		else if (settings.DebugView == ViewportDebugView::Shadows)
			label = "Shadows";
		else if (settings.DebugView == ViewportDebugView::Lighting)
			label = "Lighting";
		else if (settings.DebugView == ViewportDebugView::AmbientOcclusion)
			label = "AO";
		else if (settings.DebugView == ViewportDebugView::Indirect)
			label = "Indirect";
		else if (settings.DebugView == ViewportDebugView::Luminance)
			label = "Luminance";
		else if (settings.DebugView == ViewportDebugView::Albedo)
			label = "Albedo";
		if (UI::ToggleChip("Viewport.DebugView", Icons::Layers, label, &clicked,
			"Debug views: color the scene by streaming state (meshes, materials and textures still loading or failed), by the level of detail "
			"meshes are drawn at, by the mip levels textures lack against what the view needs, by how each pixel moved since the previous frame, "
			"or by how many screen pixels a texel of the sun's shadow cascades spans; or show its light: surfaces shaded white, ambient "
			"occlusion, indirect light alone, luminance in EV100 bands, or albedo"))
		{
			ImGui::OpenPopup("DebugView");
		}
		if (!ImGui::BeginPopup("DebugView"))
			return;
		const std::pair<ViewportDebugView, const char*> views[] = {
			{ ViewportDebugView::None, "Lit (no debug view)" },
			{ ViewportDebugView::Streaming, "Streaming: loading and failed assets" },
			{ ViewportDebugView::LOD, "LOD: level of detail drawn" },
			{ ViewportDebugView::Mip, "Mip: texture levels resident against needed" },
			{ ViewportDebugView::Motion, "Motion: movement since the previous frame" },
			{ ViewportDebugView::Shadows, "Shadows: cascades and pixels per texel" },
			{ ViewportDebugView::Lighting, "Lighting: every surface white, lit as the scene lights it" },
			{ ViewportDebugView::AmbientOcclusion, "AO: the occlusion that darkens indirect light" },
			{ ViewportDebugView::Indirect, "Indirect: sky, probe and reflected light only" },
			{ ViewportDebugView::Luminance, "Luminance: EV100 bands of the scene's light" },
			{ ViewportDebugView::Albedo, "Albedo: base colors, out of 0.03-0.85 marked" }
		};
		for (const auto& [view, name] : views)
		{
			if (ImGui::MenuItem(name, nullptr, settings.DebugView == view))
				settings.DebugView = view;
		}
		ImGui::EndPopup();
	}

	void ViewportPanel::DrawQualityChip(EditorPanelContext& panelContext, ViewportSettings& settings, bool sceneCamera)
	{
		// Lit while the view chooses its own preset; named after the preset in effect: the view's for the editor camera, the
		// game's through the scene's camera (the project's, or a player's choice while playing), where the view's own preset
		// does nothing (on, but muted). A click opens the choice.
		EditorContext& context = panelContext.Context;
		QualityChipKey key;
		key.Project = RendererQuality::GetProjectQuality().Preset;
		const bool own = settings.Quality != ViewportQuality::Project;
		RendererQualityPreset preset = own ? ResolveViewportQuality(settings.Quality).Preset : key.Project;
		if (sceneCamera)
		{
			key.Game = context.GetGameQuality().Preset;
			key.GameSource = static_cast<int32_t>(context.GetGameGraphics().GetSource("Preset").value_or(GameGraphicsSource::Project));
			preset = key.Game;
		}
		if (m_QualityChipKey != key)
		{
			const char* name = RendererQuality::PresetToString(key.Project);
			m_QualityTooltip = fmt::format("The Scene view's rendering quality. Project follows Project Settings > Graphics Quality ({}); a preset "
				"here changes only this view. The Game view, captures through the scene camera and the game follow the project and the players' "
				"options.", name);
			if (sceneCamera)
			{
				const char* source = key.GameSource == static_cast<int32_t>(GameGraphicsSource::Player) ? "the player's choice"
					: key.GameSource == static_cast<int32_t>(GameGraphicsSource::CommandLine) ? "the command line's" : "the project's";
				m_QualityTooltip = fmt::format("Through the scene's camera the view draws as the Game view does, at the game's quality ({}, {}). {}",
					RendererQuality::PresetToString(key.Game), source, m_QualityTooltip);
			}
			m_QualityProjectItem = fmt::format("Project ({})", name);
			m_QualityChipKey = key;
		}
		bool clicked = own;
		if (UI::ToggleChip("Viewport.Quality", Icons::MonitorCog, RendererQuality::PresetToString(preset), &clicked, m_QualityTooltip.c_str(), !sceneCamera))
			ImGui::OpenPopup("Quality");
		if (!ImGui::BeginPopup("Quality"))
			return;
		const std::pair<ViewportQuality, const char*> items[] = {
			{ ViewportQuality::Project, "Project" },
			{ ViewportQuality::Low, "Low" },
			{ ViewportQuality::Medium, "Medium" },
			{ ViewportQuality::High, "High" },
			{ ViewportQuality::Ultra, "Ultra" }
		};
		for (const auto& [quality, name] : items)
		{
			const char* label = quality == ViewportQuality::Project ? m_QualityProjectItem.c_str() : name;
			if (ImGui::MenuItem(label, nullptr, settings.Quality == quality))
				settings.Quality = quality;
			UI::ItemProbe::Record(fmt::format("Viewport.Quality.{}", name));
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Graphics Quality...", nullptr, false, panelContext.Shell != nullptr))
			panelContext.Shell->ShowProjectSettings(std::string(RendererQuality::c_SectionId));
		UI::ItemProbe::Record("Viewport.Quality.Open", panelContext.Shell != nullptr);
		ImGui::SetItemTooltip("The project's quality, its presets side by side with what each costs (Project Settings)");
		ImGui::EndPopup();
	}

	void ViewportPanel::DrawDebugViewLegend(EditorContext& context)
	{
		ViewportRenderer* renderer = context.GetViewport().GetRenderer();
		if (!renderer)
			return;
		const ViewportSettings& settings = context.GetViewport().GetSettings();
		const StreamingViewStats& stats = renderer->GetDebugViewStats();
		const auto toColor = [](const glm::vec4& color) { return ImVec4(color.r, color.g, color.b, color.a); };
		std::vector<UI::LegendLine> lines;
		if (settings.DebugView == ViewportDebugView::Streaming)
		{
			lines.push_back({ toColor(settings.DebugColors.Pending), "Loading", std::to_string(stats.PendingEntities) });
			lines.push_back({ toColor(settings.DebugColors.Failed), "Failed", std::to_string(stats.FailedEntities) });
		}
		else if (settings.DebugView == ViewportDebugView::LOD)
		{
			for (size_t level = 0; level < settings.DebugColors.LOD.size(); level++)
			{
				const bool last = level + 1 == settings.DebugColors.LOD.size();
				lines.push_back({ toColor(settings.DebugColors.LOD[level]), GetLODLegendName(level, last, stats.MaxErrorPixelsPerLOD[level]),
					std::to_string(stats.EntitiesPerLOD[level]) });
			}
		}
		else if (settings.DebugView == ViewportDebugView::Mip)
		{
			// Entities by the mip levels their worst texture lacks against what the view needs.
			constexpr std::array<const char*, 4> names = { "Mips as needed", "1 level short", "2 levels short", "3+ short, loading" };
			static_assert(names.size() == std::tuple_size_v<decltype(ViewportDebugColors::Mip)>);
			for (size_t shortfall = 0; shortfall < settings.DebugColors.Mip.size(); shortfall++)
				lines.push_back({ toColor(settings.DebugColors.Mip[shortfall]), names[shortfall], std::to_string(stats.EntitiesPerMipShortfall[shortfall]) });
		}
		else if (settings.DebugView == ViewportDebugView::Shadows)
			AddShadowViewLegend(renderer->GetShadowViewStats(), settings.DebugColors.ShadowRatio, lines);
		const std::optional<LightingViewStats>& lighting = renderer->GetLightingViewStats();
		const LightingViewColors& lightingColors = settings.DebugColors.Lighting;
		const auto formatShare = [](float share) { return fmt::format("{:.1f}%", share * 100.0f); };
		if (settings.DebugView == ViewportDebugView::Luminance)
		{
			// The bands from the brightest down, each one EV wide (the last and the first take what lies beyond).
			for (size_t band = lightingColors.Luminance.size(); band-- > 0;)
			{
				lines.push_back({ toColor(lightingColors.Luminance[band]), GetLuminanceLegendName(band, lightingColors.Luminance.size(),
					lightingColors.LuminanceFirstEV100), std::string() });
			}
		}
		else if (settings.DebugView == ViewportDebugView::Albedo)
		{
			lines.push_back({ toColor(lightingColors.AlbedoHigh), "Above 0.85", lighting ? formatShare(lighting->AboveRangeShare) : std::string("-") });
			lines.push_back({ toColor(lightingColors.AlbedoLow), "Below 0.03", lighting ? formatShare(lighting->BelowRangeShare) : std::string("-") });
		}
		std::string note;
		if (settings.DebugView == ViewportDebugView::Streaming && stats.PlaceholderBoxes > 0)
			note = fmt::format("{} boxes: meshes not there yet", stats.PlaceholderBoxes);
		else if (settings.DebugView == ViewportDebugView::Motion)
			note = "Hue: direction, strength: speed (full at 16 px)";
		else if (settings.DebugView == ViewportDebugView::Shadows)
			note = renderer->GetShadowViewStats().CascadeCount > 0 ? "White: cascade edges. Hatched: blend bands." : "No shadowed directional light";
		else if (GetLightingView(settings.DebugView) != SceneLightingView::None)
			note = GetLightingLegendNote(settings.DebugView, lighting); // The sun to sky ratio on flat ground (a clear day's is 6 to 10)

		// At the image's bottom left.
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		const ImGuiStyle& style = ImGui::GetStyle();
		UI::Legend("Viewport.Legend", ImVec2(m_Image.Min.x + style.ItemSpacing.x, m_Image.Min.y + m_Image.Size.y - style.ItemSpacing.y), lines, note);
		ImGui::PopFont();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Input
	////////////////////////////////////////////////////////////////////////////////

	void ViewportPanel::HandleSelectionShortcuts(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		EditorActions::HandleSelectionShortcuts(context, panelContext.Commands, true);
		const Entity primary = context.GetPrimarySelection();
		if (primary && panelContext.Shell && context.AcceptsEditShortcuts() && ImGui::Shortcut(ImGuiKey_F2, ImGuiInputFlags_RouteFocused))
			panelContext.Shell->RenameEntity(primary.GetUUID());
	}

	void ViewportPanel::HandleShortcuts(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		const ImGuiIO& io = ImGui::GetIO();
		// The selection's shortcuts while the viewport has the keyboard focus, never during a camera fly or a gizmo drag.
		if (m_CameraDrag != CameraDrag::Fly && !m_GizmoDrag && !ImGuizmo::IsUsing())
			HandleSelectionShortcuts(panelContext);

		// Only while the viewport is the target of the keyboard, never while typing or flying (WASD/QE move the camera).
		// Nor during a gizmo drag, whose operation must not change under it.
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
		if (ImGui::IsKeyPressed(ImGuiKey_F8, false))
			settings.PilotGameCamera = !settings.PilotGameCamera;
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

	////////////////////////////////////////////////////////////////////////////////
	// Gizmo
	////////////////////////////////////////////////////////////////////////////////

	void ViewportPanel::UpdateGizmo(EditorContext& context, const ViewportView& view)
	{
		const ViewportSettings& settings = context.GetViewport().GetSettings();
		Entity primary = context.GetPrimarySelection();
		// No gizmo over a scene that is being replaced (a scene is being opened): it is read-only.
		const bool readOnly = context.IsSceneLoading() && !context.IsPlaying();
		if (!primary || settings.Gizmo == GizmoOperation::None || readOnly)
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

	void ViewportPanel::DrawOverlays(EditorContext& context, const ViewportView& view, const EditorFrameStats& frame)
	{
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 imageMin(m_Image.Min.x, m_Image.Min.y);
		const float margin = ImGui::GetStyle().ItemSpacing.x;
		const float top = imageMin.y + margin;
		const UI::ThemeColors& colors = UI::GetThemeColors();

		if (!view.Notice.empty())
		{
			const float width = ImGui::CalcTextSize(view.Notice.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
			ViewportToolbar::DrawTextBox(drawList, ImVec2(std::max(imageMin.x + (m_Image.Size.x - width) * 0.5f, imageMin.x), top), { view.Notice }, colors.Warning);
		}

		EditorViewport& viewport = context.GetViewport();
		ViewportRenderer* renderer = viewport.GetRenderer();
		if (viewport.GetSettings().ShowStats && renderer)
			ViewportToolbar::DrawStatsOverlay(drawList, ImVec2(imageMin.x + margin, top), *renderer, view, frame);
	}

	void ViewportPanel::DrawSceneLoadOverlay(EditorContext& context)
	{
		const EditorSceneLoadStatus& status = context.GetSceneLoadStatus();
		const ImGuiStyle& style = ImGui::GetStyle();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const std::string name = DescribeOpeningScene(context);
		// The load's progress counts the scene's document as it is read (SceneReadProgress), then what the scene uses by bytes:
		// the bar moves on in both steps, and the step says what the time goes to (last, the view's render proxies are built).
		// Before it, a project's start scene waits for the scan of the project's assets, whose progress shows then.
		float progress = status.Progress;
		std::string stage;
		if (const std::optional<AssetScanProgress> scan = context.GetAssetScanProgress())
		{
			const bool walking = scan->Phase == AssetScanPhase::Walking || scan->Total == 0;
			progress = walking ? 0.0f : static_cast<float>(scan->Done) / static_cast<float>(scan->Total);
			stage = walking ? fmt::format("Listing the project's files: {}", StringUtils::FormatThousands(scan->Done))
				: fmt::format("Registering the project's assets: {} / {}", StringUtils::FormatThousands(scan->Done), StringUtils::FormatThousands(scan->Total));
		}
		else
		{
			const char* step = status.State == SceneLoadState::LoadingScene ? "Reading the scene"
				: status.State == SceneLoadState::Preparing ? "Preparing the view" : "Loading what it uses";
			stage = fmt::format("{} {:.0f}%", step, std::floor(status.Progress * 100.0f));
		}

		// A card in the middle of the image: the scene's name over the progress bar (no item takes the image's clicks but the
		// bar, which takes none either).
		const float width = std::min(ImGui::GetFontSize() * c_SceneLoadCardInFontSizes, std::max(m_Image.Size.x - style.WindowPadding.x * 2.0f, 0.0f));
		const float height = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeight() + style.WindowPadding.y * 2.0f;
		const ImVec2 cardMin(m_Image.Min.x + (m_Image.Size.x - width) * 0.5f, m_Image.Min.y + (m_Image.Size.y - height) * 0.5f);
		const ImVec2 cardMax(cardMin.x + width, cardMin.y + height);
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(cardMin, cardMax, ImGui::GetColorU32(colors.Overlay), style.WindowRounding);
		const ImVec2 contentMin(cardMin.x + style.WindowPadding.x, cardMin.y + style.WindowPadding.y);
		const float contentWidth = std::max(width - style.WindowPadding.x * 2.0f, 0.0f);
		UI::PushFont(UI::EditorFont::SemiBold, UI::TextSize::Body);
		const std::string title = UI::ElideStart(fmt::format("Opening {}", name), contentWidth);
		drawList->AddText(contentMin, ImGui::GetColorU32(colors.Text), title.c_str());
		ImGui::PopFont();
		ImGui::SetCursorScreenPos(ImVec2(contentMin.x, contentMin.y + ImGui::GetTextLineHeightWithSpacing()));
		UI::StrataProgressBar("Viewport.SceneLoad", progress, stage, contentWidth);
	}

	void ViewportPanel::AcceptAssetDrops()
	{
		m_Drag.reset();
		if (!ImGui::BeginDragDropTarget())
			return;
		// Accepted before the release, so that the view shows where it would land while it hovers: no rectangle around the
		// whole view, and the view's label at the pointer instead of the source's tooltip. Every asset is accepted, so that a
		// release over the view always ends the drag here; those that cannot be placed do nothing (the label said why).
		const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Asset,
			ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect | ImGuiDragDropFlags_AcceptNoPreviewTooltip);
		if (payload && payload->DataSize == static_cast<int>(sizeof(AssetHandle)) && m_Image.Size.x > 0.0f && m_Image.Size.y > 0.0f)
		{
			AssetHandle asset = UUID::Null();
			std::memcpy(&asset, payload->Data, sizeof(AssetHandle));
			m_Drag = AssetDrag { asset, payload->IsDelivery() };
		}
		ImGui::EndDragDropTarget();
	}

	void ViewportPanel::UpdatePlacement(EditorPanelContext& panelContext, const std::optional<ViewportView>& view, bool canvasMode)
	{
		EditorContext& context = panelContext.Context;
		PlacementPreview& preview = context.GetViewport().GetPlacementPreview();
		if (!m_Drag || !view)
		{
			if (preview.IsActive())
				preview.Clear();
			return;
		}
		// The pointer's ray through the camera the view shows (the scene's with Game camera), snapping as the gizmo does: the
		// toolbar's Snap, inverted while Ctrl is held.
		const ViewportSettings& settings = context.GetViewport().GetSettings();
		const ImGuiIO& io = ImGui::GetIO();
		const glm::vec2 ndc((io.MousePos.x - m_Image.Min.x) / m_Image.Size.x * 2.0f - 1.0f, 1.0f - (io.MousePos.y - m_Image.Min.y) / m_Image.Size.y * 2.0f);
		PlacementPointer pointer;
		pointer.Asset = m_Drag->Asset;
		pointer.PointerRay = EditorPlacement::MakeViewRay(view->Camera, ndc);
		pointer.MaxDistance = view->Camera.Far;
		pointer.FallbackDistance = EditorPlacement::GetFallbackDistance(view->FromScene ? nullptr : &context.GetViewport().GetCamera());
		if (settings.Snap != io.KeyCtrl)
			pointer.Snap = settings.TranslateSnap;
		pointer.CanvasMode = canvasMode;
		preview.Update(context, pointer);
		if (!m_Drag->Delivered)
			return;

		// The drop: the same request the ghost was placed by, so it lands where the ghost stood.
		if (preview.GetKind() == PlacementPreview::Kind::Place)
		{
			EditorActions::PlaceAsset(panelContext.Runner, context, panelContext.Commands, preview.MakeDropParameters());
		}
		else if (preview.GetKind() == PlacementPreview::Kind::Material && preview.GetTarget())
		{
			// Onto the mesh under the pointer: its renderer's material (one undo step).
			RunEditorCommand(context, panelContext.Commands, "component.set", { { "entity", UUIDToJson(preview.GetTarget().GetUUID()) },
				{ "component", "MeshRenderer" }, { "values", { { "Material", UUIDToJson(m_Drag->Asset) } } } });
		}
		preview.Clear();
		m_Drag.reset();
	}

	void ViewportPanel::DrawPlacementLabel(EditorContext& context, const ImVec2& imageMin, const ImVec2& imageMax)
	{
		const PlacementPreview& preview = context.GetViewport().GetPlacementPreview();
		if (!preview.IsActive() || preview.GetLabel().empty())
			return;
		// The box's lines are made again only when the label changed.
		if (m_PlacementLabelVersion != preview.GetLabelVersion())
		{
			m_PlacementLabel.assign(1, preview.GetLabel());
			m_PlacementLabelVersion = preview.GetLabelVersion();
		}
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImVec4& color = preview.GetLabelStyle() == PlacementPreview::LabelStyle::Warning ? colors.Warning
			: (preview.GetLabelStyle() == PlacementPreview::LabelStyle::Quiet ? colors.TextSecondary : colors.Text);
		// Beside the pointer, kept inside the image.
		const ImVec2 padding = ImGui::GetStyle().FramePadding;
		const ImVec2 size(ImGui::CalcTextSize(preview.GetLabel().c_str()).x + padding.x * 2.0f, ImGui::GetTextLineHeightWithSpacing() + padding.y * 2.0f);
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		const float offset = ImGui::GetFontSize();
		const ImVec2 position(std::max(imageMin.x, std::min(mouse.x + offset, imageMax.x - size.x)),
			std::max(imageMin.y, std::min(mouse.y + offset, imageMax.y - size.y)));
		ViewportToolbar::DrawTextBox(ImGui::GetWindowDrawList(), position, m_PlacementLabel, color);
		UI::ItemProbe::RecordRect("Viewport.PlacementLabel", position, ImVec2(position.x + size.x, position.y + size.y), true, ImGui::GetColorU32(color),
			preview.GetLabel());
	}

}
