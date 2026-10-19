#include "stpch.h"
#include "Strata/Reflection/PropertyBinary.h"

#include <cmath>
#include <variant>

namespace Strata
{

	namespace
	{

		// Every type has a binary form: a type added at the end of PropertyType fails this until it gets one (and the switches
		// below warn about any type they miss).
		static_assert(static_cast<int>(PropertyType::Entity) == 13, "A PropertyType was added: give it a binary form in PropertyBinary.cpp");

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

		float Finite(float value)
		{
			return std::isfinite(value) ? value : 0.0f;
		}

		template<int Count>
		void WriteFloats(BinaryWriter& writer, const float* values)
		{
			for (int index = 0; index < Count; index++)
				writer.Write(Finite(values[index]));
		}

		// Reads `Count` floats, refusing non-finite ones (no writer writes them).
		template<int Count>
		bool ReadFloats(BinaryReader& reader, float* outValues, std::string* outError)
		{
			for (int index = 0; index < Count; index++)
				outValues[index] = reader.Read<float>();
			if (!reader.IsValid())
				return Fail(outError, "The value ends early");
			for (int index = 0; index < Count; index++)
			{
				if (!std::isfinite(outValues[index]))
					return Fail(outError, "A float value is not finite");
			}
			return true;
		}

	}

	bool WritePropertyValue(BinaryWriter& writer, PropertyType type, const PropertyValue& value, std::string* outError)
	{
		switch (type)
		{
			case PropertyType::Bool:
				if (!std::holds_alternative<bool>(value))
					break;
				writer.Write(static_cast<uint8_t>(std::get<bool>(value) ? 1 : 0));
				return true;
			case PropertyType::Int:
			case PropertyType::Enum:
				if (!std::holds_alternative<int32_t>(value))
					break;
				writer.Write(std::get<int32_t>(value));
				return true;
			case PropertyType::UInt:
				if (!std::holds_alternative<uint32_t>(value))
					break;
				writer.Write(std::get<uint32_t>(value));
				return true;
			case PropertyType::Float:
				if (!std::holds_alternative<float>(value))
					break;
				writer.Write(Finite(std::get<float>(value)));
				return true;
			case PropertyType::Vec2:
				if (!std::holds_alternative<glm::vec2>(value))
					break;
				WriteFloats<2>(writer, &std::get<glm::vec2>(value).x);
				return true;
			case PropertyType::Vec3:
			case PropertyType::Color3:
				if (!std::holds_alternative<glm::vec3>(value))
					break;
				WriteFloats<3>(writer, &std::get<glm::vec3>(value).x);
				return true;
			case PropertyType::Vec4:
			case PropertyType::Color4:
				if (!std::holds_alternative<glm::vec4>(value))
					break;
				WriteFloats<4>(writer, &std::get<glm::vec4>(value).x);
				return true;
			case PropertyType::Quat:
			{
				if (!std::holds_alternative<glm::quat>(value))
					break;
				const glm::quat& rotation = std::get<glm::quat>(value);
				const float components[4] = { rotation.x, rotation.y, rotation.z, rotation.w };
				WriteFloats<4>(writer, components);
				return true;
			}
			case PropertyType::String:
			{
				if (!std::holds_alternative<std::string>(value))
					break;
				const std::string& text = std::get<std::string>(value);
				if (text.size() > c_MaxBinaryStringBytes)
					return Fail(outError, fmt::format("A string of {} bytes is longer than the {} a binary value holds", text.size(), c_MaxBinaryStringBytes));
				writer.WriteString(text);
				return true;
			}
			case PropertyType::Asset:
			case PropertyType::Entity:
				if (!std::holds_alternative<UUID>(value))
					break;
				writer.Write(static_cast<uint64_t>(std::get<UUID>(value)));
				return true;
		}
		return Fail(outError, fmt::format("The value is not a {}", PropertyTypeToString(type)));
	}

	std::optional<PropertyValue> ReadPropertyValue(BinaryReader& reader, PropertyType type, std::string* outError)
	{
		auto ended = [&]() -> std::optional<PropertyValue>
		{
			Fail(outError, "The value ends early");
			return std::nullopt;
		};
		switch (type)
		{
			case PropertyType::Bool:
			{
				const uint8_t value = reader.Read<uint8_t>();
				if (!reader.IsValid())
					return ended();
				if (value > 1)
				{
					Fail(outError, fmt::format("A boolean value is {}, neither 0 nor 1", value));
					return std::nullopt;
				}
				return PropertyValue(value == 1);
			}
			case PropertyType::Int:
			case PropertyType::Enum:
			{
				const int32_t value = reader.Read<int32_t>();
				return reader.IsValid() ? std::optional<PropertyValue>(PropertyValue(value)) : ended();
			}
			case PropertyType::UInt:
			{
				const uint32_t value = reader.Read<uint32_t>();
				return reader.IsValid() ? std::optional<PropertyValue>(PropertyValue(value)) : ended();
			}
			case PropertyType::Float:
			{
				float value = 0.0f;
				if (!ReadFloats<1>(reader, &value, outError))
					return std::nullopt;
				return PropertyValue(value);
			}
			case PropertyType::Vec2:
			{
				glm::vec2 value(0.0f);
				if (!ReadFloats<2>(reader, &value.x, outError))
					return std::nullopt;
				return PropertyValue(value);
			}
			case PropertyType::Vec3:
			case PropertyType::Color3:
			{
				glm::vec3 value(0.0f);
				if (!ReadFloats<3>(reader, &value.x, outError))
					return std::nullopt;
				return PropertyValue(value);
			}
			case PropertyType::Vec4:
			case PropertyType::Color4:
			{
				glm::vec4 value(0.0f);
				if (!ReadFloats<4>(reader, &value.x, outError))
					return std::nullopt;
				return PropertyValue(value);
			}
			case PropertyType::Quat:
			{
				float components[4] = {};
				if (!ReadFloats<4>(reader, components, outError))
					return std::nullopt;
				glm::quat rotation;
				rotation.x = components[0];
				rotation.y = components[1];
				rotation.z = components[2];
				rotation.w = components[3];
				return PropertyValue(rotation);
			}
			case PropertyType::String:
			{
				const uint32_t length = reader.Read<uint32_t>();
				if (!reader.IsValid())
					return ended();
				if (length > c_MaxBinaryStringBytes)
				{
					// Its bytes are not skipped: a length this large is corruption, and what follows cannot be trusted.
					Fail(outError, fmt::format("A string of {} bytes is longer than the {} a binary value holds", length, c_MaxBinaryStringBytes));
					return std::nullopt;
				}
				const std::span<const uint8_t> bytes = reader.ReadView(length);
				if (!reader.IsValid())
					return ended();
				return PropertyValue(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
			}
			case PropertyType::Asset:
			case PropertyType::Entity:
			{
				const uint64_t value = reader.Read<uint64_t>();
				return reader.IsValid() ? std::optional<PropertyValue>(PropertyValue(UUID(value))) : ended();
			}
		}
		Fail(outError, "Unknown property type");
		return std::nullopt;
	}

}
