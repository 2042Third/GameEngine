#include "Editor/CanvasEdit.h"

#include "Editor/EditorContext.h"
#include "Editor/GameUIPresets.h"
#include "Editor/SceneEdit.h"
#include "Editor/UIRectEdit.h"

#include <Strata/Core/Log.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <atomic>
#include <cmath>

namespace Strata::CanvasEdit
{

	namespace
	{

		bool FitsContent(const UIRectComponent& rect, int axis)
		{
			return rect.FitContent == UIFitContent::Both || (axis == 0 ? rect.FitContent == UIFitContent::Width : rect.FitContent == UIFitContent::Height);
		}

		bool MovesLeft(CanvasHandle handle) { return handle == CanvasHandle::Left || handle == CanvasHandle::TopLeft || handle == CanvasHandle::BottomLeft; }
		bool MovesRight(CanvasHandle handle) { return handle == CanvasHandle::Right || handle == CanvasHandle::TopRight || handle == CanvasHandle::BottomRight; }
		bool MovesTop(CanvasHandle handle) { return handle == CanvasHandle::Top || handle == CanvasHandle::TopLeft || handle == CanvasHandle::TopRight; }
		bool MovesBottom(CanvasHandle handle) { return handle == CanvasHandle::Bottom || handle == CanvasHandle::BottomLeft || handle == CanvasHandle::BottomRight; }

		// Drags get merge keys of their own, so two drags of one element are two undo steps.
		std::atomic<uint64_t> s_NextDrag = 1;

	}

	const char* GetHandleName(CanvasHandle handle)
	{
		switch (handle)
		{
			case CanvasHandle::None: return "None";
			case CanvasHandle::Move: return "Move";
			case CanvasHandle::Left: return "Left";
			case CanvasHandle::Right: return "Right";
			case CanvasHandle::Top: return "Top";
			case CanvasHandle::Bottom: return "Bottom";
			case CanvasHandle::TopLeft: return "TopLeft";
			case CanvasHandle::TopRight: return "TopRight";
			case CanvasHandle::BottomLeft: return "BottomLeft";
			case CanvasHandle::BottomRight: return "BottomRight";
		}
		return "None";
	}

	std::vector<HandlePoint> GetHandles(const UIBox& box)
	{
		const glm::vec2 min = box.Min;
		const glm::vec2 max = box.GetMax();
		const glm::vec2 center = box.GetCenter();
		return {
			{ CanvasHandle::TopLeft, { min.x, min.y } }, { CanvasHandle::Top, { center.x, min.y } }, { CanvasHandle::TopRight, { max.x, min.y } },
			{ CanvasHandle::Left, { min.x, center.y } }, { CanvasHandle::Right, { max.x, center.y } },
			{ CanvasHandle::BottomLeft, { min.x, max.y } }, { CanvasHandle::Bottom, { center.x, max.y } }, { CanvasHandle::BottomRight, { max.x, max.y } } };
	}

	UIBox DragBox(const UIBox& box, CanvasHandle handle, const glm::vec2& delta, bool snap)
	{
		auto snapped = [snap](float value) { return snap ? std::round(value) : value; };
		if (handle == CanvasHandle::Move)
			return UIBox { { snapped(box.Min.x + delta.x), snapped(box.Min.y + delta.y) }, box.Size };
		glm::vec2 min = box.Min;
		glm::vec2 max = box.GetMax();
		if (MovesLeft(handle))
			min.x = std::min(snapped(min.x + delta.x), max.x);
		if (MovesRight(handle))
			max.x = std::max(snapped(max.x + delta.x), min.x);
		if (MovesTop(handle))
			min.y = std::min(snapped(min.y + delta.y), max.y);
		if (MovesBottom(handle))
			max.y = std::max(snapped(max.y + delta.y), min.y);
		return UIBox { min, max - min };
	}

	UIRectComponent PlaceRect(const UIRectComponent& rect, const UIBox& box, const UIBox& parent)
	{
		UIRectComponent result = rect;
		for (int axis = 0; axis < 2; axis++)
		{
			const float start = parent.Min[axis] + rect.AnchorMin[axis] * parent.Size[axis];
			const float span = (rect.AnchorMax[axis] - rect.AnchorMin[axis]) * parent.Size[axis];
			// size = span + Size, and the pivot point sits Position away from the anchors' pivot point.
			if (!FitsContent(rect, axis))
				result.Size[axis] = box.Size[axis] - span;
			result.Position[axis] = box.Min[axis] + box.Size[axis] * rect.Pivot[axis] - (start + span * rect.Pivot[axis]);
		}
		return result;
	}

