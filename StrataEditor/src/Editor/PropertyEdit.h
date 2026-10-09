#pragma once

#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/Property.h>
#include <Strata/Scene/Entity.h>

#include <string>

namespace Strata
{

	class EditorContext;

	// Sets one property of an entity's component as an undoable edit of the active scene. Consecutive edits of the
	// same property merge into one undo step until the undo stack's merge is broken (call
	// UndoStack::BreakMerge when a drag or text edit ends). Returns false (nothing changed) for invalid values.
	bool SetPropertyWithUndo(EditorContext& context, Entity entity, const ComponentInfo& component, const PropertyInfo& property, const PropertyValue& value,
		std::string* outError = nullptr);

}
