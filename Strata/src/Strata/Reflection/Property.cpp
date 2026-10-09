#include "stpch.h"
#include "Strata/Reflection/Property.h"

#include <cctype>

namespace Strata
{

	const char* PropertyTypeToString(PropertyType type)
	{
		switch (type)
		{
			case PropertyType::Bool:   return "Bool";
			case PropertyType::Int:    return "Int";
			case PropertyType::UInt:   return "UInt";
			case PropertyType::Float:  return "Float";
			case PropertyType::Vec2:   return "Vec2";
			case PropertyType::Vec3:   return "Vec3";
			case PropertyType::Vec4:   return "Vec4";
			case PropertyType::Quat:   return "Quat";
			case PropertyType::Color3: return "Color3";
			case PropertyType::Color4: return "Color4";
			case PropertyType::String: return "String";
			case PropertyType::Enum:   return "Enum";
			case PropertyType::Asset:  return "Asset";
			case PropertyType::Entity: return "Entity";
		}
		return "Unknown";
	}

	std::optional<PropertyType> PropertyTypeFromString(std::string_view text)
	{
		constexpr PropertyType types[] = {
			PropertyType::Bool, PropertyType::Int, PropertyType::UInt, PropertyType::Float, PropertyType::Vec2,
			PropertyType::Vec3, PropertyType::Vec4, PropertyType::Quat, PropertyType::Color3, PropertyType::Color4,
			PropertyType::String, PropertyType::Enum, PropertyType::Asset, PropertyType::Entity
		};
		for (PropertyType type : types)
		{
			if (text == PropertyTypeToString(type))
				return type;
		}
		return std::nullopt;
	}

	// GetPropertyValueIndex relies on this exact alternative order.
	static_assert(std::is_same_v<std::variant_alternative_t<0, PropertyValue>, bool>);
	static_assert(std::is_same_v<std::variant_alternative_t<1, PropertyValue>, int32_t>);
	static_assert(std::is_same_v<std::variant_alternative_t<2, PropertyValue>, uint32_t>);
	static_assert(std::is_same_v<std::variant_alternative_t<3, PropertyValue>, float>);
	static_assert(std::is_same_v<std::variant_alternative_t<4, PropertyValue>, glm::vec2>);
	static_assert(std::is_same_v<std::variant_alternative_t<5, PropertyValue>, glm::vec3>);
	static_assert(std::is_same_v<std::variant_alternative_t<6, PropertyValue>, glm::vec4>);
	static_assert(std::is_same_v<std::variant_alternative_t<7, PropertyValue>, glm::quat>);
	static_assert(std::is_same_v<std::variant_alternative_t<8, PropertyValue>, std::string>);
	static_assert(std::is_same_v<std::variant_alternative_t<9, PropertyValue>, UUID>);

	size_t GetPropertyValueIndex(PropertyType type)
	{
		switch (type)
		{
			case PropertyType::Bool:   return 0;
			case PropertyType::Int:    return 1;
			case PropertyType::Enum:   return 1;
			case PropertyType::UInt:   return 2;
			case PropertyType::Float:  return 3;
			case PropertyType::Vec2:   return 4;
			case PropertyType::Vec3:   return 5;
			case PropertyType::Color3: return 5;
			case PropertyType::Vec4:   return 6;
			case PropertyType::Color4: return 6;
			case PropertyType::Quat:   return 7;
			case PropertyType::String: return 8;
			case PropertyType::Asset:  return 9;
			case PropertyType::Entity: return 9;
		}
		return 0;
	}

	PropertyValue GetDefaultPropertyValue(PropertyType type)
	{
		switch (type)
		{
			case PropertyType::Bool:   return false;
			case PropertyType::Int:    return int32_t(0);
			case PropertyType::Enum:   return int32_t(0);
			case PropertyType::UInt:   return uint32_t(0);
			case PropertyType::Float:  return 0.0f;
			case PropertyType::Vec2:   return glm::vec2(0.0f);
			case PropertyType::Vec3:   return glm::vec3(0.0f);
			case PropertyType::Color3: return glm::vec3(1.0f);
			case PropertyType::Vec4:   return glm::vec4(0.0f);
			case PropertyType::Color4: return glm::vec4(1.0f);
			case PropertyType::Quat:   return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			case PropertyType::String: return std::string();
			case PropertyType::Asset:  return UUID::Null();
			case PropertyType::Entity: return UUID::Null();
		}
		return false;
	}

