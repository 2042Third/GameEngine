#pragma once

#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Reflection/PropertyJson.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	class Entity;

	// Reflection-driven access to component data, shared by serialization, the editor inspector, the automation
	// API, undo/redo and scripting so that every path validates the same way. Entity-level functions notify
	// systems of changes; Deserialize works on a detached component and leaves notification to the caller.
	class ComponentAccess
	{
	public:
		// Serializable properties (and extra data) of a component as a JSON object.
		static nlohmann::json Serialize(const ComponentInfo& info, const void* component);

		// Applies the properties present in `json`; absent properties keep their values.
		// strict = true: the first invalid or unknown property fails the call (nothing after it is applied).
		// strict = false: invalid or unknown properties are skipped and reported in outWarnings (file loading).
		static bool Deserialize(const ComponentInfo& info, void* component, const nlohmann::json& json, bool strict, std::string* outError, std::vector<std::string>* outWarnings = nullptr);

		static std::optional<PropertyValue> GetProperty(Entity entity, const ComponentInfo& info, const PropertyInfo& property);
		// Validates and assigns the value, then notifies systems of the change. Read-only properties are rejected.
		static bool SetProperty(Entity entity, const ComponentInfo& info, const PropertyInfo& property, const PropertyValue& value, std::string* outError = nullptr);

		// Adds the component if missing (no-op when present). Returns false for unknown/hidden components.
		static bool AddComponent(Entity entity, const ComponentInfo& info, std::string* outError = nullptr);
		static bool RemoveComponent(Entity entity, const ComponentInfo& info, std::string* outError = nullptr);

		// Snapshot of every serializable component on an entity: {"ComponentName": {...}, ...}.
		static nlohmann::json SerializeEntityComponents(Entity entity);
	};

}
