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
	// (what comes before is the loading scene's). The game runs as players see it, its frame rate the display's or, where
	// the driver does not pace the window, as fast as it draws (a frame every millisecond here), so the run's length is game
	// time (StrataRuntime --seconds), never a count of frames: c_TourSeconds of game time take at least as long on the
	// clock, and cover the window once the first cell arrived within the seconds left.
	constexpr double c_TourWindowMs = 8'000.0;
	constexpr uint32_t c_TourSeconds = 12;

	// The showcase's tour (High Desert Outpost): 70 s of its 78 s, from the first frame drawn with nothing loading (the
	// loading scene and the stations' wait come before), at 2560 x 1440 (a 3840 x 2160 window does not fit the reference
	// display, and the runtime has no exclusive fullscreen: the spec's ruling Q7). 90 s of game time leave room for the wait.
	constexpr double c_ShowcaseWindowMs = 70'000.0;
	constexpr uint32_t c_ShowcaseSeconds = 90;

	struct ShowcaseMetrics
	{
		const char* GPUMedian;
		const char* GPUP95;
		const char* WorkMedian; // Null: not recorded at this preset
	};
	constexpr ShowcaseMetrics c_ShowcaseHigh = { "PerfGPU.Samples.ShowcaseHigh1440.GPUBusyMedianMs", "PerfGPU.Samples.ShowcaseHigh1440.GPUBusyP95Ms",
		"PerfGPU.Samples.ShowcaseHigh1440.FrameWorkMedianMs" };
	constexpr ShowcaseMetrics c_ShowcaseUltra = { "PerfGPU.Samples.ShowcaseUltra1440.GPUBusyMedianMs", "PerfGPU.Samples.ShowcaseUltra1440.GPUBusyP95Ms",
		nullptr };

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

	// The showcase's exported game (STRATA_SHOWCASE_GAME) through its tour at a preset, started once per attempt.
	void MeasureShowcaseTour(const char* preset, const ShowcaseMetrics& metrics)
	{
		const std::optional<std::string> game = Platform::GetEnvVar("STRATA_SHOWCASE_GAME");
		REQUIRE_MESSAGE((game && !game->empty()), "STRATA_SHOWCASE_GAME names the exported showcase (CTest StrataTests.PerfGPU.Samples)");
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory(std::string("PerfShowcase") + preset);
		std::vector<std::string> names = { metrics.GPUMedian, metrics.GPUP95 };
		if (metrics.WorkMedian)
			names.push_back(metrics.WorkMedian);
		Tests::Perf::CheckBudgetsConfirmed(names, [&]
		{
			const std::filesystem::path perfLog = directory / "Tour.csv";
			ProcessSpecification process;
			process.Executable = FileSystem::FromUTF8(*game);
			process.Arguments = { "--resolution", "2560x1440", "--seconds", std::to_string(c_ShowcaseSeconds), "--perf-log", FileSystem::ToUTF8(perfLog),
				"--save-directory", FileSystem::ToUTF8(directory / "Saves"), "--quality", preset };
			const Process::RunResult run = Process::Run(process, std::chrono::minutes(10));
			INFO("Game output:\n", run.Output);
			REQUIRE_MESSAGE(run.Started, run.Error);
			REQUIRE_FALSE(run.TimedOut);
			REQUIRE(run.ExitCode == 0);
			REQUIRE(run.Output.find("Showcase: tour started") != std::string::npos);

			std::string error;
			const std::optional<Tests::PerfLogTable> table = Tests::ReadPerfLog(perfLog, error);
			REQUIRE_MESSAGE(table, error);
			REQUIRE_MESSAGE(!table->Rows.empty(), "the perf log has no rows");
			REQUIRE_MESSAGE(Tests::ValidatePerfLog(*table, table->Rows.size(), error), error);
			const size_t loadingColumn = RequireColumn(*table, "LoadingAssets");
			const size_t workColumn = RequireColumn(*table, "WorkMs");
			const size_t intervalColumn = RequireColumn(*table, "IntervalMs");
			const size_t gpuColumn = RequireColumn(*table, "GPUBusyMs");
			const auto start = std::find_if(table->Rows.begin(), table->Rows.end(),
				[&](const std::vector<double>& row) { return row[loadingColumn] == 0.0 && row[gpuColumn] > 0.0; });
			REQUIRE_MESSAGE(start != table->Rows.end(), "no frame of the run was drawn with nothing loading");

			std::vector<double> work;
			std::vector<double> gpu;
			double elapsedMs = 0.0;
			for (auto row = start; row != table->Rows.end() && elapsedMs < c_ShowcaseWindowMs; ++row)
			{
				work.push_back((*row)[workColumn]);
				gpu.push_back((*row)[gpuColumn]);
				elapsedMs += (*row)[intervalColumn];
			}
			REQUIRE_MESSAGE(elapsedMs >= c_ShowcaseWindowMs, "the run ended ", elapsedMs / 1000.0, " s into the tour, before its ",
				c_ShowcaseWindowMs / 1000.0, " s");
			const double gpuMedian = Percentile(gpu, 0.5);
			const double gpuP95 = Percentile(gpu, 0.95);
			const double workMedian = Percentile(work, 0.5);
			MESSAGE("Showcase tour at ", std::string(preset), ", ", gpu.size(), " frames from frame ", start - table->Rows.begin(), ": GPU busy median ", gpuMedian,
				" ms, 95th percentile ", gpuP95, " ms; work median ", workMedian, " ms");
			Tests::Perf::MetricValues values { { metrics.GPUMedian, gpuMedian }, { metrics.GPUP95, gpuP95 } };
			if (metrics.WorkMedian)
				values.emplace(metrics.WorkMedian, workMedian);
			return values;
		});
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
			// At Medium, 0.6.0's costs (Docs/DeveloperGuide.md, "Perf tests"), whatever the sample's project chooses.
			process.Arguments = { "--resolution", "1920x1080", "--seconds", std::to_string(c_TourSeconds), "--perf-log", FileSystem::ToUTF8(perfLog),
				"--save-directory", FileSystem::ToUTF8(directory / "Saves"), "--quality", "medium" };
			const Process::RunResult run = Process::Run(process, std::chrono::minutes(10));
			INFO("Game output:\n", run.Output);
			REQUIRE_MESSAGE(run.Started, run.Error);
			REQUIRE_FALSE(run.TimedOut);
			REQUIRE(run.ExitCode == 0);
			REQUIRE(run.Output.find("OpenWorld: tour started") != std::string::npos);

			std::string error;
			const std::optional<Tests::PerfLogTable> table = Tests::ReadPerfLog(perfLog, error);
			REQUIRE_MESSAGE(table, error);
			REQUIRE_MESSAGE(!table->Rows.empty(), "the perf log has no rows");
			REQUIRE_MESSAGE(Tests::ValidatePerfLog(*table, table->Rows.size(), error), error);
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
				" s (the first cell arrived too late for c_TourSeconds of game time)");
			const double workMedian = Percentile(work, 0.5);
			const double workP95 = Percentile(work, 0.95);
			const double gpuMedian = Percentile(gpu, 0.5);
			MESSAGE("Open World tour, ", work.size(), " frames from frame ", start - table->Rows.begin(), ": work median ", workMedian, " ms, 95th percentile ",
				workP95, " ms; GPU busy median ", gpuMedian, " ms; frame interval median ", Percentile(intervals, 0.5), " ms (the display's)");
			return Tests::Perf::MetricValues { { c_TourWorkMedian, workMedian }, { c_TourWorkP95, workP95 }, { c_TourGPUMedian, gpuMedian } };
		});
	}

	TEST_CASE("The showcase's tour at High: the GPU's time and the frames' work")
	{
		// High Desert Outpost's six shots at the default preset: the reference scene for High's budget (wave 8's target
		// 8.3 ms at 3840 x 2160, recorded here at 2560 x 1440 until a display can show 4K windowed).
		MeasureShowcaseTour("high", c_ShowcaseHigh);
	}

	TEST_CASE("The showcase's tour at Ultra: the GPU's time")
	{
		MeasureShowcaseTour("ultra", c_ShowcaseUltra);
	}
}
