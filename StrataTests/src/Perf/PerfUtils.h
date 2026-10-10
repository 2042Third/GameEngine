#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// The perf lab: performance tests with checked-in budgets, run in optimized builds only (suites Perf.* in the CTest
// StrataTests.Perf, PerfGPU.* in StrataTests.PerfGPU; see AGENTS.md, "Testing"). A test measures a metric (Measure, or
// its own counters), then checks it against the metric's budget in StrataTests/Perf/Budgets.json (CheckBudget), which
// also records it in <build>/PerfResults/<config>.json.
namespace Strata::Tests::Perf
{

	// Statistics of the timed calls of a measurement, in milliseconds.
	struct Measurement
	{
		double MedianMs = 0.0;
		double P95Ms = 0.0; // Nearest rank: the smallest sample that at least 95% of the samples do not exceed
		double MaxMs = 0.0;
		double MinMs = 0.0;
		uint32_t Iterations = 0;
	};

	// A metric's entry in the budget file. Lower values are better: a test fails when it measures more than Budget.
	struct BudgetEntry
	{
		double Measured = 0.0; // The value measured on the reference machine
		double Budget = 0.0;   // At most Measured * c_BudgetHeadroom
		std::string Unit;      // One of c_BudgetUnits
	};
	using BudgetTable = std::map<std::string, BudgetEntry, std::less<>>;

	// A metric's entry in the results file.
	struct Result
	{
		double Value = 0.0;
		std::string Unit;
		double Budget = 0.0;
	};
	using ResultTable = std::map<std::string, Result, std::less<>>;

	// The most a budget may exceed the reference measurement.
	constexpr double c_BudgetHeadroom = 1.5;
	constexpr std::string_view c_BudgetUnits[] = { "ms", "us", "MB", "count" };

	// Statistics of samples in milliseconds, in any order. The median of an even number of samples is the mean of the
	// two middle ones. No samples give a measurement of zero iterations.
	Measurement Summarize(std::span<const double> samplesMs);

	// Calls function `warmup` times untimed (caches, lazy initialization, allocator pools), then `iterations` times, each
	// call timed with a steady clock, and prints the statistics under `name`. Fails the test if iterations is zero.
	Measurement Measure(std::string_view name, uint32_t warmup, uint32_t iterations, const std::function<void()>& function);

	// Parses a budget file (format in its "_comment"; keys starting with '_' are comments). nullopt with the reason in
	// error if it is malformed or breaks a rule: units from c_BudgetUnits, finite values that are not negative, and a
	// budget of at most c_BudgetHeadroom times the measured value.
	std::optional<BudgetTable> ParseBudgets(std::string_view json, std::string& error);

	// The budget of `key` in StrataTests/Perf/Budgets.json; nullopt if it has none or the file is unreadable. Never fails
	// the test: for tools, and to test the harness itself.
	std::optional<BudgetEntry> FindBudget(std::string_view key);
	// The budget of `key`; fails the test if the budget file has none (every metric needs a budget).
	double Budget(std::string_view key);

	// Records the value of metric `key` in the results file (GetResultsFile) with the unit and budget of its budget
	// entry; fails the test if it has none or the file cannot be written.
	void Record(std::string_view key, double value);
	// Records the value of metric `key`, then fails the test if it exceeds the budget. Budgets only apply to optimized
	// builds, so it fails in other builds without comparing.
	void CheckBudget(std::string_view key, double value);

	// Merges results into the results file at `path`: entries of other metrics, e.g. from the run's other test process,
	// stay; the given metrics replace their previous entries; a file that is not a JSON object is replaced. The file
	// holds one entry per metric with its value, unit and budget, keys sorted and nothing run-specific such as times,
	// so the results of two runs can be compared with a diff. False with the reason in error if it cannot be written.
	bool MergeResults(const std::filesystem::path& path, const ResultTable& results, std::string& error);

	// Release and Dist. Perf numbers of other builds mean nothing; the perf CTests exist only in these configurations.
	bool IsOptimizedBuild();

	// StrataTests/Perf/Budgets.json in the source tree.
	std::filesystem::path GetBudgetFile();
	// <build>/PerfResults/<config>.json. The CTest StrataTests.PerfResults.Clean removes it before the perf tests run,
	// so a run's file holds only that run's results.
	std::filesystem::path GetResultsFile();

}
