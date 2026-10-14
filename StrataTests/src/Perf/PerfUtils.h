#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The perf lab: performance tests with checked-in budgets, run in optimized builds only (suites Perf.* in the CTest
// StrataTests.Perf, PerfGPU.* in StrataTests.PerfGPU; see Docs/DeveloperGuide.md, "Perf tests"). A test measures
// metrics (Measure, or its own counters) in a measurement function and checks them against their budgets in
// StrataTests/Perf/Budgets.json with CheckBudgetConfirmed or CheckBudgetsConfirmed: a metric over its budget is
// measured again, and fails only when every attempt is over budget, so that the machine's other work does not fail a
// test while a regression does. Every check is recorded in <build>/PerfResults/<config>.json, attempts included.
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
		// A roadmap target below the budget, for information: the budget is the gate. nullopt if the metric has none.
		std::optional<double> Target;
	};
	using BudgetTable = std::map<std::string, BudgetEntry, std::less<>>;

	// A metric's entry in the results file.
	struct Result
	{
		double Value = 0.0;
		std::string Unit;
		double Budget = 0.0;
		std::optional<double> Target;
		// The value of every attempt of a confirmed check, in order; empty for a metric that was checked or recorded once.
		std::vector<double> Attempts;
	};
	using ResultTable = std::map<std::string, Result, std::less<>>;

	// The most a budget may exceed the reference measurement.
	constexpr double c_BudgetHeadroom = 1.5;
	constexpr std::string_view c_BudgetUnits[] = { "ms", "us", "MB", "count", "ratio" };
	// How often a confirmed check measures a metric at most (the first measurement included): it fails only when the
	// metric is over its budget in every attempt.
	constexpr uint32_t c_ConfirmationAttempts = 3;

	// The values of the metrics that one call of a confirmed check's measurement function measured, by metric.
	using MetricValues = std::map<std::string, double, std::less<>>;

	// What a confirmed check found for one metric.
	struct ConfirmedMetric
	{
		std::string Key;
		BudgetEntry Entry;
		std::vector<double> Attempts; // The metric's value in each attempt, up to the first one within its budget
		bool Passed = false;          // An attempt was within the budget (the last one in Attempts)
		double Value = 0.0;           // The passing attempt's value, else the smallest attempt (the closest to the budget)
	};

	// Statistics of samples in milliseconds, in any order. The median of an even number of samples is the mean of the
	// two middle ones. No samples give a measurement of zero iterations.
	Measurement Summarize(std::span<const double> samplesMs);

	// Calls function `warmup` times untimed (caches, lazy initialization, allocator pools), then `iterations` times, each
	// call timed with a steady clock, and prints the statistics under `name`. Fails the test if iterations is zero.
	Measurement Measure(std::string_view name, uint32_t warmup, uint32_t iterations, const std::function<void()>& function);

	// Parses a budget file (format in its "_comment"; keys starting with '_' are comments). nullopt with the reason in
	// error if it is malformed or breaks a rule: units from c_BudgetUnits, finite values that are not negative, a budget
	// of at most c_BudgetHeadroom times the measured value, and a target below the budget (the budget enforces any other).
	std::optional<BudgetTable> ParseBudgets(std::string_view json, std::string& error);

	// Checks metrics against a budget table and records them in a results file. The perf tests share the process's
	// checker (GetBudgetChecker: the budget file and GetResultsFile) through the functions below; tests of the lab itself
	// make their own.
	class BudgetChecker
	{
	public:
		// `budgetSource` names where the budgets come from in failure messages (the budget file).
		BudgetChecker(BudgetTable budgets, std::string budgetSource, std::filesystem::path resultsFile);

		BudgetChecker(const BudgetChecker&) = delete;
		BudgetChecker& operator=(const BudgetChecker&) = delete;

		// The budget entry of `key`; nullptr with the reason in error if it has none.
		const BudgetEntry* Find(std::string_view key, std::string& error) const;

		// Records the value of metric `key` in the results file with the unit, budget and target of its entry; fails the
		// test if it has none or the file cannot be written.
		void Record(std::string_view key, double value);

		// The confirmed check of metrics that one measurement produces together: calls `measure`, which measures every
		// metric of `keys` afresh (its own warm-up, its counters relative to its own start), until each metric has been
		// within its budget in one call, at most `attempts` times. A metric fails the test only when it is over its
		// budget in every attempt. Prints every attempt, records each metric with all its attempts and returns what it
		// found per metric, in the order of `keys`. Before measuring it fails the test (REQUIRE) if attempts is zero, or
		// keys is empty, holds a key twice or a key without a budget; after a measurement that does not report exactly
		// the metrics of keys, or a value that is not finite.
		std::vector<ConfirmedMetric> CheckConfirmed(const std::vector<std::string>& keys, const std::function<MetricValues()>& measure,
			uint32_t attempts = c_ConfirmationAttempts);

		const std::filesystem::path& GetResultsFile() const { return m_ResultsFile; }
	private:
		// Adds the result to the records of this checker and writes them all (MergeResults); fails the test if it cannot.
		void Store(std::string_view key, Result result);
	private:
		const BudgetTable m_Budgets;
		const std::string m_BudgetSource;
		const std::filesystem::path m_ResultsFile;
		std::mutex m_ResultsMutex;
		ResultTable m_Results; // Written whole on every record, so the file never misses one
	};

	// The budget of `key` in StrataTests/Perf/Budgets.json; nullopt if it has none or the file is unreadable. Never fails
	// the test: for tools, and to test the harness itself.
	std::optional<BudgetEntry> FindBudget(std::string_view key);
	// The budget of `key`; fails the test if the budget file has none (every metric needs a budget).
	double Budget(std::string_view key);

	// The checker of the perf tests: the budgets of StrataTests/Perf/Budgets.json, results in GetResultsFile(). Fails the
	// test if the budget file is unreadable or malformed.
	BudgetChecker& GetBudgetChecker();

	// Records the value of metric `key` without checking it (BudgetChecker::Record of the perf tests' checker).
	void Record(std::string_view key, double value);


	// The confirmed check of one metric (BudgetChecker::CheckConfirmed of the perf tests' checker): `measure` measures it
	// afresh and returns its value. Fails in builds that are not optimized without measuring.
	ConfirmedMetric CheckBudgetConfirmed(std::string_view key, const std::function<double()>& measure, uint32_t attempts = c_ConfirmationAttempts);
	// The confirmed check of metrics that one measurement produces together (BudgetChecker::CheckConfirmed of the perf
	// tests' checker). Fails in builds that are not optimized without measuring.
	std::vector<ConfirmedMetric> CheckBudgetsConfirmed(const std::vector<std::string>& keys, const std::function<MetricValues()>& measure,
		uint32_t attempts = c_ConfirmationAttempts);

	// Merges results into the results file at `path`: entries of other metrics, e.g. from the run's other test process,
	// stay; the given metrics replace their previous entries; a file that is not a JSON object is replaced. The file
	// holds one entry per metric with its value, unit and budget, its target if it has one and the values of its
	// attempts if it was confirmed, keys sorted and nothing else run-specific such as times, so the results of two runs
	// can be compared with a diff. False with the reason in error if it cannot be written.
	bool MergeResults(const std::filesystem::path& path, const ResultTable& results, std::string& error);

	// Release and Dist. Perf numbers of other builds mean nothing; the perf CTests exist only in these configurations.
	bool IsOptimizedBuild();

	// StrataTests/Perf/Budgets.json in the source tree.
	std::filesystem::path GetBudgetFile();
	// <build>/PerfResults/<config>.json. The CTest StrataTests.PerfResults.Clean removes it before the perf tests run,
	// so a run's file holds only that run's results.
	std::filesystem::path GetResultsFile();

}
