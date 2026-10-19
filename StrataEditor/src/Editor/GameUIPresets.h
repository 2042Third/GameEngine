#pragma once

#include <Strata/Scene/Entity.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class Scene;

	// The game UI elements the editor makes ready to use (ui.create, the Hierarchy's UI submenu): each a small hierarchy of
	// UI components with a look of its own (rounded panels, a button with its label, a toggle with its box, checkmark and
	// label, a slider with its track, fill and handle, an input field with its text and placeholder), needing no texture assets.
	enum class UIPresetType : uint8_t
	{
		Canvas = 0,
		Panel,
		Image,
		Text,
		Button,
		Toggle,
		Slider,
		InputField,
		HorizontalLayout,
		VerticalLayout,
		GridLayout
	};
	constexpr uint32_t c_UIPresetTypeCount = 11;

	// What Create made: the element, its canvas (a new top-level one when the parent was outside every canvas) and the
	// parts a script or command may want to reach. Created lists every new entity (the new canvas first).
	struct UIPresetParts
	{
		Entity Element;
		Entity Canvas;
		Entity Label;
		Entity Background;
		Entity Fill;
		Entity Handle;
		Entity Checkmark;
		Entity Placeholder;
		std::vector<Entity> Created;
	};

	namespace GameUIPresets
	{

		// "canvas", "panel", "image", "text", "button", "toggle", "slider", "inputField", "horizontalLayout", "verticalLayout",
		// "gridLayout" (as ui.create takes them), and the names people read ("Vertical Layout").
		const char* GetTypeName(UIPresetType type);
		const char* GetDisplayName(UIPresetType type);
		std::optional<UIPresetType> FindType(std::string_view name);

		// The canvas an entity is part of: itself or its nearest ancestor with a UICanvas (an invalid entity outside every
		// canvas).
		Entity FindCanvas(Entity entity);

		// Creates a preset under `parent` (an invalid entity: the top level). A canvas goes where it is asked; any other
		// element needs a canvas around it, so a parent outside every canvas (or none) gets a new top-level "Canvas" first,
		// under which the element goes. `name` (empty: the preset's display name) names the element; `text` replaces the
		// default text of a text, button or toggle, and an input field's placeholder. Returns nullopt only when the scene is full (outError says so).
		std::optional<UIPresetParts> Create(Scene& scene, UIPresetType type, Entity parent, std::string_view name, const std::optional<std::string>& text,
			std::string* outError);

	}

}
