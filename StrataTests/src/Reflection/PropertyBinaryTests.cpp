#include <doctest/doctest.h>

#include <Strata/Core/BinaryStream.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyBinary.h>
#include <Strata/Scene/Components.h>

#include <nlohmann/json.hpp>

#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace Strata;

namespace
{

	std::vector<uint8_t> Write(PropertyType type, const PropertyValue& value)
	{
		BinaryWriter writer;
		std::string error;
		INFO(error);
		REQUIRE(WritePropertyValue(writer, type, value, &error));
		return writer.GetData();
	}

	PropertyValue RoundTrip(PropertyType type, const PropertyValue& value)
	{
		const std::vector<uint8_t> bytes = Write(type, value);
		BinaryReader reader(bytes);
		std::string error;
		const std::optional<PropertyValue> read = ReadPropertyValue(reader, type, &error);
		INFO(error);
		REQUIRE(read.has_value());
		CHECK(reader.IsValid());
		CHECK(reader.GetRemaining() == 0);
		return *read;
	}

	glm::quat MakeQuat(float x, float y, float z, float w)
	{
		glm::quat rotation;
		rotation.x = x;
		rotation.y = y;
		rotation.z = z;
		rotation.w = w;
		return rotation;
	}

	// Every type with values at its extremes.
	std::vector<std::pair<PropertyType, PropertyValue>> MakeValues()
	{
		const float lowest = std::numeric_limits<float>::lowest();
		const float largest = std::numeric_limits<float>::max();
		const float tiny = std::numeric_limits<float>::denorm_min();
		return {
			{ PropertyType::Bool, PropertyValue(true) },
			{ PropertyType::Bool, PropertyValue(false) },
			{ PropertyType::Int, PropertyValue(std::numeric_limits<int32_t>::min()) },
			{ PropertyType::Int, PropertyValue(std::numeric_limits<int32_t>::max()) },
			{ PropertyType::UInt, PropertyValue(std::numeric_limits<uint32_t>::max()) },
			{ PropertyType::Float, PropertyValue(-0.0f) },
			{ PropertyType::Float, PropertyValue(tiny) },
			{ PropertyType::Float, PropertyValue(largest) },
			{ PropertyType::Vec2, PropertyValue(glm::vec2(lowest, 0.1f)) },
			{ PropertyType::Vec3, PropertyValue(glm::vec3(1.0f, -2.5f, largest)) },
			{ PropertyType::Vec4, PropertyValue(glm::vec4(tiny, 0.0f, -1.0f, 3.0e-7f)) },
			{ PropertyType::Quat, PropertyValue(MakeQuat(0.1f, 0.2f, 0.3f, 0.927362f)) },
			{ PropertyType::Color3, PropertyValue(glm::vec3(0.0f, 0.5f, 16.0f)) },
			{ PropertyType::Color4, PropertyValue(glm::vec4(1.0f, 0.25f, 0.125f, 0.0f)) },
			{ PropertyType::String, PropertyValue(std::string()) },
			{ PropertyType::String, PropertyValue(std::string("Grüße, \"quotes\"\n\t\x01")) },
			{ PropertyType::String, PropertyValue(std::string(c_MaxBinaryStringBytes, 'x')) },
			{ PropertyType::Enum, PropertyValue(int32_t(-3)) },
			{ PropertyType::Enum, PropertyValue(int32_t(7)) },
			{ PropertyType::Asset, PropertyValue(UUID(0xFEDCBA9876543210ull)) },
			{ PropertyType::Asset, PropertyValue(UUID::Null()) },
			{ PropertyType::Entity, PropertyValue(UUID(std::numeric_limits<uint64_t>::max())) },
		};
	}

}

