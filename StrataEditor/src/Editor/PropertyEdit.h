#pragma once

#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/Property.h>
#include <Strata/Scene/Entity.h>

#include <nlohmann/json.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>

namespace Strata
{

	class EditorContext;

	// Sets one property of an entity's component as an undoable edit of the active scene. Consecutive edits of the
	// same property merge into one undo step until the undo stack's merge is broken (call
	// UndoStack::BreakMerge when a drag or text edit ends). Returns false (nothing changed) for invalid values.
	bool SetPropertyWithUndo(EditorContext& context, Entity entity, const ComponentInfo& component, const PropertyInfo& property, const PropertyValue& value,
		std::string* outError = nullptr);

	// The value of a property of the same component on several entities, as the inspector shows a multi-selection.
	struct SharedPropertyValue
	{
		PropertyValue Value;       // The first entity's
		bool Mixed = false;        // Another entity's value differs
		// Per component of vectors and colors (x, y, z, w): another entity's differs there. Rotations are compared whole.
		std::array<bool, 4> MixedComponents = {};
	};
	// Nullopt when the entities are empty or one of them lacks the component.
	std::optional<SharedPropertyValue> GetSharedProperty(std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property);

	// Sets a property on several entities as one undoable edit (one SceneEditTransaction), merging with the previous edit of
	// the same property of the same entities like the single-entity form. `shown` is what the widget started from: for
	// vectors and colors only the components that differ between it and `value` are written, so editing X of entities whose
	// Y differs keeps each one's Y (and clicking an axis label resets that axis only); when `value` equals `shown`, every
	// entity gets it whole. Fails, changing nothing, when a value is invalid or an entity lacks the component.
	bool SetPropertyWithUndo(EditorContext& context, std::span<const Entity> entities, const ComponentInfo& component, const PropertyInfo& property,
		const PropertyValue& shown, const PropertyValue& value, std::string* outError = nullptr);

	// The property values of a new component of this type, in the form component.set takes: what the inspector's Reset
	// restores.
	nlohmann::json GetDefaultComponentValues(const ComponentInfo& component);

}
