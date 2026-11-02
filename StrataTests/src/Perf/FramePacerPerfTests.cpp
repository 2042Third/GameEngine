#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Strata/Core/Timer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The editor's frame rate choices (Editor.FrameRate: the idle rates and the active caps).
	constexpr uint32_t c_Rates[] = { 30, 60, 90, 120, 144, 165, 240 };
	constexpr double c_SecondsPerRate = 3.0;
	constexpr uint32_t c_WarmupFrames = 10;
	// The work of a light editor frame, busy-waited so that the pacer sleeps only the rest of the period.
	constexpr std::chrono::microseconds c_FrameWork { 1000 };

	struct PacedRun
	{
		double RateError = 0.0;   // |frames / seconds / rate - 1|
		double IntervalP95 = 0.0; // The nearest-rank 95th percentile interval between frames, in periods
	};

	PacedRun Pace(uint32_t rate)
	{
		using Clock = std::chrono::steady_clock;
		FramePacer pacer(rate);
		std::vector<double> intervals;
		intervals.reserve(static_cast<size_t>(c_SecondsPerRate * rate * 2.0) + 16);
		uint32_t frame = 0;
		Clock::time_point measuredStart;
		Clock::time_point previous;
		Clock::time_point now = Clock::now();
		const Clock::time_point deadline = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(c_SecondsPerRate));
		while (now < deadline)
		{
			const Clock::time_point workEnd = Clock::now() + c_FrameWork;
			while (Clock::now() < workEnd)
			{
			}
			pacer.WaitForNextFrame();
			now = Clock::now();
			if (frame == c_WarmupFrames)
				measuredStart = now;
			else if (frame > c_WarmupFrames)
				intervals.push_back(std::chrono::duration<double>(now - previous).count());
			previous = now;
			frame++;
		}
		REQUIRE(intervals.size() > 10);
		const double seconds = std::chrono::duration<double>(previous - measuredStart).count();
		const double period = 1.0 / static_cast<double>(rate);
		PacedRun run;
		run.RateError = std::abs(static_cast<double>(intervals.size()) / seconds / static_cast<double>(rate) - 1.0);
		std::sort(intervals.begin(), intervals.end());
		const size_t rank = static_cast<size_t>(std::ceil(0.95 * static_cast<double>(intervals.size())));
		run.IntervalP95 = intervals[std::max<size_t>(rank, 1) - 1] / period;
		return run;
	}

}

TEST_SUITE("Perf.FramePacer")
{
	TEST_CASE("FramePacer holds the editor's frame rate choices")
	{
		// Information, not a gate on this machine's timer: the worst average error and frame-to-frame spread over the rates.
		// An active cap holds with an error of at most 2% and a p95 of at most 1.25 periods, an idle rate on the error alone.
		double worstError = 0.0;
		double worstP95 = 0.0;
		for (const uint32_t rate : c_Rates)
		{
			const PacedRun run = Pace(rate);
			MESSAGE("FramePacer at ", rate, " Hz: rate error ", run.RateError, ", p95 interval ", run.IntervalP95, " periods");
			worstError = std::max(worstError, run.RateError);
			worstP95 = std::max(worstP95, run.IntervalP95);
		}
		Perf::Record("Perf.FramePacer.WorstRateError", worstError);
		Perf::Record("Perf.FramePacer.WorstIntervalP95", worstP95);
	}
}
