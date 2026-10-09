#pragma once

#include "StrataScript/Host.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>

namespace Strata
{

	// Reference to an asset (prefab, model, mesh, material, ...) by its stable handle. 0 is the null asset.
	struct AssetHandle
	{
		uint64_t ID = 0;

		constexpr AssetHandle() = default;
		constexpr explicit AssetHandle(uint64_t id)
			: ID(id)
		{
		}

		constexpr bool IsValid() const { return ID != 0; }
		constexpr explicit operator bool() const { return IsValid(); }
		constexpr bool operator==(const AssetHandle& other) const = default;
	};

	namespace Detail
	{

		// Conversion of C++ values to and from StrataScriptValue. Specialized for every supported value type:
		//   static constexpr uint32_t Type;                                 the StrataScriptValueType
		//   static StrataScriptValue ToValue(const T& value);               strings point into `value`
		//   static bool FromValue(const StrataScriptValue& value, T& out);  false if the value has another type
		template<typename T>
		struct ValueTraits;

		template<>
		struct ValueTraits<bool>
		{
			static constexpr uint32_t Type = StrataScriptValueType_Bool;

			static StrataScriptValue ToValue(bool value)
			{
				StrataScriptValue result = {};
				result.Type = Type;
				result.As.Bool = value;
				return result;
			}

			static bool FromValue(const StrataScriptValue& value, bool& out)
			{
				if (value.Type != Type)
					return false;
				out = value.As.Bool;
				return true;
			}
		};

		// Integers travel as int64_t; conversions to narrower types fail when the value does not fit.
		template<typename Integer>
		struct IntegerValueTraits
		{
			static constexpr uint32_t Type = StrataScriptValueType_Int;

			static StrataScriptValue ToValue(Integer value)
			{
				StrataScriptValue result = {};
				result.Type = Type;
				result.As.Int = static_cast<int64_t>(value);
				return result;
			}

			static bool FromValue(const StrataScriptValue& value, Integer& out)
			{
				if (value.Type != Type)
					return false;
				const int64_t integer = value.As.Int;
				if constexpr (std::is_signed_v<Integer>)
				{
					if (integer < static_cast<int64_t>(std::numeric_limits<Integer>::min()) || integer > static_cast<int64_t>(std::numeric_limits<Integer>::max()))
						return false;
				}
				else
				{
					if (integer < 0 || static_cast<uint64_t>(integer) > static_cast<uint64_t>(std::numeric_limits<Integer>::max()))
						return false;
				}
				out = static_cast<Integer>(integer);
				return true;
			}
		};

		template<>
		struct ValueTraits<int32_t> : IntegerValueTraits<int32_t>
		{
		};

		template<>
		struct ValueTraits<uint32_t> : IntegerValueTraits<uint32_t>
		{
		};

		template<>
		struct ValueTraits<int64_t> : IntegerValueTraits<int64_t>
		{
		};

		template<>
		struct ValueTraits<float>
		{
			static constexpr uint32_t Type = StrataScriptValueType_Float;

			static StrataScriptValue ToValue(float value)
			{
				StrataScriptValue result = {};
				result.Type = Type;
				result.As.Float = value;
				return result;
			}

			// Integers are accepted too (component properties report what they store; scripts often pass literals).
			static bool FromValue(const StrataScriptValue& value, float& out)
			{
				if (value.Type == StrataScriptValueType_Int)
				{
					out = static_cast<float>(value.As.Int);
					return true;
				}
				if (value.Type != Type)
					return false;
				out = value.As.Float;
				return true;
			}
		};

		template<typename Vector, uint32_t ValueType>
		struct VectorValueTraits
		{
			static constexpr uint32_t Type = ValueType;

			static StrataScriptValue ToValue(const Vector& value)
			{
				StrataScriptValue result = {};
				result.Type = Type;
				for (glm::length_t index = 0; index < Vector::length(); index++)
					result.As.Vector[index] = value[index];
				return result;
			}

			static bool FromValue(const StrataScriptValue& value, Vector& out)
			{
				if (value.Type != Type)
					return false;
				for (glm::length_t index = 0; index < Vector::length(); index++)
					out[index] = value.As.Vector[index];
				return true;
			}
		};

		template<>
		struct ValueTraits<glm::vec2> : VectorValueTraits<glm::vec2, StrataScriptValueType_Vec2>
		{
		};

		template<>
		struct ValueTraits<glm::vec3> : VectorValueTraits<glm::vec3, StrataScriptValueType_Vec3>
		{
		};

		template<>
		struct ValueTraits<glm::vec4> : VectorValueTraits<glm::vec4, StrataScriptValueType_Vec4>
		{
		};

		template<>
		struct ValueTraits<glm::quat>
		{
			static constexpr uint32_t Type = StrataScriptValueType_Quat;

			static StrataScriptValue ToValue(const glm::quat& value)
			{
				StrataScriptValue result = {};
				result.Type = Type;
				result.As.Vector[0] = value.x;
				result.As.Vector[1] = value.y;
				result.As.Vector[2] = value.z;
				result.As.Vector[3] = value.w;
				return result;
			}

			static bool FromValue(const StrataScriptValue& value, glm::quat& out)
			{
				if (value.Type != Type)
					return false;
				out = glm::quat(value.As.Vector[3], value.As.Vector[0], value.As.Vector[1], value.As.Vector[2]);
				return true;
			}
		};

		template<>
		struct ValueTraits<std::string>
		{
			static constexpr uint32_t Type = StrataScriptValueType_String;

			static StrataScriptValue ToValue(const std::string& value)
			{
				StrataScriptValue result = {};
				result.Type = Type;
				result.As.String = ToABIString(value);
				return result;
			}

			static bool FromValue(const StrataScriptValue& value, std::string& out)
			{
				if (value.Type != Type)
					return false;
				out = std::string(FromABIString(value.As.String));
				return true;
			}
		};

		template<>
		struct ValueTraits<AssetHandle>
		{
			static constexpr uint32_t Type = StrataScriptValueType_Asset;

			static StrataScriptValue ToValue(const AssetHandle& value)
			{
				StrataScriptValue result = {};
				result.Type = Type;
				result.As.ID = value.ID;
				return result;
			}

			static bool FromValue(const StrataScriptValue& value, AssetHandle& out)
			{
				if (value.Type != Type)
					return false;
				out = AssetHandle(value.As.ID);
				return true;
			}
		};

		inline void ToABIVector3(const glm::vec3& vector, float out[3])
		{
			out[0] = vector.x;
			out[1] = vector.y;
			out[2] = vector.z;
		}

		inline void ToABIQuat(const glm::quat& rotation, float out[4])
		{
			out[0] = rotation.x;
			out[1] = rotation.y;
			out[2] = rotation.z;
			out[3] = rotation.w;
		}

		inline glm::vec3 FromABIVector3(const float vector[3])
		{
			return glm::vec3(vector[0], vector[1], vector[2]);
		}

		inline glm::quat FromABIQuat(const float rotation[4])
		{
			return glm::quat(rotation[3], rotation[0], rotation[1], rotation[2]);
		}

	}

}