	Entity FindShownCanvas(EditorContext& context)
	{
		Scene& scene = *context.GetActiveScene();
		// The selection's root canvas: a canvas inside another is an element of the outer one.
		Entity canvas = GameUIPresets::FindCanvas(context.GetPrimarySelection());
		while (canvas)
		{
			const Entity outer = GameUIPresets::FindCanvas(canvas.GetParent());
			if (!outer)
				break;
			canvas = outer;
		}
		// The mode lays canvases out for a screen: a canvas placed in the scene is edited there instead.
		auto onScreen = [](Entity entity)
		{
			const UICanvasComponent* settings = entity ? entity.TryGetComponent<UICanvasComponent>() : nullptr;
			return settings && settings->RenderMode == UICanvasRenderMode::ScreenSpace;
		};
		if (onScreen(canvas))
			return canvas;
		if (!GameUIScene::HasCanvases(scene))
			return {};
		for (const UICanvasView& view : GameUIScene::Get(scene).EnsureLayout(UIRectEdit::GetGameScreen(context)))
		{
			if (!view.WorldSpace)
				return Entity(view.Canvas, &scene);
		}
		return {};
	}

	float GetCanvasAspect(Entity canvas, float fallback)
	{
		const UICanvasComponent* component = canvas ? canvas.TryGetComponent<UICanvasComponent>() : nullptr;
		if (component && component->ScaleMode == UICanvasScaleMode::ScaleWithScreen && component->ReferenceResolution.x > 0.0f && component->ReferenceResolution.y > 0.0f)
			return component->ReferenceResolution.x / component->ReferenceResolution.y;
		return fallback;
	}

	Entity PickElement(Scene& scene, Entity canvas, const UIScreen& screen, const glm::vec2& pixel)
	{
		if (!canvas || !GameUIScene::HasCanvases(scene))
			return {};
		GameUIScene& ui = GameUIScene::Get(scene);
		for (const UICanvasView& view : ui.EnsureLayout(screen))
		{
			if (view.Canvas != canvas.GetHandle())
				continue;
			const glm::vec2 point = pixel / view.Scale;
			for (uint32_t element = ui.GetElementCount(view); element-- > 1;)
			{
				if (ui.IsElementVisible(view, element) && ui.GetVisibleBox(view, element).Contains(point))
					return Entity(ui.GetElementEntity(view, element), &scene);
			}
		}
		return {};
	}

	////////////////////////////////////////////////////////////////////////////////
	// CanvasFraming
	////////////////////////////////////////////////////////////////////////////////

	namespace
	{

		// Keeps c_KeepVisible of the panel (or the whole display, when it is smaller) showing the display on each axis.
		glm::vec2 ClampDisplayMin(const UIBox& panel, const glm::vec2& displayMin, const glm::vec2& displaySize)
		{
			glm::vec2 result = displayMin;
			for (int axis = 0; axis < 2; axis++)
			{
				const float keep = std::min(displaySize[axis], panel.Size[axis] * CanvasFraming::c_KeepVisible);
				result[axis] = std::clamp(displayMin[axis], panel.Min[axis] + keep - displaySize[axis], panel.Min[axis] + panel.Size[axis] - keep);
			}
			return result;
		}

	}

	glm::vec2 CanvasFraming::Frame::ToScreenPixels(const glm::vec2& panelPoint) const
	{
		return (panelPoint - Display.Min) * PixelScale / Zoom;
	}

	glm::vec2 CanvasFraming::Frame::ToPanel(const glm::vec2& screenPixels) const
	{
		return Display.Min + screenPixels * Zoom / PixelScale;
	}

