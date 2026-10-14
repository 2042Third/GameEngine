#pragma once

#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/GameUI/GameUIScene.h>
#include <Strata/Scene/Entity.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Strata
{

	class EditorContext;
	class Scene;

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
		// "Top Left" ... "Stretch", as the Inspector shows them.
		const char* GetPresetDisplayName(UIAnchorPreset preset);
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

		// The screen the editor shows the game UI at (commands, the Inspector): while playing the UI's input screen (the game
		// view's image), in edit mode the viewport's image, headless GameUIScene::c_DefaultScreen.
		UIScreen GetGameScreen(EditorContext& context);

		// Where an element is laid out at a screen: its box and its parent's (canvas units: the nearest ancestor that is an
		// element) and its canvas's scale. Nullopt for entities that are no laid-out element, and for root canvases, whose
		// rect is the screen.
		struct Placement
		{
			UIBox Box;
			UIBox Parent;
			float Scale = 1.0f;
		};
		std::optional<Placement> FindPlacement(Scene& scene, Entity entity, const UIScreen& screen);

		// What a click at a pixel of a screen selects in the editor: the topmost visible element that draws something (an image
		// or a text) there, in every canvas whether it receives input or not. Root canvases and empty layout rects are never
		// picked, so they do not hide the scene behind them. Invalid when there is none.
		Entity PickElement(Scene& scene, const UIScreen& screen, const glm::vec2& pixel);

		// Gives every entity an anchor preset (ApplyPreset at the game's screen), as one undo step. Fails, changing nothing,
		// when an entity has no UIRect, is no laid-out element or is a root canvas (its rect is the screen), or the scene cannot
		// be edited.
		bool ApplyPresetWithUndo(EditorContext& context, std::span<const Entity> entities, UIAnchorPreset preset, bool setPivot, bool snapToAnchors,
			std::string* outError = nullptr);
		// Sets one margin (`end`: right or bottom, else left or top) along a stretched axis of every entity at the game's
		// screen, keeping the other; consecutive edits of that margin of those entities (a drag) merge into one undo step
		// until the undo stack's merge breaks. Fails, changing nothing, for entities not stretched along the axis.
		bool SetMarginWithUndo(EditorContext& context, std::span<const Entity> entities, int axis, bool end, float margin, std::string* outError = nullptr);

	}

}
