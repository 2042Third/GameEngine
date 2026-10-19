#pragma once

#include <nlohmann/json.hpp>

namespace Strata::Tests
{

	// The metadata of every registered component, in registration order: name, display name, category, description, flags,
	// whether it has extra data, and per property its name, display name, tooltip, type, flags, range, drag speed, asset
	// filter and enum values. StrataTests/Architecture/ComponentSchema.json is this, written by
	// --strata-test-helper=write-component-schema <file> before components moved into their modules; Reflection.ComponentSchema
	// checks that the registry still describes every component in it the same way, in the same order.
	nlohmann::json DescribeComponentSchema();

}
