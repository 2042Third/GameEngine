#pragma once

#include <Strata/Reflection/Property.h>

namespace Strata
{

	class Scene;

	// Drag-and-drop payload types shared by the panels.
	namespace DragDrop
	{
		constexpr const char* c_Entity = "STRATA_ENTITY"; // UUID of an entity of the active scene
		constexpr const char* c_Asset = "STRATA_ASSET";   // AssetHandle
	}

	struct PropertyEditResult
	{
		bool Changed = false;  // The value was modified this frame
		bool Finished = false; // An interactive edit (drag, text input) ended this frame
	};

	// Draws an editor widget for a reflected property and edits `value` in place. Entity references show the names of
	// entities of `scene`; asset references accept assets of the property's type (combo or drag and drop).
	PropertyEditResult DrawPropertyWidget(const PropertyInfo& property, PropertyValue& value, Scene& scene);

}
