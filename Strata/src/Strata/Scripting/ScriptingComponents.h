#pragma once

#include "Strata/Reflection/Property.h"

#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// The scripting module's component (registered by RegisterScriptingComponents, with its JSON form and its references).

	struct ScriptFieldValue
	{
		std::string Name;
		PropertyType Type = PropertyType::Float;
		PropertyValue Value = 0.0f;
	};

	struct ScriptEntry
	{
		std::string ClassName;
		std::vector<ScriptFieldValue> Fields; // Overrides of the script's default field values

		ScriptFieldValue* FindField(std::string_view name);
		const ScriptFieldValue* FindField(std::string_view name) const;
	};

	// Native C++ behaviours from the project's script module attached to this entity.
	struct ScriptComponent
	{
		std::vector<ScriptEntry> Scripts;

		ScriptEntry* FindScript(std::string_view className);
		const ScriptEntry* FindScript(std::string_view className) const;
	};

}
