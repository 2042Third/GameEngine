#pragma once

#include "Strata/Core/FrameProfiler.h"

// CPU profiling instrumentation.
//
// ST_PROFILE_FRAME/FUNCTION/SCOPE/THREAD compile to nothing unless the engine is configured with
// -DSTRATA_ENABLE_TRACY=ON, in which case zones are reported to the Tracy profiler.
//
// ST_PROFILE_STAGE(name) is always on: it times a stage of the main thread's frame in the FrameProfiler for the rest of
// the scope (the stage is registered under `name`, a constant string such as a literal, the first time the line runs)
// and, with Tracy, is also a zone of that name. Stages registered at run time (e.g. a scene system's) are timed with a
// FrameStageScope; ST_PROFILE_STAGE_ID_ZONE(stage) adds their Tracy zone.

#define ST_PROFILE_CONCAT_IMPL(a, b) a##b
#define ST_PROFILE_CONCAT(a, b) ST_PROFILE_CONCAT_IMPL(a, b)

#if defined(ST_ENABLE_TRACY)
	#include <tracy/Tracy.hpp>

	#include <cstring>

	#define ST_PROFILE_FRAME()        FrameMark
	#define ST_PROFILE_FUNCTION()     ZoneScoped
	#define ST_PROFILE_SCOPE(name)    ZoneScopedN(name)
	#define ST_PROFILE_THREAD(name)   tracy::SetThreadName(name)
	#define ST_PROFILE_STAGE_ZONE(name) ZoneNamedN(ST_PROFILE_CONCAT(stageZone, __LINE__), name, true)
	#define ST_PROFILE_STAGE_ID_ZONE(stage) \
		ZoneTransientN(ST_PROFILE_CONCAT(stageZone, __LINE__), ::Strata::FrameProfiler::Get().GetStageName(stage).c_str(), true)
#else
	#define ST_PROFILE_FRAME()
	#define ST_PROFILE_FUNCTION()
	#define ST_PROFILE_SCOPE(name)
	#define ST_PROFILE_THREAD(name)
	#define ST_PROFILE_STAGE_ZONE(name)
	#define ST_PROFILE_STAGE_ID_ZONE(stage)
#endif

#define ST_PROFILE_STAGE(name) \
	static const ::Strata::FrameStageID ST_PROFILE_CONCAT(s_FrameStage, __LINE__) = ::Strata::FrameProfiler::Get().RegisterStage(name); \
	const ::Strata::FrameStageScope ST_PROFILE_CONCAT(frameStageScope, __LINE__)(::Strata::FrameProfiler::Get(), ST_PROFILE_CONCAT(s_FrameStage, __LINE__)); \
	ST_PROFILE_STAGE_ZONE(name)
