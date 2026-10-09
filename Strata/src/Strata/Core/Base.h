#pragma once

#include "Strata/Core/PlatformDetection.h"

#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#if !defined(ST_DIST)
	#define ST_ENABLE_ASSERTS
#endif

#if defined(ST_PLATFORM_WINDOWS)
	#define ST_DEBUGBREAK() __debugbreak()
#elif defined(__clang__) || defined(__GNUC__)
	#define ST_DEBUGBREAK() __builtin_trap()
#else
	#include <csignal>
	#define ST_DEBUGBREAK() std::raise(SIGTRAP)
#endif

#define ST_EXPAND_MACRO(x) x
#define ST_STRINGIFY_MACRO(x) #x

#define ST_BIT(x) (1u << (x))

#define ST_BIND_EVENT_FN(fn) [this](auto&&... args) -> decltype(auto) { return this->fn(std::forward<decltype(args)>(args)...); }

// Bitwise operators for an `enum class` used as a set of flags.
#define ST_DEFINE_ENUM_FLAG_OPERATORS(EnumType) \
	constexpr EnumType operator|(EnumType a, EnumType b) { return static_cast<EnumType>(static_cast<std::underlying_type_t<EnumType>>(a) | static_cast<std::underlying_type_t<EnumType>>(b)); } \
	constexpr EnumType operator&(EnumType a, EnumType b) { return static_cast<EnumType>(static_cast<std::underlying_type_t<EnumType>>(a) & static_cast<std::underlying_type_t<EnumType>>(b)); } \
	constexpr EnumType operator~(EnumType a) { return static_cast<EnumType>(~static_cast<std::underlying_type_t<EnumType>>(a)); } \
	constexpr EnumType& operator|=(EnumType& a, EnumType b) { return a = a | b; } \
	constexpr EnumType& operator&=(EnumType& a, EnumType b) { return a = a & b; } \
	constexpr bool HasFlag(EnumType value, EnumType flag) { return (value & flag) == flag; }

namespace Strata
{

	template<typename T>
	using Scope = std::unique_ptr<T>;

	template<typename T, typename... Args>
	constexpr Scope<T> CreateScope(Args&&... args)
	{
		return std::make_unique<T>(std::forward<Args>(args)...);
	}

	template<typename T>
	using Ref = std::shared_ptr<T>;

	template<typename T, typename... Args>
	constexpr Ref<T> CreateRef(Args&&... args)
	{
		return std::make_shared<T>(std::forward<Args>(args)...);
	}

	template<typename T>
	using WeakRef = std::weak_ptr<T>;

}
