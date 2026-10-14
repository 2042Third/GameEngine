#include "Editor/UIRectEdit.h"

#include <cmath>
#include <string>

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

}
