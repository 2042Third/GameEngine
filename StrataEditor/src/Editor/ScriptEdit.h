#pragma once

#include <Strata/Reflection/Property.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scripting/ScriptTypes.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class EditorContext;
	class Scene;

	// Undoable edits of the scripts attached to an entity (the entries of its Script component), shared by the script.*
	// commands and the inspector. Like other edits, they record one undo step in edit mode and change the running copy
	// while playing (applied to the live script instances as well).
	namespace ScriptEdit
	{

		// A value for a field from JSON: Bool true/false, Int and Float numbers, Vec2/3/4 and Quat arrays ([x, y, z, w]),
		// String text, Entity an entity ID of `scene` (null for none), Asset a handle or a path in the asset directory.
		std::optional<PropertyValue> FieldValueFromJson(const nlohmann::json& json, const ScriptFieldInfo& field, Scene& scene, std::string* outError = nullptr);
		nlohmann::json FieldValueToJson(const PropertyValue& value, PropertyType type);
		// {"name", "type", "default"} of a field and {"name", "fields", "callbacks"} of a class.
		nlohmann::json DescribeField(const ScriptFieldInfo& field);
		nlohmann::json DescribeClass(const ScriptClassInfo& info);

		// Attaches a script class (of the loaded module) to the entity with field overrides, which must all be fields of
		// the class with its types. Fails if the entity already has the class.
		bool AddScript(EditorContext& context, Entity entity, const ScriptClassInfo& info, const std::vector<ScriptFieldValue>& fields,
			std::string* outError = nullptr);
		// Detaches a script by class name (works without a loaded module); the Script component goes with the last one.
		bool RemoveScript(EditorContext& context, Entity entity, std::string_view className, std::string* outError = nullptr);
		// Overrides a field of an attached script (`value`), or removes the override so the class default applies
		// (nullopt). The value must have the field's type. Consecutive edits of the same field merge into one undo step until
		// the undo stack's merge is broken (the inspector does that when a drag or text edit ends).
		bool SetField(EditorContext& context, Entity entity, std::string_view className, const ScriptFieldInfo& field, const std::optional<PropertyValue>& value,
			std::string* outError = nullptr);

	}

}
