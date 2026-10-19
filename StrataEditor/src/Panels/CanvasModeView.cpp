#include "Panels/CanvasModeView.h"

#include "Editor/EditorContext.h"
#include "Editor/UIRectEdit.h"
#include "Editor/ViewportRenderer.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"

#include <Strata/Core/Log.h>
#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/GameUI/GameUIScene.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <cmath>
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

	UIScreen CanvasModeView::GetScreen(EditorContext& context) const
	{
		return UIScreen { glm::vec2(m_Frame.ScreenPixels), context.GetGameWindow().GetState().ContentScale };
	}

	Entity CanvasModeView::Layout(EditorContext& context, ViewportImageArea& image)
	{
		const Entity canvas = CanvasEdit::FindShownCanvas(context);
		// Another canvas is shown whole.
		if (canvas != m_Canvas)
			m_Framing.Reset();
		m_Canvas = canvas;
		m_Frame = {};
		// The image area is under the toolbar, so nothing covers the canvas at its fit.
		const UIBox panel { image.Min, image.Size };
		if (!m_Canvas || !(panel.Size.x > 0.0f && panel.Size.y > 0.0f))
			return {};
		const float aspect = CanvasEdit::GetCanvasAspect(m_Canvas, panel.Size.x / panel.Size.y);
		m_Frame = m_Framing.Compute(panel, aspect, image.PixelScale);
		if (m_Frame.IsEmpty())
			return {};
		image.Min = m_Frame.Visible.Min;
		image.Size = m_Frame.Visible.Size;
		return m_Canvas;
	}

	void CanvasModeView::ApplyView(ViewportView& view) const
	{
		if (m_Frame.IsEmpty())
			return;
		view.Camera = CanvasEdit::CropCamera(view.Camera, m_Frame.Display, m_Frame.Visible);
		view.ScreenView = CanvasEdit::GetScreenView(m_Frame);
	}

	void CanvasModeView::Reset(EditorContext& context)
	{
		if (m_Drag)
			m_Drag->End(context);
		m_Drag.reset();
		m_Panning = false;
	}

	void CanvasModeView::Update(EditorContext& context, bool hovered, bool keyboard)
	{
		Scene& scene = *context.GetActiveScene();
		if (!m_Canvas || m_Frame.IsEmpty() || !scene.GetRegistry().valid(m_Canvas.GetHandle()) || !GameUIScene::HasCanvases(scene))
		{
			Reset(context);
			return;
		}
		const UIScreen screen = GetScreen(context);
		GameUIScene& ui = GameUIScene::Get(scene);
		const std::optional<UICanvasView> found = FindView(ui, m_Canvas, screen);
		if (!found)
		{
			Reset(context);
			return;
		}
		const UICanvasView view = *found;
		const ImGuiIO& io = ImGui::GetIO();
		// Canvas units <-> the panel's coordinates, through the frame's zoom and pan.
		auto toScreen = [&](const glm::vec2& units) { const glm::vec2 point = m_Frame.ToPanel(units * view.Scale); return ImVec2(point.x, point.y); };
		const glm::vec2 pointerPixels = m_Frame.ToScreenPixels(glm::vec2(io.MousePos.x, io.MousePos.y));
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
		else if (hovered && !m_Panning && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
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

		// Every element's outline, then the selection's rect, handles and pivot, inside the panel's image area.
		const UI::ThemeColors& colors = UI::GetThemeColors();
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->PushClipRect(ImVec2(m_Frame.Panel.Min.x, m_Frame.Panel.Min.y), ImVec2(m_Frame.Panel.GetMax().x, m_Frame.Panel.GetMax().y), true);
		const ImU32 outline = ImGui::GetColorU32(UI::WithAlpha(colors.TextSecondary, 0.45f));
		const uint32_t count = std::min(ui.GetElementCount(view), c_MaxOutlines);
		for (uint32_t element = 1; element < count; element++)
		{
			if (!ui.IsElementVisible(view, element))
				continue;
			const UIBox box = ui.GetElementBox(view, element);
			drawList->AddRect(toScreen(box.Min), toScreen(box.GetMax()), outline);
		}
		if (selectedBox)
		{
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
			if (const UIRectComponent* rect = selected.TryGetComponent<UIRectComponent>())
			{
				const ImVec2 pivot = toScreen(box.Min + box.Size * rect->Pivot);
				drawList->AddCircle(pivot, handleSize * 0.5f, accent, 0, line);
			}
		}
		drawList->PopClipRect();
		UI::ItemProbe::RecordRect("Canvas.Panel", ImVec2(m_Frame.Panel.Min.x, m_Frame.Panel.Min.y), ImVec2(m_Frame.Panel.GetMax().x, m_Frame.Panel.GetMax().y));

		UpdateFraming(context, view, hovered, keyboard);
	}

	void CanvasModeView::UpdateFraming(EditorContext& context, const UICanvasView& view, bool hovered, bool keyboard)
	{
		const ImGuiIO& io = ImGui::GetIO();
		const glm::vec2 mouse(io.MousePos.x, io.MousePos.y);
		// The wheel zooms about the pointer, a middle drag pans; neither during a handle drag.
		if (hovered && !m_Drag && io.MouseWheel != 0.0f)
			m_Framing.ZoomAbout(m_Frame, mouse, std::pow(c_WheelZoomStep, io.MouseWheel));
		if (hovered && !m_Drag && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
			m_Panning = true;
		if (m_Panning)
		{
			if (ImGui::IsMouseDown(ImGuiMouseButton_Middle))
				m_Framing.Pan(m_Frame, glm::vec2(io.MouseDelta.x, io.MouseDelta.y));
			else
				m_Panning = false;
		}

		// F frames the selected element (the whole canvas without one on it), Home shows the whole canvas, like the
		// viewport's F and Home frame the selection and the scene.
		if (!keyboard || io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeySuper || io.KeyShift || m_Drag)
			return;
		if (ImGui::IsKeyPressed(ImGuiKey_Home, false))
			m_Framing.Reset();
		if (!ImGui::IsKeyPressed(ImGuiKey_F, false))
			return;
		Scene& scene = *context.GetActiveScene();
		GameUIScene& ui = GameUIScene::Get(scene);
		const Entity selected = context.GetPrimarySelection();
		UICanvasView selectedView;
		uint32_t element = 0;
		if (selected && ui.FindElement(selected.GetHandle(), GetScreen(context), selectedView, element) && selectedView.CanvasIndex == view.CanvasIndex
			&& element != 0 && view.UnitSize.x > 0.0f && view.UnitSize.y > 0.0f)
		{
			const UIBox box = ui.GetElementBox(view, element);
			m_Framing.FrameBox(m_Frame, UIBox { box.Min / view.UnitSize, box.Size / view.UnitSize });
		}
		else
		{
			m_Framing.Reset();
		}
	}

}
