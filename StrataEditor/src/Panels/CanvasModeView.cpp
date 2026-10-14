#include "Panels/CanvasModeView.h"

#include "Editor/EditorContext.h"
#include "Editor/UIRectEdit.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"

#include <Strata/Core/Log.h>
#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/GameUI/GameUIScene.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <span>
#include <string>

namespace Strata
{

	namespace
	{

		// Outlines drawn per frame at most: a canvas of thousands of elements shows the first ones (the selection always).
		constexpr uint32_t c_MaxOutlines = 2000;
		// Handle squares and lines in font sizes, so they scale with the editor's UI.
		constexpr float c_HandleInFontSizes = 0.45f;
		constexpr float c_LineInFontSizes = 0.08f;

		std::optional<UICanvasView> FindView(GameUIScene& ui, Entity canvas, const UIScreen& screen)
		{
			for (const UICanvasView& view : ui.EnsureLayout(screen))
			{
				if (view.Canvas == canvas.GetHandle())
					return view;
			}
			return std::nullopt;
		}

	}

	bool CanvasModeView::IsActive(const EditorContext& context)
	{
		return context.GetViewport().GetSettings().CanvasMode && !context.IsPlaying();
	}

	UIScreen CanvasModeView::GetScreen(EditorContext& context, const ViewportImageArea& image)
	{
		return UIScreen { glm::vec2(image.GetPixelSize()), context.GetGameWindow().GetState().ContentScale };
	}

	Entity CanvasModeView::Layout(EditorContext& context, ViewportImageArea& image)
	{
		m_Canvas = CanvasEdit::FindShownCanvas(context);
		if (!m_Canvas || !(image.Size.x > 0.0f && image.Size.y > 0.0f))
			return m_Canvas;
		// The canvas's aspect ratio, centered with bars on the sides or above and below.
		const float available = image.Size.x / image.Size.y;
		const float aspect = CanvasEdit::GetCanvasAspect(m_Canvas, available);
		glm::vec2 size = image.Size;
		if (available > aspect)
			size.x = std::floor(image.Size.y * aspect);
		else
			size.y = std::floor(image.Size.x / aspect);
		image.Min += glm::floor((image.Size - size) * 0.5f);
		image.Size = glm::max(size, glm::vec2(1.0f));
		return m_Canvas;
	}

	void CanvasModeView::Reset(EditorContext& context)
	{
		if (m_Drag)
			m_Drag->End(context);
		m_Drag.reset();
	}