	CanvasFraming::Frame CanvasFraming::Compute(const UIBox& panel, float aspect, const glm::vec2& pixelScale) const
	{
		Frame frame;
		frame.Panel = panel;
		const bool validScale = pixelScale.x > 0.0f && pixelScale.y > 0.0f && std::isfinite(pixelScale.x) && std::isfinite(pixelScale.y);
		frame.PixelScale = validScale ? pixelScale : glm::vec2(1.0f);
		frame.Zoom = m_Zoom;
		if (!(panel.Size.x >= 1.0f && panel.Size.y >= 1.0f) || !(aspect > 0.0f) || !std::isfinite(aspect))
			return frame;

		// The fit: the canvas's aspect ratio in whole units, centered with bars on the sides or above and below.
		const float available = panel.Size.x / panel.Size.y;
		glm::vec2 size = panel.Size;
		if (available > aspect)
			size.x = std::floor(panel.Size.y * aspect);
		else
			size.y = std::floor(panel.Size.x / aspect);
		size = glm::max(size, glm::vec2(1.0f));
		frame.Fit = UIBox { panel.Min + glm::floor((panel.Size - size) * 0.5f), size };
		frame.ScreenPixels = glm::uvec2(size * frame.PixelScale);
		if (frame.ScreenPixels.x == 0 || frame.ScreenPixels.y == 0)
			return frame;

		// Zoomed about the fit's center, then panned (and kept partly in the panel).
		const glm::vec2 displaySize = glm::vec2(frame.ScreenPixels) * m_Zoom / frame.PixelScale;
		const glm::vec2 displayMin = frame.Fit.Min + (frame.Fit.Size - displaySize) * 0.5f + m_Pan * frame.Fit.Size;
		frame.Display = UIBox { ClampDisplayMin(panel, displayMin, displaySize), displaySize };

		// The visible part, on whole pixels: the image the viewport renders.
		const glm::vec2 visibleMin = glm::round(glm::max(frame.Display.Min, panel.Min) * frame.PixelScale) / frame.PixelScale;
		const glm::vec2 visibleMax = glm::round(glm::min(frame.Display.GetMax(), panel.GetMax()) * frame.PixelScale) / frame.PixelScale;
		frame.Visible = UIBox { visibleMin, glm::max(visibleMax - visibleMin, glm::vec2(0.0f)) };
		return frame;
	}

	void CanvasFraming::SetDisplayMin(const Frame& frame, const glm::vec2& displayMin, const glm::vec2& displaySize)
	{
		const glm::vec2 clamped = ClampDisplayMin(frame.Panel, displayMin, displaySize);
		m_Pan = (clamped - frame.Fit.Min - (frame.Fit.Size - displaySize) * 0.5f) / frame.Fit.Size;
	}

	void CanvasFraming::ZoomAbout(const Frame& frame, const glm::vec2& point, float factor)
	{
		if (frame.IsEmpty() || !(factor > 0.0f) || !std::isfinite(factor))
			return;
		const float zoom = std::clamp(m_Zoom * factor, c_MinZoom, c_MaxZoom);
		if (zoom == m_Zoom)
			return;
		// The canvas point under `point` stays there.
		const glm::vec2 fraction = (point - frame.Display.Min) / frame.Display.Size;
		const glm::vec2 displaySize = glm::vec2(frame.ScreenPixels) * zoom / frame.PixelScale;
		m_Zoom = zoom;
		SetDisplayMin(frame, point - fraction * displaySize, displaySize);
	}

	void CanvasFraming::Pan(const Frame& frame, const glm::vec2& delta)
	{
		if (frame.IsEmpty() || !std::isfinite(delta.x) || !std::isfinite(delta.y))
			return;
		SetDisplayMin(frame, frame.Display.Min + delta, frame.Display.Size);
	}

	void CanvasFraming::FrameBox(const Frame& frame, const UIBox& fraction)
	{
		if (frame.IsEmpty())
			return;
		// The zoom that makes the box fill c_FrameFill of the panel along its tighter axis (an empty box keeps the zoom).
		const glm::vec2 fitSize = glm::vec2(frame.ScreenPixels) / frame.PixelScale;
		float zoom = c_MaxZoom;
		bool sized = false;
		for (int axis = 0; axis < 2; axis++)
		{
			if (fraction.Size[axis] > 0.0f)
			{
				zoom = std::min(zoom, c_FrameFill * frame.Panel.Size[axis] / (fitSize[axis] * fraction.Size[axis]));
				sized = true;
			}
		}
		m_Zoom = sized && std::isfinite(zoom) ? std::clamp(zoom, c_MinZoom, c_MaxZoom) : m_Zoom;
		const glm::vec2 displaySize = fitSize * m_Zoom;
		SetDisplayMin(frame, frame.Panel.GetCenter() - fraction.GetCenter() * displaySize, displaySize);
	}

	void CanvasFraming::Reset()
	{
		m_Zoom = 1.0f;
		m_Pan = glm::vec2(0.0f);
	}

