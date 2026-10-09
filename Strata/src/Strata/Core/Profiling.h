#pragma once

// CPU profiling instrumentation. Compiles to nothing unless the engine is configured with
// -DSTRATA_ENABLE_TRACY=ON, in which case zones are reported to the Tracy profiler.

#if defined(ST_ENABLE_TRACY)
	#include <tracy/Tracy.hpp>

	#define ST_PROFILE_FRAME()        FrameMark
	#define ST_PROFILE_FUNCTION()     ZoneScoped
	#define ST_PROFILE_SCOPE(name)    ZoneScopedN(name)
	#define ST_PROFILE_THREAD(name)   tracy::SetThreadName(name)
#else
	#define ST_PROFILE_FRAME()
	#define ST_PROFILE_FUNCTION()
	#define ST_PROFILE_SCOPE(name)
	#define ST_PROFILE_THREAD(name)
#endif
