#include "Panels/ViewportPanel.h"

#include "Editor/CommandJournal.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorPlacement.h"
#include "Editor/EditorViewport.h"
#include "Editor/SceneBounds.h"
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
#include <Strata/Core/Log.h>
#include <Strata/Input/Input.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/SceneRenderer.h>
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
		const ViewportToolbar::Layout layout = ViewportToolbar::Compute();
		const ImVec2 imageMin = layout.ImageMin;
		const ImVec2 available = layout.ImageSize;
		ImGui::SetCursorScreenPos(imageMin);
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
		AcceptAssetDrops(context, panelContext.Commands);

		// The editor camera, on the edited scene and on the running game alike (the Game view shows the game as it is); or,
		// with "Game camera", the scene's primary camera with the editor's overlays, which the editor camera's controls
		// leave alone.
		const float aspectRatio = static_cast<float>(size.x) / static_cast<float>(size.y);
		const auto resolveView = [&context, &settings, aspectRatio]()
		{
			if (settings.PilotGameCamera)
			{
				if (std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Scene, aspectRatio))
				{
					view->EditorOverlays = true;
					view->EditorView = true;
					return view;
				}
			}
			std::optional<ViewportView> view = ResolveViewportView(context, ViewportCameraSource::Editor, aspectRatio);
			if (view && settings.PilotGameCamera)
				view->Notice = "The scene has no primary camera: showing the editor camera";
			return view;
		};
		std::optional<ViewportView> view = resolveView();
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
				viewport.RequestPick(*pixel, size, mode);
		}
		// The camera may have moved this frame.
		view = resolveView();

		const UI::ThemeColors& colors = UI::GetThemeColors();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImVec2 imageMax(imageMin.x + available.x, imageMin.y + available.y);
		ViewportRenderer* renderer = viewport.GetRenderer();
		const bool rendered = renderer && view && renderer->Render(context, size, *view, settings, view->EditorOverlays);
		if (rendered)
		{
			drawList->AddImage(ViewportToolbar::ToTextureID(renderer->GetOutputTexture()), imageMin, imageMax);
		}
		else
		{
			const ImVec2 padding = ImGui::GetStyle().WindowPadding;
			drawList->AddRectFilled(imageMin, imageMax, ImGui::GetColorU32(colors.Chrome));
			drawList->AddText(ImVec2(imageMin.x + padding.x, imageMax.y - padding.y - ImGui::GetFontSize()), ImGui::GetColorU32(colors.TextSecondary),
				renderer ? "The scene could not be rendered (see the Console)" : "No GPU: the viewport cannot render");
		}
		ViewportToolbar::DrawBackground(drawList, layout, context);

		if (view)
		{
			DrawAgentFlash(context, *view);
			UpdateGizmo(context, *view);
		}

		// The chips, in the toolbar from its left; the agent's at its right.
		ImGui::PushClipRect(layout.Min, layout.Max, true);
		ImGui::SetCursorScreenPos(layout.ChipsMin);
		DrawChips(context);
		DrawPreviewChip(panelContext);
		DrawAgentChip(panelContext, layout.Max.x - style.ItemSpacing.x, layout.ChipsMin.y);
		ImGui::PopClipRect();
		if (rendered && settings.DebugView != ViewportDebugView::None)
			DrawDebugViewLegend(context);
		if (view)
			DrawOverlays(context, *view, panelContext.Frame);
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
		return m_CameraDrag != CameraDrag::None || m_GizmoDrag.has_value() || (!m_FlashBounds.empty() && IsFlashing(Now()));
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
			"Show the game's screen-space text (its HUD) in the editor view; the game and the scene camera always show it");
		ImGui::SameLine();
		DrawDebugViewChip(settings);
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

	void ViewportPanel::DrawAgentChip(EditorPanelContext& panelContext, float right, float top)
	{
		const AutomationActivity& activity = panelContext.Context.GetJournal().GetLatestAutomationActivity();
		if (activity.Id == 0)
			return;
		const double age = std::chrono::duration<double>(Now() - activity.Finished).count();
		const bool connected = panelContext.Shell && panelContext.Shell->GetAutomationState().Clients > 0;
		if (!connected && age >= c_AgentChipSeconds)
			return;

		// The text changes once a second at most: it is made again only then.
		const int64_t seconds = static_cast<int64_t>(age);
		if (activity.Id != m_AgentChipActivity || seconds != m_AgentChipSeconds)
		{
			m_AgentChipActivity = activity.Id;
			m_AgentChipSeconds = seconds;
			m_AgentChipText = fmt::format("Agent: {} \xC2\xB7 {}", activity.Command, UI::DescribeSecondsAgo(age));
		}
		const float width = UI::ChipWidth(Icons::Bot, m_AgentChipText.c_str());
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
		if (UI::ToggleChip("Viewport.DebugView", Icons::Layers, label, &clicked,
			"Debug views: color the scene by streaming state (meshes, materials and textures still loading or failed), by the level of detail "
			"meshes are drawn at, or by the mip levels textures lack against what the view needs"))
		{
			ImGui::OpenPopup("DebugView");
		}
		if (!ImGui::BeginPopup("DebugView"))
			return;
		const std::pair<ViewportDebugView, const char*> views[] = {
			{ ViewportDebugView::None, "Lit (no debug view)" },
			{ ViewportDebugView::Streaming, "Streaming: loading and failed assets" },
			{ ViewportDebugView::LOD, "LOD: level of detail drawn" },
			{ ViewportDebugView::Mip, "Mip: texture levels resident against needed" }
		};
		for (const auto& [view, name] : views)
		{
			if (ImGui::MenuItem(name, nullptr, settings.DebugView == view))
				settings.DebugView = view;
		}
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
				lines.push_back({ toColor(settings.DebugColors.LOD[level]), fmt::format("LOD {}{}", level, last ? "+" : ""), std::to_string(stats.EntitiesPerLOD[level]) });
			}
		}
		else
		{
			// Entities by the mip levels their worst texture lacks against what the view needs.
			constexpr std::array<const char*, 4> names = { "Mips as needed", "1 level short", "2 levels short", "3+ short, loading" };
			static_assert(names.size() == std::tuple_size_v<decltype(ViewportDebugColors::Mip)>);
			for (size_t shortfall = 0; shortfall < settings.DebugColors.Mip.size(); shortfall++)
				lines.push_back({ toColor(settings.DebugColors.Mip[shortfall]), names[shortfall], std::to_string(stats.EntitiesPerMipShortfall[shortfall]) });
		}
		const std::string note = settings.DebugView == ViewportDebugView::Streaming && stats.PlaceholderBoxes > 0
			? fmt::format("{} boxes: meshes not there yet", stats.PlaceholderBoxes) : std::string();

		// At the image's bottom left.
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		const ImGuiStyle& style = ImGui::GetStyle();
		UI::Legend("Viewport.Legend", ImVec2(m_Image.Min.x + style.ItemSpacing.x, m_Image.Min.y + m_Image.Size.y - style.ItemSpacing.y), lines, note);
		ImGui::PopFont();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Input
	////////////////////////////////////////////////////////////////////////////////

	void ViewportPanel::HandleShortcuts(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		const ImGuiIO& io = ImGui::GetIO();
		// The selection's shortcuts (Delete, Ctrl+D, Ctrl+C/X/V, Ctrl+A, F2) while the viewport has the keyboard focus, never
		// during a camera fly or a gizmo drag.
		if (m_CameraDrag != CameraDrag::Fly && !m_GizmoDrag && !ImGuizmo::IsUsing())
		{
			EditorActions::HandleSelectionShortcuts(context, panelContext.Commands, true);
			const Entity primary = context.GetPrimarySelection();
			if (primary && panelContext.Shell && context.AcceptsEditShortcuts() && ImGui::Shortcut(ImGuiKey_F2, ImGuiInputFlags_RouteFocused))
				panelContext.Shell->RenameEntity(primary.GetUUID());
		}

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
		std::string name = status.Scene.ToString();
		if (const Ref<AssetManagerBase>& assets = AssetManager::GetActive())
		{
			if (const std::optional<AssetMetadata> metadata = assets->GetMetadata(status.Scene))
				name = metadata->Path.empty() ? metadata->Name : metadata->Path;
		}
		// The load's progress counts the scene's document as it is read (SceneReadProgress), then what the scene uses by bytes:
		// the bar moves on in both steps, and the step says what the time goes to (last, the view's render proxies are built).
		const char* step = status.State == SceneLoadState::LoadingScene ? "Reading the scene"
			: status.State == SceneLoadState::Preparing ? "Preparing the view" : "Loading what it uses";
		const std::string stage = fmt::format("{} {:.0f}%", step, std::floor(status.Progress * 100.0f));

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
		UI::StrataProgressBar("Viewport.SceneLoad", status.Progress, stage, contentWidth);
	}

	void ViewportPanel::AcceptAssetDrops(EditorContext& context, const EditorCommandRegistry& commands)
	{
		if (!ImGui::BeginDragDropTarget())
			return;
		const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Asset);
		if (payload && payload->DataSize == static_cast<int>(sizeof(AssetHandle)) && m_Image.Size.x > 0.0f && m_Image.Size.y > 0.0f)
		{
			AssetHandle asset = UUID::Null();
			std::memcpy(&asset, payload->Data, sizeof(AssetHandle));
			const AssetType type = AssetManager::GetAssetType(asset);
			// The cursor in normalized device coordinates of the editor camera's view.
			const ImVec2 mouse = ImGui::GetIO().MousePos;
			const glm::vec2 ndc((mouse.x - m_Image.Min.x) / m_Image.Size.x * 2.0f - 1.0f, 1.0f - (mouse.y - m_Image.Min.y) / m_Image.Size.y * 2.0f);
			const float aspect = m_Image.Size.x / m_Image.Size.y;
			Scene& scene = *context.GetActiveScene();
			const EditorCamera& camera = context.GetViewport().GetCamera();
			if (type == AssetType::Prefab || type == AssetType::Model)
			{
				// Resting on the surface under the cursor, else on the one under the view's pivot (the game's camera when
				// piloting it: the editor camera's pivot there).
				if (m_Piloting)
				{
					EditorActions::InstantiatePrefab(context, commands, asset);
				}
				else
				{
					const EditorPlacement::Surface surface = EditorPlacement::FindSurfaceAt(scene, camera, ndc, aspect);
					EditorActions::InstantiatePrefab(context, commands, asset, UUID::Null(), &surface);
				}
			}
			else if (type == AssetType::Material)
			{
				// Onto the mesh under the cursor: its renderer's material (one undo step).
				const std::optional<SceneRayHit> hit = m_Piloting ? std::nullopt : EditorPlacement::CastAt(scene, camera, ndc, aspect);
				if (hit && hit->HitEntity.HasComponent<MeshRendererComponent>())
				{
					RunEditorCommand(context, commands, "component.set", { { "entity", UUIDToJson(hit->HitEntity.GetUUID()) }, { "component", "MeshRenderer" },
						{ "values", { { "Material", UUIDToJson(asset) } } } });
				}
				else
				{
					ST_WARN("Drop a material onto a mesh in the viewport to give it the material");
				}
			}
			else
			{
				ST_WARN("Only prefabs, models and materials can be dropped into the viewport ({} is a {})", asset.ToString(), AssetTypeToString(type));
			}
		}
		ImGui::EndDragDropTarget();
	}

}