	void CanvasModeView::Update(EditorContext& context, const ViewportImageArea& image, bool hovered)
	{
		Scene& scene = *context.GetActiveScene();
		if (!m_Canvas || !scene.GetRegistry().valid(m_Canvas.GetHandle()) || !GameUIScene::HasCanvases(scene))
		{
			Reset(context);
			return;
		}
		const UIScreen screen = GetScreen(context, image);
		GameUIScene& ui = GameUIScene::Get(scene);
		const std::optional<UICanvasView> found = FindView(ui, m_Canvas, screen);
		if (!found)
		{
			Reset(context);
			return;
		}
		const UICanvasView view = *found;
		const ImGuiIO& io = ImGui::GetIO();
		// Canvas units <-> the panel's coordinates (ImGui units: pixels divided by the framebuffer scale).
		auto toScreen = [&](const glm::vec2& units) { const glm::vec2 point = image.Min + units * view.Scale / image.PixelScale; return ImVec2(point.x, point.y); };
		const glm::vec2 pointerPixels = (glm::vec2(io.MousePos.x, io.MousePos.y) - image.Min) * image.PixelScale;
		const glm::vec2 pointer = pointerPixels / view.Scale;
		const float handleSize = ImGui::GetFontSize() * c_HandleInFontSizes;
		const float line = std::max(1.0f, std::floor(ImGui::GetFontSize() * c_LineInFontSizes));

		// The selected element, when it is on this canvas.
		const Entity selected = context.GetPrimarySelection();
		std::optional<UIBox> selectedBox;
		UICanvasView selectedView;
		uint32_t selectedElement = 0;
		if (selected && ui.FindElement(selected.GetHandle(), screen, selectedView, selectedElement) && selectedView.CanvasIndex == view.CanvasIndex
			&& selectedElement != 0)
			selectedBox = ui.GetElementBox(view, selectedElement);

		const ViewportPickMode mode = io.KeyCtrl ? ViewportPickMode::Toggle : (io.KeyShift ? ViewportPickMode::Add : ViewportPickMode::Replace);
		// A drag follows the pointer until the button goes up. A press on the selection that did not move is a click: it
		// selects what is on top there (a button's label, say).
		if (m_Drag)
		{
			if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
			{
				const bool snap = context.GetViewport().GetSettings().Snap != io.KeyCtrl;
				std::string error;
				if (!m_Drag->Update(context, pointer, snap, &error))
				{
					ST_WARN("Canvas mode: {}", error);
					Reset(context);
				}
			}
			else
			{
				const bool click = m_PickOnRelease && glm::length(glm::vec2(io.MousePos.x, io.MousePos.y) - m_PressPosition) < handleSize * 0.25f;
				Reset(context);
				if (click)
					EditorViewport::ApplyPick(context, CanvasEdit::PickElement(scene, m_Canvas, screen, pointerPixels), mode);
			}
		}
		else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		{
			// A handle of the selection resizes it, a press on its rect moves it, a press elsewhere selects what is there.
			CanvasHandle grabbed = CanvasHandle::None;
			if (selectedBox)
			{
				for (const CanvasEdit::HandlePoint& handle : CanvasEdit::GetHandles(*selectedBox))
				{
					const ImVec2 center = toScreen(handle.Position);
					if (std::abs(io.MousePos.x - center.x) <= handleSize && std::abs(io.MousePos.y - center.y) <= handleSize)
					{
						grabbed = handle.Handle;
						break;
					}
				}
				if (grabbed == CanvasHandle::None && selectedBox->Contains(pointer))
					grabbed = CanvasHandle::Move;
			}
			if (grabbed != CanvasHandle::None)
			{
				std::string error;
				m_Drag = CanvasEdit::CanvasRectDrag::Begin(context, selected, grabbed, pointer, screen, &error);
				if (!m_Drag)
					ST_WARN("Canvas mode: {}", error);
				m_PickOnRelease = grabbed == CanvasHandle::Move;
				m_PressPosition = glm::vec2(io.MousePos.x, io.MousePos.y);
			}
			else
			{
				EditorViewport::ApplyPick(context, CanvasEdit::PickElement(scene, m_Canvas, screen, pointerPixels), mode);
			}
		}

		// Every element's outline, then the selection's rect, handles and pivot.
		const UI::ThemeColors& colors = UI::GetThemeColors();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		const ImU32 outline = ImGui::GetColorU32(UI::WithAlpha(colors.TextSecondary, 0.45f));
		const uint32_t count = std::min(ui.GetElementCount(view), c_MaxOutlines);
		for (uint32_t element = 1; element < count; element++)
		{
			if (!ui.IsElementVisible(view, element))
				continue;
			const UIBox box = ui.GetElementBox(view, element);
			drawList->AddRect(toScreen(box.Min), toScreen(box.GetMax()), outline);
		}
		if (!selectedBox)
			return;
		const UIBox box = *selectedBox;
		const ImU32 accent = ImGui::GetColorU32(colors.Accent);
		drawList->AddRect(toScreen(box.Min), toScreen(box.GetMax()), accent, 0.0f, line);
		UI::ItemProbe::RecordRect("Canvas.Selection", toScreen(box.Min), toScreen(box.GetMax()));
		for (const CanvasEdit::HandlePoint& handle : CanvasEdit::GetHandles(box))
		{
			const ImVec2 center = toScreen(handle.Position);
			const ImVec2 min(center.x - handleSize * 0.5f, center.y - handleSize * 0.5f);
			const ImVec2 max(center.x + handleSize * 0.5f, center.y + handleSize * 0.5f);
			drawList->AddRectFilled(min, max, ImGui::GetColorU32(colors.Text));
			drawList->AddRect(min, max, accent, 0.0f, line);
			UI::ItemProbe::RecordRect(std::string("Canvas.Handle.") + CanvasEdit::GetHandleName(handle.Handle), min, max);
		}
		const UIRectComponent* rect = selected.TryGetComponent<UIRectComponent>();
		if (rect)
		{
			const ImVec2 pivot = toScreen(box.Min + box.Size * rect->Pivot);
			drawList->AddCircle(pivot, handleSize * 0.5f, accent, 0, line);
		}
	}

}
