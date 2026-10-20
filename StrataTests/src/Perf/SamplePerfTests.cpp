#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Runtime/PerfLogReader.h"

#include <Strata/Core/FileSystem.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// The value at `fraction` (0.5 the median, 0.95 the 95th percentile) of the finite values, nearest rank.
	double Percentile(std::vector<double> values, double fraction)
	{
		std::erase_if(values, [](double value) { return !std::isfinite(value); });
		REQUIRE_FALSE(values.empty());
		std::sort(values.begin(), values.end());
		const size_t index = std::min(values.size() - 1, static_cast<size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1);
		return values[index];
	}

	std::vector<double> Column(const Tests::PerfLogTable& table, std::string_view name, size_t skip)
	{
		const std::optional<size_t> column = table.FindColumn(name);
		REQUIRE_MESSAGE(column.has_value(), "the perf log has no column ", std::string(name));
		std::vector<double> values;
		for (size_t row = skip; row < table.Rows.size(); row++)
			values.push_back(table.Rows[row][*column]);
		return values;
	}

}

// The sample projects' own measurements: their exported games run by CTests (StrataRuntime.OpenWorldTour writes the perf
// log of the Open World's tour, windowed at 1920 x 1080), which these read through STRATA_OPEN_WORLD_TOUR_LOG.
TEST_SUITE("PerfSamples")
{
	TEST_CASE("The Open World's tour: frame time and GPU time")
	{
		REQUIRE_MESSAGE(Tests::Perf::IsOptimizedBuild(), "perf metrics are measured in optimized builds (Release, Dist) only");
		const char* path = std::getenv("STRATA_OPEN_WORLD_TOUR_LOG");
		REQUIRE_MESSAGE(path != nullptr, "STRATA_OPEN_WORLD_TOUR_LOG names the tour's perf log (CTest StrataTests.PerfSamples)");
		std::string error;
		const std::optional<Tests::PerfLogTable> table = Tests::ReadPerfLog(FileSystem::FromUTF8(path), error);
		REQUIRE_MESSAGE(table.has_value(), error);
		// The first two seconds load the island behind the loading scene: the tour is what follows.
		constexpr size_t c_LoadingFrames = 120;
		REQUIRE(table->Rows.size() > c_LoadingFrames + 600);
		const std::vector<double> frames = Column(*table, "IntervalMs", c_LoadingFrames);
		const std::vector<double> gpu = Column(*table, "GPUBusyMs", c_LoadingFrames);
		const double median = Percentile(frames, 0.5);
		const double p95 = Percentile(frames, 0.95);
		const double gpuMedian = Percentile(gpu, 0.5);
		MESSAGE("Open World tour: frame median ", median, " ms, 95th percentile ", p95, " ms, GPU busy median ", gpuMedian, " ms");
		Tests::Perf::Record("PerfSamples.OpenWorldTour.FrameMedianMs", median);
		Tests::Perf::Record("PerfSamples.OpenWorldTour.FrameP95Ms", p95);
		Tests::Perf::Record("PerfSamples.OpenWorldTour.GPUBusyMedianMs", gpuMedian);
	}
}
