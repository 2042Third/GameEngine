#include "Editor/UIRectEdit.h"

#include "Editor/EditorContext.h"
#include "Editor/SceneEdit.h"
#include "Editor/SceneRaycast.h"

#include <Strata/Core/Log.h>
#include <Strata/GameUI/GameUIHitTest.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace Strata::UIRectEdit
{

	namespace
	{

		constexpr std::array<const char*, c_UIAnchorPresetCount> c_PresetNames = {
			"topLeft", "topCenter", "topRight", "topStretch",
			"middleLeft", "middleCenter", "middleRight", "middleStretch",
			"bottomLeft", "bottomCenter", "bottomRight", "bottomStretch",
			"stretchLeft", "stretchCenter", "stretchRight", "stretch"
		};
		// As people read them (the Inspector).
		constexpr std::array<const char*, c_UIAnchorPresetCount> c_PresetDisplayNames = {
			"Top Left", "Top Center", "Top Right", "Top Stretch",
			"Middle Left", "Middle Center", "Middle Right", "Middle Stretch",
			"Bottom Left", "Bottom Center", "Bottom Right", "Bottom Stretch",
			"Stretch Left", "Stretch Center", "Stretch Right", "Stretch"
		};

		// Where a row (vertical) or column (horizontal) puts the anchors: start, center, end, or both ends (stretch).
		struct AxisAnchors
		{
			float Min = 0.5f;
			float Max = 0.5f;
			float Pivot = 0.5f;
		};

		AxisAnchors GetAxisAnchors(uint32_t index)
		{
			switch (index)
			{
				case 0: return { 0.0f, 0.0f, 0.0f };
				case 1: return { 0.5f, 0.5f, 0.5f };
				case 2: return { 1.0f, 1.0f, 1.0f };
				default: return { 0.0f, 1.0f, 0.5f };
			}
		}

		// The parent's points the anchors stand for along an axis.
		void GetAnchorPoints(const UIRectComponent& rect, const UIBox& parent, int axis, float& outStart, float& outEnd)
		{
			outStart = parent.Min[axis] + rect.AnchorMin[axis] * parent.Size[axis];
			outEnd = parent.Min[axis] + rect.AnchorMax[axis] * parent.Size[axis];
		}

		bool FitsContent(const UIRectComponent& rect, int axis)
		{
			return rect.FitContent == UIFitContent::Both || (axis == 0 ? rect.FitContent == UIFitContent::Width : rect.FitContent == UIFitContent::Height);
		}

	}

	const char* GetPresetName(UIAnchorPreset preset)
	{
		const uint32_t index = static_cast<uint32_t>(preset);
		return index < c_UIAnchorPresetCount ? c_PresetNames[index] : "middleCenter";
	}

	const char* GetPresetDisplayName(UIAnchorPreset preset)
	{
		const uint32_t index = static_cast<uint32_t>(preset);
		return index < c_UIAnchorPresetCount ? c_PresetDisplayNames[index] : "Middle Center";
	}

	std::optional<UIAnchorPreset> FindPreset(std::string_view name)
	{
		for (uint32_t index = 0; index < c_UIAnchorPresetCount; index++)
		{
			if (name == c_PresetNames[index])
				return static_cast<UIAnchorPreset>(index);
		}
		return std::nullopt;
	}

	std::optional<UIAnchorPreset> FindPreset(const UIRectComponent& rect)
	{
		for (uint32_t index = 0; index < c_UIAnchorPresetCount; index++)
		{
			const Anchors anchors = GetAnchors(static_cast<UIAnchorPreset>(index));
			if (anchors.Min == rect.AnchorMin && anchors.Max == rect.AnchorMax)
				return static_cast<UIAnchorPreset>(index);
		}
		return std::nullopt;
	}

	Anchors GetAnchors(UIAnchorPreset preset)
	{
		const uint32_t index = static_cast<uint32_t>(preset);
		// Rows are vertical (top, middle, bottom, stretch), columns horizontal (left, center, right, stretch).
		const AxisAnchors horizontal = GetAxisAnchors(index % 4);
		const AxisAnchors vertical = GetAxisAnchors(index / 4);
		Anchors anchors;
		anchors.Min = { horizontal.Min, vertical.Min };
		anchors.Max = { horizontal.Max, vertical.Max };
		anchors.Pivot = { horizontal.Pivot, vertical.Pivot };
		return anchors;
	}

	bool IsStretched(const UIRectComponent& rect, int axis)
	{
		return rect.AnchorMin[axis] != rect.AnchorMax[axis];
	}

	UIRectComponent ApplyPreset(const UIRectComponent& rect, const UIBox& box, const UIBox& parent, UIAnchorPreset preset, bool setPivot, bool snapToAnchors)
	{
		const Anchors anchors = GetAnchors(preset);
		UIRectComponent result = rect;
		result.AnchorMin = anchors.Min;
		result.AnchorMax = anchors.Max;
		if (setPivot)
			result.Pivot = anchors.Pivot;
		for (int axis = 0; axis < 2; axis++)
		{
			float start = 0.0f;
			float end = 0.0f;
			GetAnchorPoints(result, parent, axis, start, end);
			const float span = end - start;
			if (snapToAnchors)
			{
				// Onto the anchors: a point keeps its size, a stretched axis fills the anchors' span.
				result.Position[axis] = 0.0f;
				if (!FitsContent(result, axis))
					result.Size[axis] = IsStretched(result, axis) ? 0.0f : box.Size[axis];
				continue;
			}
			// size = span + Size, and the pivot point is where it was.
			if (!FitsContent(result, axis))
				result.Size[axis] = box.Size[axis] - span;
			const float pivotPoint = box.Min[axis] + box.Size[axis] * result.Pivot[axis];
			result.Position[axis] = pivotPoint - (start + span * result.Pivot[axis]);
		}
		return result;
	}

	glm::vec2 GetMargins(const UIRectComponent& rect, const UIBox& box, const UIBox& parent, int axis)
	{
		float start = 0.0f;
		float end = 0.0f;
		GetAnchorPoints(rect, parent, axis, start, end);
		return { box.Min[axis] - start, end - (box.Min[axis] + box.Size[axis]) };
	}

	UIRectComponent SetMargins(const UIRectComponent& rect, const UIBox& parent, int axis, const glm::vec2& margins)
	{
		float start = 0.0f;
		float end = 0.0f;
		GetAnchorPoints(rect, parent, axis, start, end);
		UIRectComponent result = rect;
		// The box from start + left to end - right: size = span - left - right, so Size = -(left + right); the pivot point
		// start + left + size * pivot sits Position away from the anchors' pivot point start + span * pivot.
		result.Size[axis] = -(margins.x + margins.y);
		result.Position[axis] = margins.x * (1.0f - rect.Pivot[axis]) - margins.y * rect.Pivot[axis];
		return result;
	}

	UIScreen GetGameScreen(EditorContext& context)
	{
		if (context.IsPlaying())
			return GameUIScene::GetInputScreen().Screen;
		const glm::uvec2 viewport = context.GetViewport().GetSize();
		if (viewport.x > 0 && viewport.y > 0)
			return UIScreen { glm::vec2(viewport), context.GetGameWindow().GetState().ContentScale };
		return GameUIScene::c_DefaultScreen;
	}

	std::optional<Placement> FindPlacement(Scene& scene, Entity entity, const UIScreen& screen)
	{
		if (!entity || !GameUIScene::HasCanvases(scene))
			return std::nullopt;
		GameUIScene& ui = GameUIScene::Get(scene);
		ui.EnsureLayout(screen); // Changes since the latest layout included
		UICanvasView view;
		uint32_t element = 0;
		if (!ui.FindElement(entity.GetHandle(), screen, view, element) || element == 0)
			return std::nullopt;
		Placement placement;
		placement.Box = ui.GetElementBox(view, element);
		placement.Scale = view.Scale;
		placement.WorldSpace = view.WorldSpace;
		for (Entity parent = entity.GetParent(); parent; parent = parent.GetParent())
		{
			UICanvasView parentView;
			uint32_t parentElement = 0;
			if (ui.FindElement(parent.GetHandle(), screen, parentView, parentElement) && parentView.CanvasIndex == view.CanvasIndex)
			{
				placement.Parent = ui.GetElementBox(parentView, parentElement);
				return placement;
			}
		}
		return std::nullopt;
	}

	Entity PickElement(Scene& scene, const UIScreen& screen, const glm::vec2& pixel, bool screenCanvases, const glm::mat4* viewProjection)
	{
		if (!GameUIScene::HasCanvases(scene))
			return {};
		GameUIScene& ui = GameUIScene::Get(scene);
		const std::span<const UICanvasView> laidOut = ui.EnsureLayout(screen, screenCanvases);
		const std::vector<UICanvasView> views(laidOut.begin(), laidOut.end());
		const entt::registry& registry = scene.GetRegistry();
		auto pick = [&](const UICanvasView& view, const glm::vec2& point) -> Entity
		{
			for (uint32_t element = ui.GetElementCount(view); element-- > 1;)
			{
				const entt::entity entity = ui.GetElementEntity(view, element);
				if (!registry.any_of<UIImageComponent, UITextComponent>(entity) || !ui.IsElementVisible(view, element))
					continue;
				if (ui.GetVisibleBox(view, element).Contains(point))
					return Entity(entity, &scene);
			}
			return {};
		};
		// The screen's canvases are in front of the scene's.
		for (size_t index = views.size(); index-- > 0;)
		{
			if (views[index].WorldSpace)
				continue;
			if (const Entity picked = pick(views[index], pixel / views[index].Scale))
				return picked;
		}
		// The canvases in the scene where the view's ray through the pixel crosses them, nearest first.
		const std::optional<UIPointerRay> ray = viewProjection ? GameUIPointer::MakeRay(*viewProjection, screen.Size, pixel) : std::nullopt;
		if (!ray)
			return {};
		std::vector<std::pair<float, std::pair<size_t, glm::vec2>>> crossings;
		for (size_t index = 0; index < views.size(); index++)
		{
			if (!views[index].WorldSpace)
				continue;
			if (const std::optional<std::pair<glm::vec2, float>> crossing = GameUIPointer::IntersectCanvas(ui.GetCanvasWorldMatrix(views[index]), *ray))
				crossings.push_back({ crossing->second, { index, crossing->first } });
		}
		if (crossings.empty())
			return {};
		std::stable_sort(crossings.begin(), crossings.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		// What the scene draws in front of a canvas hides it, so a click there picks that instead (the ID buffer's pick).
		const std::optional<SceneRayHit> sceneHit = SceneRaycast::Cast(scene, Ray(ray->Origin, ray->Direction));
		for (const auto& [distance, crossing] : crossings)
		{
			if (sceneHit && sceneHit->Distance < distance)
				break;
			if (const Entity picked = pick(views[crossing.first], crossing.second))
				return picked;
		}
		return {};
	}

	namespace
	{

		// The entities' placements at the game's screen, or the reason one has none.
		bool FindPlacements(EditorContext& context, std::span<const Entity> entities, std::vector<Placement>& outPlacements, std::string* outError)
		{
			if (entities.empty())
			{
				if (outError)
					*outError = "No entities";
				return false;
			}
			Scene& scene = *entities.front().GetScene();
			const UIScreen screen = GetGameScreen(context);
			outPlacements.clear();
			for (const Entity entity : entities)
			{
				std::optional<Placement> placement = entity.HasComponent<UIRectComponent>() ? FindPlacement(scene, entity, screen) : std::nullopt;
				if (!placement)
				{
					if (outError)
						*outError = fmt::format("'{}' is no element laid out on a UI canvas", entity.GetName());
					return false;
				}
				outPlacements.push_back(*placement);
			}
			return true;
		}

		// Writes new rects as one edit of the active scene (mergeKey: empty for its own undo step).
		bool CommitRects(EditorContext& context, std::span<const Entity> entities, const std::vector<UIRectComponent>& rects, std::string name, std::string mergeKey,
			std::string* outError)
		{
			if (!context.CheckSceneEditable(outError))
				return false;
			std::vector<UUID> ids;
			for (const Entity entity : entities)
				ids.push_back(entity.GetUUID());
			SceneEditTransaction transaction(*entities.front().GetScene(), std::move(name), ids, std::move(mergeKey));
			for (size_t index = 0; index < entities.size(); index++)
			{
				Entity entity = entities[index];
				entity.GetComponent<UIRectComponent>() = rects[index];
				entity.MarkModified<UIRectComponent>();
			}
			context.CommitEdit(transaction);
			return true;
		}

	}

	bool ApplyPresetWithUndo(EditorContext& context, std::span<const Entity> entities, UIAnchorPreset preset, bool setPivot, bool snapToAnchors, std::string* outError)
	{
		std::vector<Placement> placements;
		if (!FindPlacements(context, entities, placements, outError))
			return false;
		std::vector<UIRectComponent> rects;
		for (size_t index = 0; index < entities.size(); index++)
			rects.push_back(ApplyPreset(entities[index].GetComponent<UIRectComponent>(), placements[index].Box, placements[index].Parent, preset, setPivot, snapToAnchors));
		return CommitRects(context, entities, rects, fmt::format("Anchor {}", GetPresetName(preset)), {}, outError);
	}

	bool SetMarginWithUndo(EditorContext& context, std::span<const Entity> entities, int axis, bool end, float margin, std::string* outError)
	{
		if ((axis != 0 && axis != 1) || !std::isfinite(margin))
		{
			if (outError)
				*outError = "The margin must be a finite number of an axis 0 or 1";
			return false;
		}
		std::vector<Placement> placements;
		if (!FindPlacements(context, entities, placements, outError))
			return false;
		std::vector<UIRectComponent> rects;
		std::string mergeKey = fmt::format("UIRectMargin/{}/{}", axis, end ? "End" : "Start");
		for (size_t index = 0; index < entities.size(); index++)
		{
			const UIRectComponent& rect = entities[index].GetComponent<UIRectComponent>();
			if (!IsStretched(rect, axis))
			{
				if (outError)
					*outError = fmt::format("'{}' is not stretched {}", entities[index].GetName(), axis == 0 ? "horizontally" : "vertically");
				return false;
			}
			glm::vec2 margins = GetMargins(rect, placements[index].Box, placements[index].Parent, axis);
			margins[end ? 1 : 0] = margin;
			rects.push_back(SetMargins(rect, placements[index].Parent, axis, margins));
			mergeKey += "/" + entities[index].GetUUID().ToString();
		}
		const char* side = axis == 0 ? (end ? "Right" : "Left") : (end ? "Bottom" : "Top");
		return CommitRects(context, entities, rects, fmt::format("Edit UI Rect {}", side), std::move(mergeKey), outError);
	}

}
