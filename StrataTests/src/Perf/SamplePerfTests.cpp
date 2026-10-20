#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Runtime/PerfLogReader.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Process.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	constexpr const char* c_TourWorkMedian = "PerfGPU.Samples.OpenWorldTour.FrameWorkMedianMs";
	constexpr const char* c_TourWorkP95 = "PerfGPU.Samples.OpenWorldTour.FrameWorkP95Ms";
	constexpr const char* c_TourGPUMedian = "PerfGPU.Samples.OpenWorldTour.GPUBusyMedianMs";

	// The tour's frames measured: those of its first c_TourWindowMs, from the first frame with a cell of the island loaded
	// (what comes before is the loading scene's). The game runs with the display's vsync, as players see it, so the frame
	// rate follows the display: c_TourFrames covers the window on displays of up to 240 Hz.
	constexpr double c_TourWindowMs = 8'000.0;
	constexpr uint32_t c_TourFrames = 3'000;

	// The value at `fraction` (0.5 the median, 0.95 the 95th percentile) of the finite values, nearest rank.
	double Percentile(std::vector<double> values, double fraction)
	{
		std::erase_if(values, [](double value) { return !std::isfinite(value); });
		REQUIRE_FALSE(values.empty());
		std::sort(values.begin(), values.end());
		const size_t index = std::min(values.size() - 1, static_cast<size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1);
		return values[index];
	}

	size_t RequireColumn(const Tests::PerfLogTable& table, std::string_view name)
	{
		const std::optional<size_t> column = table.FindColumn(name);
		REQUIRE_MESSAGE(column.has_value(), "the perf log has no column ", std::string(name));
		return *column;
	}

}

// The sample projects' own measurements: the exported games the sample CTests leave (StrataEditor.OpenWorld exports the
// Open World), started here once per attempt, so that a result over its budget is measured again before it fails.
TEST_SUITE("PerfGPU.Samples")
{
	TEST_CASE("The Open World's tour: the frames' work and the GPU's time")
	{
		// The game's frames at 1920 x 1080 while the tour flies over the streamed island: the work of each frame (the main
		// thread's, without the waits for the display), its 95th percentile (frames that a cell's arrival lands in) and the
		// GPU's busy time. The frame interval is reported, never checked: with vsync it is the display's refresh interval.
		const std::optional<std::string> game = Platform::GetEnvVar("STRATA_OPEN_WORLD_GAME");
		REQUIRE_MESSAGE((game && !game->empty()), "STRATA_OPEN_WORLD_GAME names the exported Open World (CTest StrataTests.PerfGPU.Samples)");
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("PerfOpenWorldTour");
		Tests::Perf::CheckBudgetsConfirmed({ c_TourWorkMedian, c_TourWorkP95, c_TourGPUMedian }, [&]
		{
			const std::filesystem::path perfLog = directory / "Tour.csv";
			ProcessSpecification process;
			process.Executable = FileSystem::FromUTF8(*game);
			process.Arguments = { "--resolution", "1920x1080", "--frames", std::to_string(c_TourFrames), "--perf-log", FileSystem::ToUTF8(perfLog),
				"--save-directory", FileSystem::ToUTF8(directory / "Saves") };
			const Process::RunResult run = Process::Run(process, std::chrono::minutes(10));
			INFO("Game output:\n", run.Output);
			REQUIRE_MESSAGE(run.Started, run.Error);
			REQUIRE_FALSE(run.TimedOut);
			REQUIRE(run.ExitCode == 0);
			REQUIRE(run.Output.find("OpenWorld: tour started") != std::string::npos);

			std::string error;
			const std::optional<Tests::PerfLogTable> table = Tests::ReadPerfLog(perfLog, error);
			REQUIRE_MESSAGE(table, error);
			REQUIRE_MESSAGE(Tests::ValidatePerfLog(*table, c_TourFrames, error), error);
			const size_t cellsColumn = RequireColumn(*table, "WorldCellsLoaded");
			const size_t workColumn = RequireColumn(*table, "WorkMs");
			const size_t intervalColumn = RequireColumn(*table, "IntervalMs");
			const size_t gpuColumn = RequireColumn(*table, "GPUBusyMs");
			const auto start = std::find_if(table->Rows.begin(), table->Rows.end(), [&](const std::vector<double>& row) { return row[cellsColumn] > 0.0; });
			REQUIRE_MESSAGE(start != table->Rows.end(), "no frame of the run had a cell of the island loaded");

			std::vector<double> work;
			std::vector<double> intervals;
			std::vector<double> gpu;
			double elapsedMs = 0.0;
			for (auto row = start; row != table->Rows.end() && elapsedMs < c_TourWindowMs; ++row)
			{
				work.push_back((*row)[workColumn]);
				intervals.push_back((*row)[intervalColumn]);
				gpu.push_back((*row)[gpuColumn]);
				elapsedMs += (*row)[intervalColumn];
			}
			REQUIRE_MESSAGE(elapsedMs >= c_TourWindowMs, "the run ended ", elapsedMs / 1000.0, " s into the tour, before its ", c_TourWindowMs / 1000.0,
				" s (a display above 240 Hz needs more than c_TourFrames frames)");
			const double workMedian = Percentile(work, 0.5);
			const double workP95 = Percentile(work, 0.95);
			const double gpuMedian = Percentile(gpu, 0.5);
			MESSAGE("Open World tour, ", work.size(), " frames from frame ", start - table->Rows.begin(), ": work median ", workMedian, " ms, 95th percentile ",
				workP95, " ms; GPU busy median ", gpuMedian, " ms; frame interval median ", Percentile(intervals, 0.5), " ms (the display's)");
			return Tests::Perf::MetricValues { { c_TourWorkMedian, workMedian }, { c_TourWorkP95, workP95 }, { c_TourGPUMedian, gpuMedian } };
		});
	}
}