	static bool EqualsIgnoreCase(std::string_view a, std::string_view b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t index = 0; index < a.size(); index++)
		{
			if (std::tolower(static_cast<unsigned char>(a[index])) != std::tolower(static_cast<unsigned char>(b[index])))
				return false;
		}
		return true;
	}

	const EnumValue* PropertyInfo::FindEnumValue(std::string_view name) const
	{
		for (const EnumValue& value : EnumValues)
		{
			if (EqualsIgnoreCase(value.Name, name))
				return &value;
		}
		return nullptr;
	}

	const EnumValue* PropertyInfo::FindEnumValue(int32_t value) const
	{
		for (const EnumValue& enumValue : EnumValues)
		{
			if (enumValue.Value == value)
				return &enumValue;
		}
		return nullptr;
	}

	bool PropertyInfo::SetValue(void* object, const PropertyValue& value, std::string* outError) const
	{
		if (value.index() != GetPropertyValueIndex(Type))
		{
			if (outError)
				*outError = fmt::format("Property '{}' expects a value of type {}", Name, PropertyTypeToString(Type));
			return false;
		}

		PropertyValue finalValue = value;
		switch (Type)
		{
			case PropertyType::Enum:
			{
				const int32_t enumValue = std::get<int32_t>(value);
				if (!FindEnumValue(enumValue))
				{
					if (outError)
						*outError = fmt::format("Value {} is not a valid option for property '{}'", enumValue, Name);
					return false;
				}
				break;
			}
			case PropertyType::Int:
				if (HasRange())
					finalValue = std::clamp(std::get<int32_t>(value), static_cast<int32_t>(Min), static_cast<int32_t>(Max));
				break;
			case PropertyType::UInt:
				if (HasRange())
					finalValue = std::clamp(std::get<uint32_t>(value), static_cast<uint32_t>(std::max(Min, 0.0f)), static_cast<uint32_t>(std::max(Max, 0.0f)));
				break;
			case PropertyType::Float:
			{
				const float floatValue = std::get<float>(value);
				if (!std::isfinite(floatValue))
				{
					if (outError)
						*outError = fmt::format("Property '{}' must be a finite number", Name);
					return false;
				}
				if (HasRange())
					finalValue = std::clamp(floatValue, Min, Max);
				break;
			}
			case PropertyType::Vec2:
			case PropertyType::Vec3:
			case PropertyType::Vec4:
			case PropertyType::Color3:
			case PropertyType::Color4:
			{
				bool finite = true;
				std::visit([&](auto& vector)
				{
					using VectorType = std::decay_t<decltype(vector)>;
					if constexpr (std::is_same_v<VectorType, glm::vec2> || std::is_same_v<VectorType, glm::vec3> || std::is_same_v<VectorType, glm::vec4>)
					{
						for (glm::length_t index = 0; index < VectorType::length(); index++)
						{
							finite &= std::isfinite(vector[index]);
							if (HasRange())
								vector[index] = std::clamp(vector[index], Min, Max);
						}
					}
				}, finalValue);
				if (!finite)
				{
					if (outError)
						*outError = fmt::format("Property '{}' components must be finite numbers", Name);
					return false;
				}
				break;
			}
			case PropertyType::Quat:
			{
				const glm::quat rotation = std::get<glm::quat>(value);
				const float length = glm::length(rotation);
				if (!std::isfinite(length) || length < 1e-6f)
				{
					if (outError)
						*outError = fmt::format("Property '{}' requires a non-zero rotation quaternion", Name);
					return false;
				}
				// Only correct rotations that are noticeably off unit length, so that values which already are
				// (within float precision) survive save/load round trips bit-exactly.
				if (glm::abs(length - 1.0f) > 1e-4f)
					finalValue = rotation / length;
				break;
			}
			default:
				break;
		}

		Setter(object, finalValue);
		return true;
	}

	namespace Utils
	{

		std::string PascalCaseToDisplayName(std::string_view name)
		{
			std::string result;
			result.reserve(name.size() + 4);
			for (size_t index = 0; index < name.size(); index++)
			{
				const char character = name[index];
				if (index > 0 && std::isupper(static_cast<unsigned char>(character)))
				{
					const char previous = name[index - 1];
					const bool previousLower = std::islower(static_cast<unsigned char>(previous)) || std::isdigit(static_cast<unsigned char>(previous));
					const bool nextLower = index + 1 < name.size() && std::islower(static_cast<unsigned char>(name[index + 1]));
					// Break before an upper-case letter that starts a word: "aB" or the last capital of an acronym "ABc".
					if (previousLower || (std::isupper(static_cast<unsigned char>(previous)) && nextLower))
						result.push_back(' ');
				}
				result.push_back(character);
			}
			return result;
		}

	}

}
