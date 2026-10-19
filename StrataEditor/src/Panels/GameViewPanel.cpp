#include "Panels/GameViewPanel.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorGameWindow.h"
#include "Editor/EditorViewport.h"
#include "Editor/ViewportRenderer.h"
#include "Panels/ViewportToolbar.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Input/Input.h>
#include <Strata/Renderer/GraphicsDevice.h>
#include <Strata/Renderer/Renderer.h>
#include <Strata/Scene/Scene.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace Strata
{

	namespace
	{

		// The resolution menu's custom fields, in text heights.
		constexpr float c_SizeFieldInFontSizes = 5.0f;

		std::string DescribeMode(const GameViewSettings& settings, const std::optional<glm::uvec2>& gameRequest)
		{
			switch (settings.Mode)
			{
				case GameViewMode::Free:           return "Free";
				case GameViewMode::Aspect16x9:     return "16:9";
				case GameViewMode::Aspect16x10:    return "16:10";
				case GameViewMode::Aspect4x3:      return "4:3";
				case GameViewMode::Aspect21x9:     return "21:9";
				case GameViewMode::Fixed1920x1080: return "1920 x 1080";
				case GameViewMode::Fixed1280x720:  return "1280 x 720";
				case GameViewMode::Custom:         return fmt::format("{} x {}", settings.Width, settings.Height);
				case GameViewMode::GameRequest:
					return gameRequest ? fmt::format("Game's {} x {}", gameRequest->x, gameRequest->y) : std::string("Game's (none: free)");
			}
			return "Free";
		}

	}

	EditorPanelWindowOptions GameViewPanel::GetWindowOptions(EditorPanelContext& context)
	{
		EditorPanelWindowOptions options;
		options.NoPadding = true;
		options.Flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		// While the game has the input, arrows, Space and Enter belong to it, not to keyboard navigation of the chips.
		if (m_GameInputEnabled && context.Context.IsGameInputActive())
			options.Flags |= ImGuiWindowFlags_NoNavInputs;
		return options;
	}

	void GameViewPanel::OnImGuiRender(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		EditorViewport& viewport = context.GetViewport();
		ViewportSettings& settings = viewport.GetSettings();
		m_Shown = true;
		m_Focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

		const ViewportToolbar::Layout layout = ViewportToolbar::Compute();
		const std::optional<glm::uvec2> gameRequest = context.GetGameWindow().GetRequestedResolution();
		const glm::vec2 pixelScale = ViewportImageArea::ChoosePixelScale(
			glm::vec2(ImGui::GetWindowViewport()->FramebufferScale.x, ImGui::GetWindowViewport()->FramebufferScale.y),
			glm::vec2(ImGui::GetIO().DisplayFramebufferScale.x, ImGui::GetIO().DisplayFramebufferScale.y));
		const GameViewFrame frame = ComputeGameViewFrame(settings.GameView, glm::vec2(layout.ImageSize.x, layout.ImageSize.y), pixelScale, gameRequest);
		// Fixed sizes beyond what the device can render shrink to it, keeping their shape.
		glm::uvec2 renderSize = frame.RenderSize;
		if (Renderer::IsInitialized())
		{
			const uint32_t limit = Renderer::GetGraphicsDevice().GetInfo().MaxTextureDimension2D;
			const uint32_t largest = std::max(renderSize.x, renderSize.y);
			if (largest > limit)
			{
				const double scale = static_cast<double>(limit) / static_cast<double>(largest);
				renderSize = glm::max(glm::uvec2(glm::dvec2(renderSize) * scale), glm::uvec2(1));
			}
		}
		m_Image.Min = glm::vec2(layout.ImageMin.x, layout.ImageMin.y) + frame.Offset;
		m_Image.Size = frame.Size;
		m_Image.PixelScale = frame.Size.x > 0.0f && frame.Size.y > 0.0f ? glm::vec2(renderSize) / frame.Size : pixelScale;
		m_RenderSize = renderSize;
		viewport.SetGameViewSize(renderSize);

		// The whole area takes the clicks (they focus the view, which hands the game its input).
		ImGui::SetCursorScreenPos(layout.ImageMin);
		if (layout.ImageSize.x > 0.0f && layout.ImageSize.y > 0.0f)
			ImGui::InvisibleButton("GameImage", layout.ImageSize, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
		const ImVec2 imageMin(m_Image.Min.x, m_Image.Min.y);
		const ImVec2 imageMax(m_Image.Min.x + m_Image.Size.x, m_Image.Min.y + m_Image.Size.y);
		UI::ItemProbe::RecordRect("GameView.Image", imageMin, imageMax);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const UI::ThemeColors& colors = UI::GetThemeColors();
		// The bars around a letterboxed image.
		drawList->AddRectFilled(layout.ImageMin, ImVec2(layout.ImageMin.x + layout.ImageSize.x, layout.ImageMin.y + layout.ImageSize.y),
			ImGui::GetColorU32(colors.Chrome));

		std::optional<ViewportView> view;
		if (renderSize.x > 0 && renderSize.y > 0)
			view = ResolveViewportView(context, ViewportCameraSource::Scene, static_cast<float>(renderSize.x) / static_cast<float>(renderSize.y));
		bool rendered = false;
		if (view)
		{
			ViewportRenderer* renderer = viewport.GetGameRenderer();
			rendered = renderer && renderer->Render(context, renderSize, *view, settings, false);
			if (rendered)
				drawList->AddImage(ViewportToolbar::ToTextureID(renderer->GetOutputTexture()), imageMin, imageMax);
			else
				drawList->AddText(ImVec2(imageMin.x + ImGui::GetStyle().WindowPadding.x, imageMax.y - ImGui::GetStyle().WindowPadding.y - ImGui::GetFontSize()),
					ImGui::GetColorU32(colors.TextSecondary), renderer ? "The scene could not be rendered (see the Console)" : "No GPU: the game view cannot render");
		}
		else
		{
			if (viewport.HasGameRenderer())
				viewport.GetGameRenderer()->ReleaseTargets();
			DrawNoCamera(context, drawList, layout.ImageMin, layout.ImageSize);
		}
		UpdateGameInput(context, true);

		ViewportToolbar::DrawBackground(drawList, layout, context);
		ImGui::PushClipRect(layout.Min, layout.Max, true);
		ImGui::SetCursorScreenPos(layout.ChipsMin);
		DrawResolutionChip(context);
		ImGui::SameLine();
		UI::ToggleChip("GameView.Stats", Icons::Gauge, "Stats", &settings.ShowStats, "Frame time, draw calls and loading assets");
		ImGui::PopClipRect();

		m_GameWindowNoticeShown = false;
		m_MouseHintShown = false;
		if (view)
		{
			DrawOverlays(context, drawList);
			if (rendered && settings.ShowStats)
			{
				const float margin = ImGui::GetStyle().ItemSpacing.x;
				ViewportToolbar::DrawStatsOverlay(drawList, ImVec2(imageMin.x + margin, imageMin.y + margin), *viewport.GetGameRenderer(), *view,
					panelContext.Frame);
			}
		}
	}

	void GameViewPanel::OnHidden(EditorPanelContext& context)
	{
		// Hidden behind another tab, collapsed or closed: nothing is rendered, no targets are kept, the game loses the input.
		Reset(context.Context);
	}

	void GameViewPanel::OnDetach(EditorPanelContext& context)
	{
		Reset(context.Context);
	}

	void GameViewPanel::Reset(EditorContext& context)
	{
		m_Shown = false;
		m_Focused = false;
		m_GameWindowNoticeShown = false;
		m_MouseHintShown = false;
		UpdateGameInput(context, false);
		EditorViewport& viewport = context.GetViewport();
		viewport.SetGameViewSize(glm::uvec2(0, 0));
		if (viewport.HasGameRenderer())
			viewport.GetGameRenderer()->ReleaseTargets();
	}

	void GameViewPanel::DrawResolutionChip(EditorContext& context)
	{
		GameViewSettings& gameView = context.GetViewport().GetSettings().GameView;
		const std::optional<glm::uvec2> gameRequest = context.GetGameWindow().GetRequestedResolution();
		const std::string label = DescribeMode(gameView, gameRequest);
		if (UI::Chip("GameView.Resolution", Icons::Monitor, label.c_str(), "The game view's image: the panel's size, an aspect ratio or a fixed resolution"))
		{
			m_CustomWidth = static_cast<int>(gameView.Width);
			m_CustomHeight = static_cast<int>(gameView.Height);
			ImGui::OpenPopup("GameViewResolution");
		}
		if (!ImGui::BeginPopup("GameViewResolution"))
			return;
		const std::pair<GameViewMode, const char*> modes[] = {
			{ GameViewMode::Free, "Free (fills the panel)" },
			{ GameViewMode::Aspect16x9, "16:9" },
			{ GameViewMode::Aspect16x10, "16:10" },
			{ GameViewMode::Aspect4x3, "4:3" },
			{ GameViewMode::Aspect21x9, "21:9" },
			{ GameViewMode::Fixed1920x1080, "1920 x 1080" },
			{ GameViewMode::Fixed1280x720, "1280 x 720 (exported games' window)" } };
		for (const auto& [mode, name] : modes)
		{
			if (mode == GameViewMode::Fixed1920x1080)
				ImGui::Separator();
			if (ImGui::MenuItem(name, nullptr, gameView.Mode == mode))
				gameView.Mode = mode;
		}
		// What the game asked for, while it asked (or while that mode is chosen).
		if (gameRequest || gameView.Mode == GameViewMode::GameRequest)
		{
			const std::string request = gameRequest ? fmt::format("Game's request ({} x {})", gameRequest->x, gameRequest->y) : std::string("Game's request (none)");
			if (ImGui::MenuItem(request.c_str(), nullptr, gameView.Mode == GameViewMode::GameRequest))
				gameView.Mode = GameViewMode::GameRequest;
		}
		ImGui::Separator();
		ImGui::TextUnformatted("Custom");
		const float fieldWidth = ImGui::GetFontSize() * c_SizeFieldInFontSizes;
		ImGui::SetNextItemWidth(fieldWidth);
		ImGui::InputInt("##CustomWidth", &m_CustomWidth, 0, 0);
		ImGui::SameLine();
		ImGui::TextUnformatted("x");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(fieldWidth);
		ImGui::InputInt("##CustomHeight", &m_CustomHeight, 0, 0);
		ImGui::SameLine();
		if (ImGui::Button("Use"))
		{
			const auto clamp = [](int value) { return static_cast<uint32_t>(std::clamp<int>(value, GameViewSettings::c_MinSize, GameViewSettings::c_MaxSize)); };
			gameView.Width = clamp(m_CustomWidth);
			gameView.Height = clamp(m_CustomHeight);
			gameView.Mode = GameViewMode::Custom;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void GameViewPanel::UpdateGameInput(EditorContext& context, bool shown)
	{
		// Shift+F1 leaves the view (and frees a cursor the game locked or hid) without stopping the game. Only asked while it
		// is shown: Reset runs this without an ImGui frame (when the editor detaches).
		if (shown && m_Focused && ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_F1, false))
		{
			ImGui::SetWindowFocus(nullptr);
			m_Focused = false;
		}

		// Scripts read Input: only the focused Game view feeds it, with positions in the image's pixels. A game that hid or
		// locked the cursor gives it back when it loses the input and takes it again when it gets the input back
		// (Input::SetGameCursorMode applies only while input is enabled).
		const bool enabled = shown && m_Focused && context.GetSceneState() == SceneState::Play;
		m_GameInputEnabled = enabled;
		context.SetGameInputActive(enabled);
		Input::SetEnabled(enabled);
		// A locked cursor stays inside the view: the editor's UI ignores the mouse meanwhile, so no click lands on another
		// panel (Shift+F1 leaves the view).
		const bool gameOwnsMouse = enabled && Input::GetGameCursorMode() == CursorMode::Locked;
		// Detaching runs this after the ImGui context is gone.
		if (gameOwnsMouse != m_GameOwnsMouse && ImGui::GetCurrentContext())
		{
			m_GameOwnsMouse = gameOwnsMouse;
			ImGuiIO& io = ImGui::GetIO();
			if (gameOwnsMouse)
				io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
			else
				io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
			// Without a pointer nothing on screen leads out: the view says how, for a while after each time it took it.
			if (gameOwnsMouse)
				m_MouseHintUntil = ImGui::GetTime() + c_MouseHintSeconds;
		}
		// The game's screen is the image the view shows, in every frame it shows play, focused or not: the game's window size,
		// UI hit tests and simulated mouse positions all read it (Input::GetViewportSize, at the rendered resolution).
		if (shown && context.GetSceneState() == SceneState::Play && m_Image.Size.x > 0.0f && m_Image.Size.y > 0.0f)
		{
			const ImVec2 origin = ImGui::GetMainViewport()->Pos;
			Input::SetViewport(glm::vec2(m_Image.Min.x - origin.x, m_Image.Min.y - origin.y), m_Image.Size, glm::vec2(m_RenderSize));
		}
	}

	void GameViewPanel::DrawOverlays(EditorContext& context, ImDrawList* drawList)
	{
		const float margin = ImGui::GetStyle().ItemSpacing.x;
		const UI::ThemeColors& colors = UI::GetThemeColors();
		// The game locked the cursor: the way out, at the top of the view.
		m_MouseHintShown = m_GameOwnsMouse && ImGui::GetTime() < m_MouseHintUntil;
		if (m_MouseHintShown)
		{
			const float width = ImGui::CalcTextSize(c_MouseHint).x + ImGui::GetStyle().FramePadding.x * 2.0f;
			ViewportToolbar::DrawTextBox(drawList, ImVec2(std::max(m_Image.Min.x + (m_Image.Size.x - width) * 0.5f, m_Image.Min.x), m_Image.Min.y + margin),
				{ c_MouseHint }, colors.Text);
		}
		DrawGameWindowNotice(context, drawList);
	}

	void GameViewPanel::DrawGameWindowNotice(EditorContext& context, ImDrawList* drawList)
	{
		const EditorGameWindow& gameWindow = context.GetGameWindow();
		m_GameWindowNoticeShown = context.IsPlaying() && gameWindow.HasRequests();
		if (!m_GameWindowNoticeShown)
			return;
		if (gameWindow.GetVersion() != m_GameWindowNoticeVersion)
		{
			m_GameWindowNotice = gameWindow.DescribeForPeople();
			m_GameWindowNoticeVersion = gameWindow.GetVersion();
		}
		const ImVec2 padding = ImGui::GetStyle().FramePadding;
		const float margin = ImGui::GetStyle().ItemSpacing.x;
		const ImVec2 textSize = ImGui::CalcTextSize(m_GameWindowNotice.c_str());
		const float x = std::max(m_Image.Min.x + (m_Image.Size.x - textSize.x - padding.x * 2.0f) * 0.5f, m_Image.Min.x);
		const float y = m_Image.Min.y + m_Image.Size.y - textSize.y - padding.y * 2.0f - margin;
		ViewportToolbar::DrawTextBox(drawList, ImVec2(x, y), { m_GameWindowNotice }, UI::GetThemeColors().TextSecondary);
	}

	void GameViewPanel::DrawNoCamera(EditorContext& context, ImDrawList* drawList, const ImVec2& areaMin, const ImVec2& areaSize)
	{
		// What an exported game shows then (GameRenderer), centered.
		const std::string text = fmt::format("No camera to show\n\nThe scene '{}' has no active entity\nwith a Camera component marked Primary.",
			context.GetActiveScene()->GetName());
		const ImVec2 size = ImGui::CalcTextSize(text.c_str());
		const ImVec2 position(areaMin.x + std::max((areaSize.x - size.x) * 0.5f, 0.0f), areaMin.y + std::max((areaSize.y - size.y) * 0.5f, 0.0f));
		drawList->AddText(position, ImGui::GetColorU32(UI::GetThemeColors().TextSecondary), text.c_str());
		UI::ItemProbe::RecordRect("GameView.NoCamera", position, ImVec2(position.x + size.x, position.y + size.y), true, 0, text);
	}

}
