#pragma once

#include "Strata/Reflection/Property.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace Strata
{

	// Canonical JSON form of a property value, as written to scene and asset files:
	//   Bool/Int/UInt/Float: JSON scalars; Vec2/3/4, Color3/4: arrays; Quat: [x, y, z, w];
	//   String: string; Enum: option name; Asset/Entity: UUID as 16 hex digits.
	nlohmann::json PropertyValueToJson(const PropertyInfo& property, const PropertyValue& value);

	// Parses JSON into a value for the property. Besides the canonical form it accepts convenient
	// alternatives, which makes the automation API forgiving for humans and AI agents:
	//   vectors/colors: objects with x/y/z/w (or r/g/b/a) keys; a single number fills every component
	//   Quat: [pitch, yaw, roll] Euler degrees, or {"Euler": [...]}
	//   Enum: option name (case-insensitive) or integer value
	//   Asset/Entity: hex string, integer, or null / 0 / "" for none
	// Range clamping and enum validation happen later in PropertyInfo::SetValue.
	std::optional<PropertyValue> PropertyValueFromJson(const PropertyInfo& property, const nlohmann::json& json, std::string* outError = nullptr);

	// A float as a JSON number in its shortest round-trip form ("0.1" rather than "0.10000000149011612").
	// Non-finite values (not representable in JSON) are written as 0.
	nlohmann::json FloatToJson(float value);

	// JSON value <-> UUID (used for asset and entity references everywhere in JSON files).
	nlohmann::json UUIDToJson(UUID uuid);
	std::optional<UUID> UUIDFromJson(const nlohmann::json& json);

	// JSON description of a property (name, type, ranges, enum options) for tooling and the automation API.
	nlohmann::json DescribeProperty(const PropertyInfo& property);

}
