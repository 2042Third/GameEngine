#pragma once

#include "Strata/Core/PlatformDetection.h"

#include <cstdint>
#include <memory>
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
