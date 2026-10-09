#pragma once

// Platform detection. Exactly one ST_PLATFORM_* macro is defined for supported targets.

#if defined(_WIN32)
	#if defined(_WIN64)
		#define ST_PLATFORM_WINDOWS
	#else
		#error "Strata requires a 64-bit Windows target"
	#endif
#elif defined(__APPLE__) && defined(__MACH__)
	#include <TargetConditionals.h>
	#if TARGET_OS_OSX
		#define ST_PLATFORM_MACOS
	#else
		#error "Unsupported Apple platform: Strata supports macOS only"
	#endif
#elif defined(__linux__)
	#define ST_PLATFORM_LINUX
#else
	#error "Unsupported platform: Strata supports Windows, Linux and macOS"
#endif

#if defined(ST_PLATFORM_LINUX) || defined(ST_PLATFORM_MACOS)
	#define ST_PLATFORM_POSIX
#endif
