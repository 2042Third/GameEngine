#pragma once

#include "Strata/Core/BinaryStream.h"
#include "Strata/Reflection/Property.h"

#include <cstdint>
#include <optional>
#include <string>

namespace Strata
{

	// The longest string a binary property value holds, in bytes (cooked scenes refuse longer ones).
	inline constexpr uint32_t c_MaxBinaryStringBytes = 1u << 20;

	// Binary form of property values, beside PropertyJson's (cooked scenes), little endian: Bool u8 (0 or 1), Int i32,
	// UInt u32, Float f32, Vec2/3/4 and Color3/4 f32 arrays, Quat f32 x, y, z, w, String u32 length and its UTF-8 bytes,
	// Enum i32, Asset and Entity u64. Non-finite floats are written as 0, as FloatToJson writes them. False when the value is
	// not of the type or a string is longer than c_MaxBinaryStringBytes (nothing is written then).
	bool WritePropertyValue(BinaryWriter& writer, PropertyType type, const PropertyValue& value, std::string* outError = nullptr);
	// Reads a value of the type. nullopt with the reason when the data ends first (the reader is then invalid) or the value is
	// none a writer writes: a boolean other than 0 and 1, a non-finite float, a string over the limit (the reader stays valid
	// past a value it refuses whose size it knows). Ranges and enum options are PropertyInfo::SetValue's to check, as for
	// values read from JSON.
	std::optional<PropertyValue> ReadPropertyValue(BinaryReader& reader, PropertyType type, std::string* outError = nullptr);

}
