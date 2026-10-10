#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Strata/Core/FrameProfiler.h"

#include <array>
#include <cstdint>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Frames are timed in batches: one frame of the profiler takes about a microsecond.
	constexpr uint32_t c_FramesPerBatch = 1000;
	// The overhead the frame profiler may add to a frame with 16 stages (the streaming observatory's acceptance limit).
	constexpr double c_MaxFrameOverheadUs = 20.0;

}

TEST_SUITE("Perf.FrameProfiler")
{
	TEST_CASE("Profiling a frame of 16 stages costs microseconds")
	{
		// As in an application: the job system runs, and every frame samples its counters.
		Perf::ScopedApplicationJobSystem jobs;
		FrameProfiler profiler;
		std::array<FrameStageID, 16> stages = {};
		for (size_t index = 0; index < stages.size(); index++)
		{
			stages[index] = index < c_BuiltinFrameStages.size() ? static_cast<FrameStageID>(index)
				: profiler.RegisterStage("Perf.Stage" + std::to_string(index));
			REQUIRE(stages[index] != c_InvalidFrameStage);
		}

		// A frame shaped like the editor's: most stages one after another, a few nested (finalization in the asset update,
		// extraction in the render, transforms in the scene update).
		const Perf::Measurement frames = Perf::Measure("Perf.FrameProfiler.Frame16Stages (1000 frames)", 3, 30, [&]
		{
			for (uint32_t frame = 0; frame < c_FramesPerBatch; frame++)
			{
				profiler.BeginFrame();
				for (size_t index = 0; index < stages.size(); index += 2)
				{
					const FrameStageScope outer(profiler, stages[index]);
					const FrameStageScope inner(profiler, stages[index + 1]);
				}
				profiler.EndFrame(0.001);
			}
		});
		REQUIRE(profiler.GetTotalFrameCount() >= c_FramesPerBatch);
		const double overheadUs = frames.MedianMs * 1000.0 / c_FramesPerBatch;
		MESSAGE("Frame profiler overhead: ", overheadUs, " us per frame of 16 stages");
		CHECK(overheadUs <= c_MaxFrameOverheadUs);
		Perf::CheckBudget("Perf.FrameProfiler.Frame16Stages", overheadUs);
	}
}
