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
		if (canvas)
			return canvas;
		if (!GameUIScene::HasCanvases(scene))
			return {};
		const std::span<const UICanvasView> views = GameUIScene::Get(scene).EnsureLayout(UIRectEdit::GetGameScreen(context));
		return views.empty() ? Entity() : Entity(views.front().Canvas, &scene);
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
				if (ui.IsElementVisible(view, element) && ui.GetElementBox(view, element).Contains(point))
					return Entity(ui.GetElementEntity(view, element), &scene);
			}
		}
		return {};
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