TEST_SUITE("Reflection.PropertyBinary")
{
	TEST_CASE("Every property type round trips through its binary form, at its extremes")
	{
		for (const auto& [type, value] : MakeValues())
		{
			CAPTURE(PropertyTypeToString(type));
			const PropertyValue read = RoundTrip(type, value);
			REQUIRE(read.index() == value.index());
			if (const float* number = std::get_if<float>(&value))
			{
				// Bit for bit: -0 stays -0.
				const float readNumber = std::get<float>(read);
				CHECK(std::memcmp(&readNumber, number, sizeof(float)) == 0);
			}
			else if (const glm::quat* rotation = std::get_if<glm::quat>(&value))
			{
				const glm::quat readRotation = std::get<glm::quat>(read);
				CHECK(readRotation.x == rotation->x);
				CHECK(readRotation.y == rotation->y);
				CHECK(readRotation.z == rotation->z);
				CHECK(readRotation.w == rotation->w);
			}
			else
			{
				CHECK(read == value);
			}
		}
		// The sizes the format names.
		CHECK(Write(PropertyType::Bool, PropertyValue(true)).size() == 1);
		CHECK(Write(PropertyType::Int, PropertyValue(int32_t(1))).size() == 4);
		CHECK(Write(PropertyType::Vec3, PropertyValue(glm::vec3(1.0f))).size() == 12);
		CHECK(Write(PropertyType::Quat, PropertyValue(glm::quat(1.0f, 0.0f, 0.0f, 0.0f))).size() == 16);
		CHECK(Write(PropertyType::String, PropertyValue(std::string("abc"))).size() == 7);
		CHECK(Write(PropertyType::Entity, PropertyValue(UUID(1))).size() == 8);
		// Non-finite floats are written as 0, as JSON writes them.
		const PropertyValue infinite = RoundTrip(PropertyType::Vec2, PropertyValue(glm::vec2(std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN())));
		CHECK(std::get<glm::vec2>(infinite) == glm::vec2(0.0f));
	}

	TEST_CASE("Values a writer does not write are refused: truncated, non-finite, oversized, booleans other than 0 and 1, values of another type")
	{
		// Truncated at every byte: refused, the reader failed.
		for (const auto& [type, value] : MakeValues())
		{
			CAPTURE(PropertyTypeToString(type));
			const std::vector<uint8_t> bytes = Write(type, value);
			const size_t limit = std::min<size_t>(bytes.size(), 64);
			for (size_t size = 0; size < limit; size++)
			{
				BinaryReader reader(std::span<const uint8_t>(bytes.data(), size));
				std::string error;
				CHECK_FALSE(ReadPropertyValue(reader, type, &error).has_value());
				CHECK_FALSE(error.empty());
			}
		}

		auto read = [](PropertyType type, const std::vector<uint8_t>& bytes, bool& outReaderValid)
		{
			BinaryReader reader(bytes);
			std::string error;
			const std::optional<PropertyValue> value = ReadPropertyValue(reader, type, &error);
			outReaderValid = reader.IsValid();
			CHECK((value.has_value() || !error.empty()));
			return value;
		};
		bool valid = false;
		CHECK_FALSE(read(PropertyType::Bool, { 2 }, valid).has_value());
		CHECK(valid);
		CHECK_FALSE(read(PropertyType::Bool, { 0xFF }, valid).has_value());
		BinaryWriter nan;
		nan.Write(std::numeric_limits<float>::quiet_NaN());
		CHECK_FALSE(read(PropertyType::Float, nan.GetData(), valid).has_value());
		BinaryWriter infinite;
		infinite.Write(1.0f);
		infinite.Write(-std::numeric_limits<float>::infinity());
		infinite.Write(1.0f);
		CHECK_FALSE(read(PropertyType::Color3, infinite.GetData(), valid).has_value());
		BinaryWriter oversized;
		oversized.Write(static_cast<uint32_t>(c_MaxBinaryStringBytes + 1));
		oversized.WriteBytes(std::string(16, 'x').data(), 16);
		CHECK_FALSE(read(PropertyType::String, oversized.GetData(), valid).has_value());

		// The writer refuses strings over the limit and values of another type, writing nothing.
		BinaryWriter writer;
		std::string error;
		CHECK_FALSE(WritePropertyValue(writer, PropertyType::String, PropertyValue(std::string(c_MaxBinaryStringBytes + 1, 'x')), &error));
		CHECK_FALSE(WritePropertyValue(writer, PropertyType::Vec3, PropertyValue(1.0f), &error));
		CHECK_FALSE(WritePropertyValue(writer, PropertyType::Enum, PropertyValue(uint32_t(1)), &error));
		CHECK(writer.GetSize() == 0);
	}
}

TEST_SUITE("Reflection.PropertyBinary")
{
	TEST_CASE("The Script component's binary extra data holds its scripts and typed fields as the JSON form does")
	{
		const ComponentInfo* info = ComponentRegistry::Find<ScriptComponent>();
		REQUIRE(info);
		REQUIRE(info->SerializeExtraBinary);
		REQUIRE(info->DeserializeExtraBinary);

		ScriptComponent scripts;
		ScriptEntry& player = scripts.Scripts.emplace_back();
		player.ClassName = "Player";
		player.Fields = { { "Alive", PropertyType::Bool, PropertyValue(true) }, { "Count", PropertyType::Int, PropertyValue(int32_t(-4)) },
			{ "Mode", PropertyType::Enum, PropertyValue(int32_t(2)) }, { "Name", PropertyType::String, PropertyValue(std::string("Ada")) },
			{ "Speed", PropertyType::Float, PropertyValue(2.5f) }, { "Target", PropertyType::Entity, PropertyValue(UUID(42)) },
			{ "Tint", PropertyType::Color4, PropertyValue(glm::vec4(1.0f, 0.5f, 0.25f, 1.0f)) },
			// A value that is not of its type is left out, as the JSON form leaves it out.
			{ "Broken", PropertyType::Vec3, PropertyValue(1.0f) } };
		scripts.Scripts.emplace_back().ClassName = "Empty";

		BinaryWriter writer;
		std::string error;
		REQUIRE(info->SerializeExtraBinary(&scripts, writer, &error));
		ScriptComponent read;
		BinaryReader reader(writer.GetData());
		REQUIRE_MESSAGE(info->DeserializeExtraBinary(&read, reader, &error), error);
		CHECK(reader.GetRemaining() == 0);

		// What the JSON form reads back.
		nlohmann::json json;
		info->SerializeExtra(&scripts, json);
		ScriptComponent fromJson;
		REQUIRE(info->DeserializeExtra(&fromJson, json, &error));
		REQUIRE(read.Scripts.size() == fromJson.Scripts.size());
		for (size_t script = 0; script < read.Scripts.size(); script++)
		{
			CHECK(read.Scripts[script].ClassName == fromJson.Scripts[script].ClassName);
			REQUIRE(read.Scripts[script].Fields.size() == fromJson.Scripts[script].Fields.size());
			for (size_t field = 0; field < read.Scripts[script].Fields.size(); field++)
			{
				const ScriptFieldValue& binary = read.Scripts[script].Fields[field];
				const ScriptFieldValue& text = fromJson.Scripts[script].Fields[field];
				CHECK(binary.Name == text.Name);
				CHECK(binary.Type == text.Type);
				CHECK(binary.Value == text.Value);
			}
		}

		// Data that ends early fails, at every byte.
		for (size_t size = 0; size < writer.GetSize(); size++)
		{
			ScriptComponent truncated;
			BinaryReader cut(std::span<const uint8_t>(writer.GetData().data(), size));
			CHECK_FALSE(info->DeserializeExtraBinary(&truncated, cut, &error));
		}
	}
}
