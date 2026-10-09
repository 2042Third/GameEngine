#include "stpch.h"
#include "Strata/Scripting/ScriptValue.h"

#include <limits>

namespace Strata
{

	namespace
	{

		template<typename Vector>
		void WriteVector(StrataScriptValue& value, const Vector& vector)
		{
			for (glm::length_t index = 0; index < Vector::length(); index++)
				value.As.Vector[index] = vector[index];
		}

		StrataScriptValue MakeValue(uint32_t type)
		{
			StrataScriptValue value = {};
			value.Type = type;
			return value;
		}

		glm::quat ReadQuat(const StrataScriptValue& value)
		{
			return glm::quat(value.As.Vector[3], value.As.Vector[0], value.As.Vector[1], value.As.Vector[2]);
		}

	}

	bool ReadScriptString(const StrataScriptString& text, std::string& out)
	{
		if (text.Size == 0)
		{
			out.clear();
			return true;
		}
		if (!text.Data || text.Size > c_MaxScriptStringSize)
			return false;
		out.assign(text.Data, static_cast<size_t>(text.Size));
		return true;
	}

	std::string_view ScriptStringView(const StrataScriptString& text)
	{
		if (!text.Data || text.Size == 0 || text.Size > c_MaxScriptStringSize)
			return {};
		return std::string_view(text.Data, static_cast<size_t>(text.Size));
	}

	std::optional<PropertyType> ScriptFieldTypeToPropertyType(uint32_t valueType)
	{
		switch (valueType)
		{
			case StrataScriptValueType_Bool:   return PropertyType::Bool;
			case StrataScriptValueType_Int:    return PropertyType::Int;
			case StrataScriptValueType_Float:  return PropertyType::Float;
			case StrataScriptValueType_Vec2:   return PropertyType::Vec2;
			case StrataScriptValueType_Vec3:   return PropertyType::Vec3;
			case StrataScriptValueType_Vec4:   return PropertyType::Vec4;
			case StrataScriptValueType_Quat:   return PropertyType::Quat;
			case StrataScriptValueType_String: return PropertyType::String;
			case StrataScriptValueType_Entity: return PropertyType::Entity;
			case StrataScriptValueType_Asset:  return PropertyType::Asset;
			default:                           return std::nullopt;
		}
	}

	const char* ScriptValueTypeToString(uint32_t valueType)
	{
		switch (valueType)
		{
			case StrataScriptValueType_Empty:  return "Empty";
			case StrataScriptValueType_Bool:   return "Bool";
			case StrataScriptValueType_Int:    return "Int";
			case StrataScriptValueType_Float:  return "Float";
			case StrataScriptValueType_Vec2:   return "Vec2";
			case StrataScriptValueType_Vec3:   return "Vec3";
			case StrataScriptValueType_Vec4:   return "Vec4";
			case StrataScriptValueType_Quat:   return "Quat";
			case StrataScriptValueType_String: return "String";
			case StrataScriptValueType_Entity: return "Entity";
			case StrataScriptValueType_Asset:  return "Asset";
			default:                           return "Unknown";
		}
	}

	std::optional<PropertyValue> ScriptValueToFieldValue(const StrataScriptValue& value, PropertyType fieldType)
	{
		const std::optional<PropertyType> valueType = ScriptFieldTypeToPropertyType(value.Type);
		if (!valueType || *valueType != fieldType)
			return std::nullopt;

		switch (fieldType)
		{
			case PropertyType::Bool:
				return PropertyValue(value.As.Bool);
			case PropertyType::Int:
				if (value.As.Int < std::numeric_limits<int32_t>::min() || value.As.Int > std::numeric_limits<int32_t>::max())
					return std::nullopt;
				return PropertyValue(static_cast<int32_t>(value.As.Int));
			case PropertyType::Float:
				return PropertyValue(value.As.Float);
			case PropertyType::Vec2:
				return PropertyValue(glm::vec2(value.As.Vector[0], value.As.Vector[1]));
			case PropertyType::Vec3:
				return PropertyValue(glm::vec3(value.As.Vector[0], value.As.Vector[1], value.As.Vector[2]));
			case PropertyType::Vec4:
				return PropertyValue(glm::vec4(value.As.Vector[0], value.As.Vector[1], value.As.Vector[2], value.As.Vector[3]));
			case PropertyType::Quat:
				return PropertyValue(ReadQuat(value));
			case PropertyType::String:
			{
				std::string text;
				if (!ReadScriptString(value.As.String, text))
					return std::nullopt;
				return PropertyValue(std::move(text));
			}
			case PropertyType::Entity:
			case PropertyType::Asset:
				return PropertyValue(UUID(value.As.ID));
			default:
				return std::nullopt;
		}
	}

	StrataScriptValue FieldValueToScriptValue(const PropertyValue& value, PropertyType fieldType)
	{
		return PropertyValueToScriptValue(value, fieldType);
	}

