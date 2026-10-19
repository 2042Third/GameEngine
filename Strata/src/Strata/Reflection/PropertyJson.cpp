#include "stpch.h"
#include "Strata/Reflection/PropertyJson.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Math/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace Strata
{

	namespace
	{

		template<glm::length_t L>
		nlohmann::json VectorToJson(const glm::vec<L, float>& vector)
		{
			nlohmann::json array = nlohmann::json::array();
			for (glm::length_t index = 0; index < L; index++)
				array.push_back(FloatToJson(vector[index]));
			return array;
		}

		// Reads a JSON number as a finite float; out-of-range or non-numeric values are rejected.
		std::optional<float> ReadNumber(const nlohmann::json& json)
		{
			if (!json.is_number())
				return std::nullopt;

			const double value = json.get<double>();
			if (!std::isfinite(value) || std::abs(value) > static_cast<double>(std::numeric_limits<float>::max()))
				return std::nullopt;
			return static_cast<float>(value);
		}

		template<glm::length_t L>
		std::optional<glm::vec<L, float>> ReadVector(const nlohmann::json& json, bool isColor, std::string* outError)
		{
			glm::vec<L, float> result(isColor && L == 4 ? 1.0f : 0.0f);
			if (json.is_number())
			{
				const std::optional<float> number = ReadNumber(json);
				if (!number)
				{
					if (outError)
						*outError = "number is out of range";
					return std::nullopt;
				}
				const float value = *number;
				for (glm::length_t index = 0; index < L; index++)
					result[index] = value;
				if (isColor && L == 4)
					result[L - 1] = 1.0f;
				return result;
			}

			if (json.is_array())
			{
				// Colors may omit alpha: [r, g, b] for a Color4 means alpha 1.
				const size_t required = (isColor && L == 4) ? 3 : static_cast<size_t>(L);
				if (json.size() < required || json.size() > static_cast<size_t>(L))
				{
					if (outError)
						*outError = fmt::format("expected an array of {} numbers", L);
					return std::nullopt;
				}
				for (size_t index = 0; index < json.size(); index++)
				{
					const std::optional<float> component = ReadNumber(json[index]);
					if (!component)
					{
						if (outError)
							*outError = "array elements must be finite numbers";
						return std::nullopt;
					}
					result[static_cast<glm::length_t>(index)] = *component;
				}
				return result;
			}

			if (json.is_object())
			{
				static constexpr const char* positionKeys[] = { "x", "y", "z", "w" };
				static constexpr const char* colorKeys[] = { "r", "g", "b", "a" };
				for (glm::length_t index = 0; index < L; index++)
				{
					const char* key = positionKeys[index];
					if (!json.contains(key))
						key = colorKeys[index];
					if (!json.contains(key))
					{
						if (isColor && L == 4 && index == 3)
							continue; // Alpha defaults to 1
						if (outError)
							*outError = fmt::format("missing component '{}'", positionKeys[index]);
						return std::nullopt;
					}

					const std::optional<float> component = ReadNumber(json[key]);
					if (!component)
					{
						if (outError)
							*outError = fmt::format("component '{}' must be a finite number", key);
						return std::nullopt;
					}
					result[index] = *component;
				}
				return result;
			}

			if (outError)
				*outError = fmt::format("expected an array of {} numbers", L);
			return std::nullopt;
		}

	}

	namespace
	{

		// 10^0 to 10^22, the powers of ten a double holds exactly.
		constexpr double c_ExactPowersOfTen[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16,
			1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };

		// The double nearest to a decimal number (fmt's shortest form of a float: an optional '-', digits with an optional '.'
		// and an optional exponent), where one IEEE multiplication or division of exact values gives it: a significand of at
		// most 2^53 and a power of ten up to 10^22 (the result of either operation is the exact value correctly rounded, so it
		// is what a correctly rounding strtod gives). Nullopt for anything else, which callers read the slow way.
		std::optional<double> ParseExactDecimal(std::string_view text)
		{
			size_t position = 0;
			const bool negative = position < text.size() && text[position] == '-';
			if (negative)
				position++;
			uint64_t significand = 0;
			int fractionDigits = 0;
			bool point = false;
			bool digits = false;
			for (; position < text.size(); position++)
			{
				const char character = text[position];
				if (character >= '0' && character <= '9')
				{
					digits = true;
					if (point)
						fractionDigits++;
					if (significand > (UINT64_MAX - 9) / 10)
						return std::nullopt;
					significand = significand * 10 + static_cast<uint64_t>(character - '0');
				}
				else if (character == '.' && !point)
				{
					point = true;
				}
				else
				{
					break;
				}
			}
			int exponent = 0;
			if (position < text.size() && (text[position] == 'e' || text[position] == 'E'))
			{
				position++;
				bool negativeExponent = false;
				if (position < text.size() && (text[position] == '+' || text[position] == '-'))
					negativeExponent = text[position++] == '-';
				bool exponentDigits = false;
				for (; position < text.size() && text[position] >= '0' && text[position] <= '9'; position++)
				{
					exponentDigits = true;
					if (exponent > 1000)
						return std::nullopt;
					exponent = exponent * 10 + (text[position] - '0');
				}
				if (!exponentDigits)
					return std::nullopt;
				if (negativeExponent)
					exponent = -exponent;
			}
			if (!digits || position != text.size() || significand > (uint64_t(1) << 53))
				return std::nullopt;

			const int scale = exponent - fractionDigits;
			double value = static_cast<double>(significand);
			if (scale >= 0)
			{
				if (scale > 22)
					return std::nullopt;
				value *= c_ExactPowersOfTen[scale];
			}
			else
			{
				if (-scale > 22)
					return std::nullopt;
				value /= c_ExactPowersOfTen[-scale];
			}
			return negative ? -value : value;
		}

		template<glm::length_t L>
		void AppendVector(std::string& out, const glm::vec<L, float>& vector)
		{
			out += '[';
			for (glm::length_t index = 0; index < L; index++)
			{
				if (index > 0)
					out += ',';
				AppendFloatJson(out, vector[index]);
			}
			out += ']';
		}

		template<typename T>
		void AppendInteger(std::string& out, T value)
		{
			char buffer[32];
			const auto result = fmt::format_to_n(buffer, sizeof(buffer), "{}", value);
			out.append(buffer, std::min(static_cast<size_t>(result.size), sizeof(buffer)));
		}

	}

	void AppendFloatJson(std::string& out, float value)
	{
		// FloatToJson's steps without its parse: fmt's shortest form of the float, read as nlohmann reads it (text without a
		// point or an exponent is an integer, "-0" the integer 0; the rest the double strtod gives, which nlohmann writes in
		// its own shortest form, nlohmann::detail::to_chars, as Dump does).
		if (!std::isfinite(value))
		{
			out += "0.0";
			return;
		}
		char buffer[64];
		const auto result = fmt::format_to_n(buffer, sizeof(buffer), "{}", value);
		const std::string_view text(buffer, std::min(static_cast<size_t>(result.size), sizeof(buffer)));
		if (text.find_first_of(".eE") == std::string_view::npos)
		{
			out.append(text == "-0" ? std::string_view("0") : text);
			return;
		}
		std::optional<double> number = ParseExactDecimal(text);
		if (!number)
		{
			// Very small and very large magnitudes: nlohmann's own reading, as FloatToJson does.
			const nlohmann::json parsed = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
			if (!parsed.is_number_float())
			{
				out += JsonUtils::Dump(FloatToJson(value));
				return;
			}
			number = parsed.get<double>();
		}
		char digits[64];
		const char* end = nlohmann::detail::to_chars(digits, digits + sizeof(digits), *number);
		out.append(digits, static_cast<size_t>(end - digits));
	}

	void AppendPropertyValueJson(std::string& out, const PropertyInfo& property, const PropertyValue& value)
	{
		switch (property.Type)
		{
			case PropertyType::Bool:   out += std::get<bool>(value) ? "true" : "false"; return;
			case PropertyType::Int:    AppendInteger(out, std::get<int32_t>(value)); return;
			case PropertyType::UInt:   AppendInteger(out, std::get<uint32_t>(value)); return;
			case PropertyType::Float:  AppendFloatJson(out, std::get<float>(value)); return;
			case PropertyType::Vec2:   AppendVector(out, std::get<glm::vec2>(value)); return;
			case PropertyType::Vec3:
			case PropertyType::Color3: AppendVector(out, std::get<glm::vec3>(value)); return;
			case PropertyType::Vec4:
			case PropertyType::Color4: AppendVector(out, std::get<glm::vec4>(value)); return;
			case PropertyType::Quat:
			{
				const glm::quat& rotation = std::get<glm::quat>(value);
				AppendVector(out, glm::vec4(rotation.x, rotation.y, rotation.z, rotation.w));
				return;
			}
			case PropertyType::String: JsonUtils::AppendString(out, std::get<std::string>(value)); return;
			case PropertyType::Enum:
			{
				const int32_t enumValue = std::get<int32_t>(value);
				if (const EnumValue* option = property.FindEnumValue(enumValue))
					JsonUtils::AppendString(out, option->Name);
				else
					AppendInteger(out, enumValue);
				return;
			}
			case PropertyType::Asset:
			case PropertyType::Entity: JsonUtils::AppendString(out, std::get<UUID>(value).ToString()); return;
		}
		out += "null";
	}

	nlohmann::json FloatToJson(float value)
	{
		if (!std::isfinite(value))
			return 0.0;

		// fmt prints the shortest decimal string that round-trips to the same float; the double parsed from it is
		// what nlohmann::json then prints (it also uses shortest round-trip formatting for doubles). The string is
		// parsed with nlohmann's lexer, which is independent of the C locale (strtod is not: a toolkit calling
		// setlocale could make it ignore the '.').
		char buffer[64];
		const auto result = fmt::format_to_n(buffer, sizeof(buffer), "{}", value);
		const size_t length = std::min(static_cast<size_t>(result.size), sizeof(buffer));
		nlohmann::json parsed = nlohmann::json::parse(buffer, buffer + length, nullptr, false);
		if (parsed.is_number())
			return parsed;
		return static_cast<double>(value);
	}

	nlohmann::json UUIDToJson(UUID uuid)
	{
		return uuid.ToString();
	}

	std::optional<UUID> UUIDFromJson(const nlohmann::json& json)
	{
		if (json.is_null())
			return UUID::Null();
		if (json.is_number_unsigned() || json.is_number_integer())
			return UUID(json.get<uint64_t>());
		if (json.is_string())
		{
			const std::string& text = json.get_ref<const std::string&>();
			if (text.empty())
				return UUID::Null();
			return UUID::FromString(text);
		}
		return std::nullopt;
	}

	nlohmann::json PropertyValueToJson(const PropertyInfo& property, const PropertyValue& value)
	{
		switch (property.Type)
		{
			case PropertyType::Bool:   return std::get<bool>(value);
			case PropertyType::Int:    return std::get<int32_t>(value);
			case PropertyType::UInt:   return std::get<uint32_t>(value);
			case PropertyType::Float:  return FloatToJson(std::get<float>(value));
			case PropertyType::Vec2:   return VectorToJson(std::get<glm::vec2>(value));
			case PropertyType::Vec3:
			case PropertyType::Color3: return VectorToJson(std::get<glm::vec3>(value));
			case PropertyType::Vec4:
			case PropertyType::Color4: return VectorToJson(std::get<glm::vec4>(value));
			case PropertyType::Quat:
			{
				const glm::quat& rotation = std::get<glm::quat>(value);
				return nlohmann::json::array({ FloatToJson(rotation.x), FloatToJson(rotation.y), FloatToJson(rotation.z), FloatToJson(rotation.w) });
			}
			case PropertyType::String: return std::get<std::string>(value);
			case PropertyType::Enum:
			{
				const int32_t enumValue = std::get<int32_t>(value);
				if (const EnumValue* option = property.FindEnumValue(enumValue))
					return option->Name;
				return enumValue;
			}
			case PropertyType::Asset:
			case PropertyType::Entity: return UUIDToJson(std::get<UUID>(value));
		}
		return nullptr;
	}

	std::optional<PropertyValue> PropertyValueFromJson(const PropertyInfo& property, const nlohmann::json& json, std::string* outError)
	{
		std::string error;
		auto fail = [&](const std::string& reason) -> std::optional<PropertyValue>
		{
			if (outError)
				*outError = fmt::format("Invalid value for property '{}' ({}): {}", property.Name, PropertyTypeToString(property.Type), reason);
			return std::nullopt;
		};

		switch (property.Type)
		{
			case PropertyType::Bool:
				if (json.is_boolean())
					return json.get<bool>();
				if (json.is_number_integer())
					return json.get<int64_t>() != 0;
				return fail("expected true or false");

			case PropertyType::Int:
				if (json.is_number_integer())
				{
					const int64_t value = json.get<int64_t>();
					if (value < INT32_MIN || value > INT32_MAX)
						return fail("integer out of range");
					return static_cast<int32_t>(value);
				}
				if (json.is_number_float())
				{
					const double value = json.get<double>();
					if (value != std::floor(value) || value < INT32_MIN || value > INT32_MAX)
						return fail("expected an integer");
					return static_cast<int32_t>(value);
				}
				return fail("expected an integer");

			case PropertyType::UInt:
				if (json.is_number_unsigned() || (json.is_number_integer() && json.get<int64_t>() >= 0))
				{
					const uint64_t value = json.get<uint64_t>();
					if (value > UINT32_MAX)
						return fail("integer out of range");
					return static_cast<uint32_t>(value);
				}
				return fail("expected a non-negative integer");

			case PropertyType::Float:
				if (std::optional<float> number = ReadNumber(json))
					return *number;
				return fail("expected a finite number within float range");

			case PropertyType::Vec2:
				if (std::optional<glm::vec2> vector = ReadVector<2>(json, false, &error))
					return *vector;
				return fail(error);

			case PropertyType::Vec3:
			case PropertyType::Color3:
				if (std::optional<glm::vec3> vector = ReadVector<3>(json, property.Type == PropertyType::Color3, &error))
					return *vector;
				return fail(error);

			case PropertyType::Vec4:
			case PropertyType::Color4:
				if (std::optional<glm::vec4> vector = ReadVector<4>(json, property.Type == PropertyType::Color4, &error))
					return *vector;
				return fail(error);

			case PropertyType::Quat:
			{
				const nlohmann::json* eulerJson = nullptr;
				if (json.is_object() && json.contains("Euler"))
					eulerJson = &json["Euler"];
				else if (json.is_array() && json.size() == 3)
					eulerJson = &json;

				if (eulerJson)
				{
					std::optional<glm::vec3> euler = ReadVector<3>(*eulerJson, false, &error);
					if (!euler)
						return fail(error);
					return Math::EulerDegreesToQuat(*euler);
				}

				std::optional<glm::vec4> components = ReadVector<4>(json, false, &error);
				if (!components)
					return fail("expected [x, y, z, w], Euler degrees [pitch, yaw, roll] or {\"Euler\": [...]}");
				return glm::quat(components->w, components->x, components->y, components->z);
			}

			case PropertyType::String:
				if (json.is_string())
					return json.get<std::string>();
				return fail("expected a string");

			case PropertyType::Enum:
				if (json.is_string())
				{
					if (const EnumValue* option = property.FindEnumValue(json.get_ref<const std::string&>()))
						return option->Value;

					std::string options;
					for (const EnumValue& option : property.EnumValues)
						options += (options.empty() ? "" : ", ") + option.Name;
					return fail(fmt::format("unknown option '{}' (valid: {})", json.get<std::string>(), options));
				}
				if (json.is_number_integer())
					return static_cast<int32_t>(json.get<int64_t>());
				return fail("expected an option name");

			case PropertyType::Asset:
			case PropertyType::Entity:
				if (std::optional<UUID> uuid = UUIDFromJson(json))
					return *uuid;
				return fail("expected a UUID (16 hex digits), an integer, or null");
		}
		return fail("unsupported property type");
	}

	nlohmann::json DescribeProperty(const PropertyInfo& property)
	{
		nlohmann::json description;
		description["Name"] = property.Name;
		description["DisplayName"] = property.DisplayName;
		description["Type"] = PropertyTypeToString(property.Type);
		if (!property.Tooltip.empty())
			description["Description"] = property.Tooltip;
		if (property.HasRange())
		{
			description["Min"] = property.Min;
			description["Max"] = property.Max;
		}
		if (property.Type == PropertyType::Asset)
			description["AssetType"] = AssetTypeToString(property.AssetFilter);
		if (property.Type == PropertyType::Enum)
		{
			nlohmann::json options = nlohmann::json::array();
			for (const EnumValue& option : property.EnumValues)
				options.push_back(option.Name);
			description["Options"] = options;
		}
		if (property.IsReadOnly())
			description["ReadOnly"] = true;
		if (HasFlag(property.Flags, PropertyFlags::PhysicsLayer))
			description["PhysicsLayer"] = true;
		return description;
	}

}
