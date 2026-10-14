#pragma once

#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/GameUI/GameUIScene.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace Strata
{

	// The anchor presets of a UIRect, shared by ui.create's "anchor" and the Inspector's anchor grid: rows top, middle, bottom
	// and stretch (vertically), columns left, center, right and stretch (horizontally). Point presets put both anchors (and
	// the pivot) at that point of the parent; stretch presets span the parent along their axis.
	enum class UIAnchorPreset : uint8_t
	{
		TopLeft = 0, TopCenter, TopRight, TopStretch,
		MiddleLeft, MiddleCenter, MiddleRight, MiddleStretch,
		BottomLeft, BottomCenter, BottomRight, BottomStretch,
		StretchLeft, StretchCenter, StretchRight, Stretch
	};
	constexpr uint32_t c_UIAnchorPresetCount = 16;

	namespace UIRectEdit
	{

		// "topLeft" ... "stretch" (camelCase, as commands take them).
		const char* GetPresetName(UIAnchorPreset preset);
		std::optional<UIAnchorPreset> FindPreset(std::string_view name);
		// The preset whose anchors the rect has (nullopt for anchors of no preset).
		std::optional<UIAnchorPreset> FindPreset(const UIRectComponent& rect);

		struct Anchors
		{
			glm::vec2 Min = glm::vec2(0.5f);
			glm::vec2 Max = glm::vec2(0.5f);
			glm::vec2 Pivot = glm::vec2(0.5f); // What the preset sets as the pivot when asked to
		};
		Anchors GetAnchors(UIAnchorPreset preset);

		// Whether the rect stretches along an axis (0: horizontal, 1: vertical): its anchors differ there.
		bool IsStretched(const UIRectComponent& rect, int axis);

		// Gives a rect new anchors (and optionally the preset's pivot) while it stays where it is: `box` is where the rect is
		// now and `parent` its parent's box (canvas units, from the layout). With `snapToAnchors` the rect moves onto its new
		// anchors instead (Position 0; along stretched axes it fills the parent).
		UIRectComponent ApplyPreset(const UIRectComponent& rect, const UIBox& box, const UIBox& parent, UIAnchorPreset preset, bool setPivot, bool snapToAnchors);

		// The margins of a rect along a stretched axis: the distance from its parent's start edge to its own (left or top)
		// and from its own end edge to the parent's (right or bottom), positive inward.
		glm::vec2 GetMargins(const UIRectComponent& rect, const UIBox& box, const UIBox& parent, int axis);
		// Sets those margins, keeping the anchors and pivot.
		UIRectComponent SetMargins(const UIRectComponent& rect, const UIBox& parent, int axis, const glm::vec2& margins);

	}

}
