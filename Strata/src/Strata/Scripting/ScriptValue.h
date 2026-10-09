#pragma once

#include "Strata/Reflection/Property.h"

#include "StrataScript/ScriptABI.h"

#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	// Conversions between script ABI values and engine property values (engine-internal).

	// Longest string accepted from a script module; longer sizes are treated as corrupt data.
	constexpr uint64_t c_MaxScriptStringSize = 256ull * 1024 * 1024;

	// Copies a module-provided string. False if the pointer/size pair is invalid.
	bool ReadScriptString(const StrataScriptString& text, std::string& out);
	// A string argument from a script as a view (empty for invalid input).
	std::string_view ScriptStringView(const StrataScriptString& text);

	// The property type of a script field declared with this value type, or nullopt if fields cannot have it.
	std::optional<PropertyType> ScriptFieldTypeToPropertyType(uint32_t valueType);
	const char* ScriptValueTypeToString(uint32_t valueType);

	// Script field values convert exactly: the value must have the field's type.
	std::optional<PropertyValue> ScriptValueToFieldValue(const StrataScriptValue& value, PropertyType fieldType);
	// String values point into `value`, which must outlive the result.
	StrataScriptValue FieldValueToScriptValue(const PropertyValue& value, PropertyType fieldType);

	// Component properties convert leniently: integers for Int/UInt/Enum (and Float), Vec3/Vec4 for colors, Vec4 or Quat
	// for rotations. Range clamping and enum validation happen in PropertyInfo::SetValue.
	std::optional<PropertyValue> ScriptValueToPropertyValue(const StrataScriptValue& value, const PropertyInfo& property, std::string* outError);
	// String values point into `value`, which must outlive the result.
	StrataScriptValue PropertyValueToScriptValue(const PropertyValue& value, PropertyType propertyType);

}