	std::optional<PropertyValue> ScriptValueToPropertyValue(const StrataScriptValue& value, const PropertyInfo& property, std::string* outError)
	{
		auto mismatch = [&]() -> std::optional<PropertyValue>
		{
			if (outError)
				*outError = fmt::format("Property '{}' is {}; a {} value cannot be assigned", property.Name, PropertyTypeToString(property.Type), ScriptValueTypeToString(value.Type));
			return std::nullopt;
		};
		auto outOfRange = [&]() -> std::optional<PropertyValue>
		{
			if (outError)
				*outError = fmt::format("Value {} is out of range for property '{}'", value.As.Int, property.Name);
			return std::nullopt;
		};

		switch (property.Type)
		{
			case PropertyType::Bool:
				if (value.Type != StrataScriptValueType_Bool)
					return mismatch();
				return PropertyValue(value.As.Bool);
			case PropertyType::Int:
			case PropertyType::Enum:
				if (value.Type != StrataScriptValueType_Int)
					return mismatch();
				if (value.As.Int < std::numeric_limits<int32_t>::min() || value.As.Int > std::numeric_limits<int32_t>::max())
					return outOfRange();
				return PropertyValue(static_cast<int32_t>(value.As.Int));
			case PropertyType::UInt:
				if (value.Type != StrataScriptValueType_Int)
					return mismatch();
				if (value.As.Int < 0 || value.As.Int > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
					return outOfRange();
				return PropertyValue(static_cast<uint32_t>(value.As.Int));
			case PropertyType::Float:
				if (value.Type == StrataScriptValueType_Int)
					return PropertyValue(static_cast<float>(value.As.Int));
				if (value.Type != StrataScriptValueType_Float)
					return mismatch();
				return PropertyValue(value.As.Float);
			case PropertyType::Vec2:
				if (value.Type != StrataScriptValueType_Vec2)
					return mismatch();
				return PropertyValue(glm::vec2(value.As.Vector[0], value.As.Vector[1]));
			case PropertyType::Vec3:
			case PropertyType::Color3:
				if (value.Type != StrataScriptValueType_Vec3)
					return mismatch();
				return PropertyValue(glm::vec3(value.As.Vector[0], value.As.Vector[1], value.As.Vector[2]));
			case PropertyType::Vec4:
			case PropertyType::Color4:
				if (value.Type != StrataScriptValueType_Vec4)
					return mismatch();
				return PropertyValue(glm::vec4(value.As.Vector[0], value.As.Vector[1], value.As.Vector[2], value.As.Vector[3]));
			case PropertyType::Quat:
				// Quaternions travel as x, y, z, w either way.
				if (value.Type != StrataScriptValueType_Quat && value.Type != StrataScriptValueType_Vec4)
					return mismatch();
				return PropertyValue(ReadQuat(value));
			case PropertyType::String:
			{
				if (value.Type != StrataScriptValueType_String)
					return mismatch();
				std::string text;
				if (!ReadScriptString(value.As.String, text))
				{
					if (outError)
						*outError = fmt::format("Invalid string for property '{}'", property.Name);
					return std::nullopt;
				}
				return PropertyValue(std::move(text));
			}
			case PropertyType::Asset:
				if (value.Type != StrataScriptValueType_Asset)
					return mismatch();
				return PropertyValue(UUID(value.As.ID));
			case PropertyType::Entity:
				if (value.Type != StrataScriptValueType_Entity)
					return mismatch();
				return PropertyValue(UUID(value.As.ID));
		}
		return mismatch();
	}

	StrataScriptValue PropertyValueToScriptValue(const PropertyValue& value, PropertyType propertyType)
	{
		// The value always holds the alternative of its property type (PropertyInfo guarantees it); the index check keeps
		// a mismatch from ever reading the wrong alternative.
		if (value.index() != GetPropertyValueIndex(propertyType))
			return MakeValue(StrataScriptValueType_Empty);

		switch (propertyType)
		{
			case PropertyType::Bool:
			{
				StrataScriptValue result = MakeValue(StrataScriptValueType_Bool);
				result.As.Bool = std::get<bool>(value);
				return result;
			}
			case PropertyType::Int:
			case PropertyType::Enum:
			{
				StrataScriptValue result = MakeValue(StrataScriptValueType_Int);
				result.As.Int = std::get<int32_t>(value);
				return result;
			}
			case PropertyType::UInt:
			{
				StrataScriptValue result = MakeValue(StrataScriptValueType_Int);
				result.As.Int = std::get<uint32_t>(value);
				return result;
			}
			case PropertyType::Float:
			{
				StrataScriptValue result = MakeValue(StrataScriptValueType_Float);
				result.As.Float = std::get<float>(value);
				return result;
			}
			case PropertyType::Vec2:
			{
				StrataScriptValue result = MakeValue(StrataScriptValueType_Vec2);
				WriteVector(result, std::get<glm::vec2>(value));
				return result;
			}
			case PropertyType::Vec3:
			case PropertyType::Color3:
			{
				StrataScriptValue result = MakeValue(StrataScriptValueType_Vec3);
				WriteVector(result, std::get<glm::vec3>(value));
				return result;
			}
			case PropertyType::Vec4:
			case PropertyType::Color4:
			{
				StrataScriptValue result = MakeValue(StrataScriptValueType_Vec4);
				WriteVector(result, std::get<glm::vec4>(value));
				return result;
			}
			case PropertyType::Quat:
			{
				const glm::quat& rotation = std::get<glm::quat>(value);
				StrataScriptValue result = MakeValue(StrataScriptValueType_Quat);
				result.As.Vector[0] = rotation.x;
				result.As.Vector[1] = rotation.y;
				result.As.Vector[2] = rotation.z;
				result.As.Vector[3] = rotation.w;
				return result;
			}
			case PropertyType::String:
			{
				const std::string& text = std::get<std::string>(value);
				StrataScriptValue result = MakeValue(StrataScriptValueType_String);
				result.As.String = StrataScriptString { text.data(), static_cast<uint64_t>(text.size()) };
				return result;
			}
			case PropertyType::Asset:
			{
				StrataScriptValue result = MakeValue(StrataScriptValueType_Asset);
				result.As.ID = static_cast<uint64_t>(std::get<UUID>(value));
				return result;
			}
			case PropertyType::Entity:
			{
				StrataScriptValue result = MakeValue(StrataScriptValueType_Entity);
				result.As.ID = static_cast<uint64_t>(std::get<UUID>(value));
				return result;
			}
		}
		return MakeValue(StrataScriptValueType_Empty);
	}

}