	SceneCamera CropCamera(const SceneCamera& camera, const UIBox& display, const UIBox& visible)
	{
		if (!(display.Size.x > 0.0f && display.Size.y > 0.0f && visible.Size.x > 0.0f && visible.Size.y > 0.0f))
			return camera;
		// The visible part in the display's normalized device coordinates (+Y up).
		const glm::vec2 start = (visible.Min - display.Min) / display.Size;
		const glm::vec2 end = (visible.GetMax() - display.Min) / display.Size;
		const float left = start.x * 2.0f - 1.0f;
		const float right = end.x * 2.0f - 1.0f;
		const float top = 1.0f - start.y * 2.0f;
		const float bottom = 1.0f - end.y * 2.0f;
		if (!(right > left) || !(top > bottom))
			return camera;

		// Clip x' = sx * x + tx * w maps [left, right] onto [-1, 1] after the division by w (likewise y): the same pixels,
		// magnified, for perspective and orthographic projections alike.
		glm::mat4 crop(1.0f);
		crop[0][0] = 2.0f / (right - left);
		crop[3][0] = -(right + left) / (right - left);
		crop[1][1] = 2.0f / (top - bottom);
		crop[3][1] = -(top + bottom) / (top - bottom);
		SceneCamera result = camera;
		result.Projection = crop * camera.Projection;

		// The symmetric frustum (at the cropped image's aspect ratio) that holds the crop, in units of the full view's half
		// height.
		const float displayAspect = display.Size.x / display.Size.y;
		const float visibleAspect = visible.Size.x / visible.Size.y;
		const float halfHeight = std::max(std::max(std::abs(top), std::abs(bottom)), displayAspect * std::max(std::abs(left), std::abs(right)) / visibleAspect);
		if (camera.Orthographic)
			result.VerticalFOV = camera.VerticalFOV * halfHeight;
		else
			result.VerticalFOV = 2.0f * std::atan(std::tan(camera.VerticalFOV * 0.5f) * halfHeight);
		return result;
	}

	SceneScreenView GetScreenView(const CanvasFraming::Frame& frame)
	{
		SceneScreenView view;
		view.ScreenSize = glm::vec2(frame.ScreenPixels);
		view.Zoom = frame.Zoom;
		view.Offset = (frame.Display.Min - frame.Visible.Min) * frame.PixelScale;
		return view;
	}

	std::optional<CanvasRectDrag> CanvasRectDrag::Begin(EditorContext& context, Entity element, CanvasHandle handle, const glm::vec2& pointer,
		const UIScreen& screen, std::string* outError)
	{
		if (!context.CheckSceneEditable(outError))
			return std::nullopt;
		const std::optional<UIRectEdit::Placement> placement = element ? UIRectEdit::FindPlacement(*element.GetScene(), element, screen) : std::nullopt;
		if (!placement || handle == CanvasHandle::None)
		{
			if (outError)
				*outError = element ? fmt::format("'{}' is no element laid out on a UI canvas", element.GetName()) : std::string("No element");
			return std::nullopt;
		}
		CanvasRectDrag drag;
		drag.m_Element = element.GetUUID();
		drag.m_Handle = handle;
		drag.m_Start = pointer;
		drag.m_StartBox = placement->Box;
		drag.m_Parent = placement->Parent;
		drag.m_MergeKey = fmt::format("CanvasDrag/{}/{}", element.GetUUID().ToString(), s_NextDrag.fetch_add(1));
		return drag;
	}

	bool CanvasRectDrag::Update(EditorContext& context, const glm::vec2& pointer, bool snap, std::string* outError)
	{
		Scene& scene = *context.GetActiveScene();
		Entity element = scene.GetEntityByUUID(m_Element);
		if (!element || !element.HasComponent<UIRectComponent>())
		{
			if (outError)
				*outError = "The element is gone";
			return false;
		}
		if (!context.CheckSceneEditable(outError))
			return false;
		const UIBox box = DragBox(m_StartBox, m_Handle, pointer - m_Start, snap);
		const UIRectComponent rect = PlaceRect(element.GetComponent<UIRectComponent>(), box, m_Parent);
		if (rect.Position == element.GetComponent<UIRectComponent>().Position && rect.Size == element.GetComponent<UIRectComponent>().Size)
			return true;
		const char* action = m_Handle == CanvasHandle::Move ? "Move" : "Resize";
		SceneEditTransaction transaction(scene, fmt::format("{} {}", action, SceneEdit::QuoteName(element.GetName())), { m_Element }, m_MergeKey);
		element.GetComponent<UIRectComponent>() = rect;
		element.MarkModified<UIRectComponent>();
		context.CommitEdit(transaction);
		return true;
	}

	void CanvasRectDrag::End(EditorContext& context)
	{
		context.GetUndoStack().BreakMerge();
	}

}
