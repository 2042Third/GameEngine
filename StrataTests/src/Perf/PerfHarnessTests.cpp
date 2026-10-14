#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Strata/Core/FileSystem.h"
#include "TestHelpers.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The perf lab's own tests: PerfLab (the harness's logic, in StrataTests.Core in every configuration) and Perf.Harness
// (timed, in StrataTests.Perf in optimized builds).

using namespace Strata;

namespace
{

	// The harness's own metric: a fixed single-threaded CPU workload. When it exceeds its budget, the machine itself is
	// slow (throttled, or busier than it has cores), which explains other perf failures of the same run. The converse does
	// not hold: load on many cores of a large machine leaves one core free for it while it slows parallel work, so the
	// canary is usually unchanged in runs that load failed.
	constexpr std::string_view c_CanaryMetric = "Perf.Harness.Sort200k";
	constexpr std::string_view c_MissingMetric = "Perf.Harness.MetricWithoutBudget";

	// The entry of metric in a results file; nullopt if the file or the entry is missing.
	std::optional<nlohmann::json> ReadResultEntry(const std::filesystem::path& file, std::string_view metric)
	{
		const std::optional<std::string> text = FileSystem::ReadText(file);
		if (!text)
			return std::nullopt;
		const nlohmann::json document = nlohmann::json::parse(*text, nullptr, false);
		if (document.is_discarded() || !document.is_object())
			return std::nullopt;
		const auto entry = document.find(std::string(metric));
		if (entry == document.end())
			return std::nullopt;
		return *entry;
	}

	// Whether entry holds exactly a value, unit and budget with these values, and the attempts if there are any.
	bool IsResultEntry(const nlohmann::json& entry, double value, std::string_view unit, double budget, const std::vector<double>& attempts = {})
	{
		if (!entry.is_object() || entry.size() != (attempts.empty() ? 3u : 4u))
			return false;
		const auto valueMember = entry.find("Value");
		const auto unitMember = entry.find("Unit");
		const auto budgetMember = entry.find("Budget");
		if (valueMember == entry.end() || !valueMember->is_number() || valueMember->get<double>() != value || unitMember == entry.end()
			|| !unitMember->is_string() || unitMember->get<std::string>() != unit || budgetMember == entry.end() || !budgetMember->is_number()
			|| budgetMember->get<double>() != budget)
			return false;
		if (attempts.empty())
			return true;
		const auto attemptsMember = entry.find("Attempts");
		return attemptsMember != entry.end() && attemptsMember->is_array() && attemptsMember->get<std::vector<double>>() == attempts;
	}

	// A checker of metric "A" (budget 10 ms) and "B" (budget 2 MB, target 1 MB) that records into a fresh results file.
	struct TestChecker
	{
		std::filesystem::path ResultsFile = Tests::CreateTemporaryDirectory("PerfLab") / "PerfResults" / "Release.json";
		Tests::Perf::BudgetChecker Checker { Tests::Perf::BudgetTable {
			{ "A", { 8.0, 10.0, "ms", std::nullopt } },
			{ "B", { 1.5, 2.0, "MB", 1.0 } } }, "TestBudgets.json", ResultsFile };
	};

	// A measurement of metric `key` that returns `values` in turn, counting its calls.
	struct ScriptedMeasurement
	{
		std::string Key;
		std::vector<double> Values;
		uint32_t Calls = 0;

		Tests::Perf::MetricValues operator()()
		{
			REQUIRE(Calls < Values.size());
			return { { Key, Values[Calls++] } };
		}
	};

}

TEST_SUITE("PerfLab")
{
	TEST_CASE("Summarize computes the median, the 95th percentile and the extremes")
	{
		const std::vector<double> odd = { 5.0, 1.0, 4.0, 2.0, 3.0 };
		Tests::Perf::Measurement measurement = Tests::Perf::Summarize(odd);
		CHECK(measurement.Iterations == 5);
		CHECK(measurement.MedianMs == 3.0);
		CHECK(measurement.P95Ms == 5.0);
		CHECK(measurement.MinMs == 1.0);
		CHECK(measurement.MaxMs == 5.0);

		// Even counts take the mean of the two middle samples; the percentile is the nearest rank, ceil(0.95 * n).
		std::vector<double> hundred;
		for (int sample = 100; sample >= 1; sample--)
			hundred.push_back(static_cast<double>(sample));
		measurement = Tests::Perf::Summarize(hundred);
		CHECK(measurement.Iterations == 100);
		CHECK(measurement.MedianMs == 50.5);
		CHECK(measurement.P95Ms == 95.0);
		CHECK(measurement.MinMs == 1.0);
		CHECK(measurement.MaxMs == 100.0);

		std::vector<double> twenty;
		for (int sample = 1; sample <= 20; sample++)
			twenty.push_back(static_cast<double>(sample));
		measurement = Tests::Perf::Summarize(twenty);
		CHECK(measurement.MedianMs == 10.5);
		CHECK(measurement.P95Ms == 19.0);

		const std::vector<double> single = { 7.25 };
		measurement = Tests::Perf::Summarize(single);
		CHECK(measurement.Iterations == 1);
		CHECK(measurement.MedianMs == 7.25);
		CHECK(measurement.P95Ms == 7.25);
		CHECK(measurement.MinMs == 7.25);
		CHECK(measurement.MaxMs == 7.25);

		measurement = Tests::Perf::Summarize({});
		CHECK(measurement.Iterations == 0);
		CHECK(measurement.MedianMs == 0.0);
		CHECK(measurement.MaxMs == 0.0);
	}

	TEST_CASE("The budget file follows the budget rules and Budget reads it")
	{
		const std::optional<std::string> text = FileSystem::ReadText(Tests::Perf::GetBudgetFile());
		REQUIRE(text.has_value());
		std::string error;
		const std::optional<Tests::Perf::BudgetTable> table = Tests::Perf::ParseBudgets(*text, error);
		REQUIRE_MESSAGE(table.has_value(), error);

		const auto canary = table->find(c_CanaryMetric);
		REQUIRE(canary != table->end());
		CHECK(canary->second.Unit == "ms");
		CHECK(canary->second.Budget > 0.0);

		const std::optional<Tests::Perf::BudgetEntry> found = Tests::Perf::FindBudget(c_CanaryMetric);
		REQUIRE(found.has_value());
		CHECK(found->Measured == canary->second.Measured);
		CHECK(found->Budget == canary->second.Budget);
		CHECK(Tests::Perf::Budget(c_CanaryMetric) == canary->second.Budget);
	}

	TEST_CASE("Budget files that break the rules are rejected with the reason")
	{
		struct InvalidFile
		{
			const char* Json;
			const char* Reason;
		};
		const InvalidFile invalidFiles[] = {
			{ "not json", "not a JSON object" },
			{ "[]", "not a JSON object" },
			{ R"({ "A": 1 })", "'A' is not an object" },
			{ R"({ "": { "Measured": 1, "Budget": 1, "Unit": "ms" } })", "empty name" },
			{ R"({ "A": { "Measured": "1", "Budget": 1, "Unit": "ms" } })", "needs a number 'Measured'" },
			{ R"({ "A": { "Measured": 1, "Unit": "ms" } })", "needs a number 'Budget'" },
			{ R"({ "A": { "Measured": -1, "Budget": 0, "Unit": "ms" } })", "negative" },
			{ R"({ "A": { "Measured": 1, "Budget": 1.5 } })", "needs a 'Unit' of ms, us, MB, count" },
			{ R"({ "A": { "Measured": 1, "Budget": 1.5, "Unit": "seconds" } })", "needs a 'Unit'" },
			{ R"({ "A": { "Measured": 1, "Budget": 1.6, "Unit": "ms" } })", "exceeds 1.5 times its measured value" },
			{ R"({ "A": { "Measured": 1, "Budget": 1, "Unit": "ms", "Date": "today" } })", "unknown member 'Date'" },
			{ R"({ "A": { "Measured": 1, "Budget": 1.5, "Unit": "ms", "Target": "1" } })", "needs a number 'Target'" },
			{ R"({ "A": { "Measured": 1, "Budget": 1.5, "Unit": "ms", "Target": -1 } })", "negative" },
			// A target the budget meets is enforced by it; the change that lowers the budget that far removes the target.
			{ R"({ "A": { "Measured": 1, "Budget": 1.5, "Unit": "ms", "Target": 1.5 } })", "is not below its budget" },
			{ R"({ "A": { "Measured": 1, "Budget": 1.5, "Unit": "ms", "Target": 2 } })", "is not below its budget" },
			// Provisional entries have no values the reference machine has not measured, and say so only with true.
			{ R"({ "A": { "Measured": 1, "Budget": 1.5, "Unit": "ms", "Provisional": true } })", "has a 'Measured' or 'Budget'" },
			{ R"({ "A": { "Budget": 1.5, "Unit": "ms", "Provisional": true } })", "has a 'Measured' or 'Budget'" },
			{ R"({ "A": { "Unit": "ms", "Provisional": false } })", "'Provisional' that is not true" },
			{ R"({ "A": { "Unit": "ms", "Provisional": "yes" } })", "'Provisional' that is not true" },
			{ R"({ "A": { "Provisional": true } })", "needs a 'Unit'" },
			{ R"({ "A": { "Unit": "ms", "Provisional": true, "Target": -1 } })", "negative" },
		};
		for (const InvalidFile& invalidFile : invalidFiles)
		{
			CAPTURE(invalidFile.Json);
			std::string error;
			CHECK_FALSE(Tests::Perf::ParseBudgets(invalidFile.Json, error).has_value());
			CHECK(error.find(invalidFile.Reason) != std::string::npos);
		}

		// Comments are skipped, a budget may be exactly 1.5 times the measurement, a zero count is a budget too, a target
		// lies below the budget (even below the measurement, when the reference machine meets it), a ratio of two
		// measurements is a unit, and a provisional metric names its unit and maybe a target.
		const char* validFile = R"({
			"_comment": ["About this file"],
			"A": { "Measured": 3.21, "Budget": 4.815, "Unit": "ms" },
			"B": { "Measured": 0, "Budget": 0, "Unit": "count" },
			"C": { "Measured": 720, "Budget": 974.1, "Unit": "MB", "Target": 750 },
			"D": { "Measured": 1.02, "Budget": 1.1, "Unit": "ratio" },
			"E": { "Unit": "ms", "Provisional": true, "Target": 5 }
		})";
		std::string error;
		const std::optional<Tests::Perf::BudgetTable> table = Tests::Perf::ParseBudgets(validFile, error);
		REQUIRE_MESSAGE(table.has_value(), error);
		REQUIRE(table->size() == 5);
		CHECK_FALSE(table->at("A").Provisional);
		CHECK(table->at("E").Provisional);
		CHECK(table->at("E").Unit == "ms");
		CHECK(table->at("E").Target == 5.0);
		CHECK(table->at("D").Unit == "ratio");
		CHECK(table->at("A").Measured == 3.21);
		CHECK(table->at("A").Budget == 4.815);
		CHECK(table->at("A").Unit == "ms");
		CHECK_FALSE(table->at("A").Target.has_value());
		CHECK(table->at("B").Budget == 0.0);
		CHECK(table->at("B").Unit == "count");
		CHECK(table->at("C").Target == 750.0);
	}

	TEST_CASE("A metric without a budget is reported missing without failing")
	{
		CHECK_FALSE(Tests::Perf::FindBudget(c_MissingMetric).has_value());
		CHECK(Tests::Perf::FindBudget(c_CanaryMetric).has_value());

		// The reason names the metric and where its budget belongs.
		const TestChecker test;
		std::string error;
		CHECK(test.Checker.Find("A", error) != nullptr);
		CHECK(test.Checker.Find("Missing", error) == nullptr);
		CHECK(error.find("metric 'Missing' has no budget in 'TestBudgets.json'") != std::string::npos);
	}

	// Exactly one failed assertion: the budget file is fine (the CHECK passes), and asking for a missing metric fails.
	TEST_CASE("Asking for the budget of a metric without one fails the test" * doctest::expected_failures(1))
	{
		CHECK(Tests::Perf::FindBudget(c_CanaryMetric).has_value());
		Tests::Perf::Budget(c_MissingMetric);
	}

	TEST_CASE("Recording a metric without a budget fails the test" * doctest::expected_failures(1))
	{
		CHECK(Tests::Perf::FindBudget(c_CanaryMetric).has_value());
		Tests::Perf::Record(c_MissingMetric, 1.0);
	}

	TEST_CASE("Results files hold one entry per metric with its value, unit and budget")
	{
		const std::filesystem::path file = Tests::CreateTemporaryDirectory("PerfResults") / "PerfResults" / "Release.json";
		Tests::Perf::ResultTable results;
		results["B.Metric"] = { 2.5, "ms", 3.0 };
		results["A.Metric"] = { 12.0, "MB", 15.0 };
		std::string error;
		REQUIRE_MESSAGE(Tests::Perf::MergeResults(file, results, error), error);

		const std::optional<std::string> first = FileSystem::ReadText(file);
		REQUIRE(first.has_value());
		const nlohmann::json document = nlohmann::json::parse(*first, nullptr, false);
		REQUIRE(document.is_object());
		CHECK(document.size() == 2);
		CHECK(IsResultEntry(document.value("A.Metric", nlohmann::json()), 12.0, "MB", 15.0));
		CHECK(IsResultEntry(document.value("B.Metric", nlohmann::json()), 2.5, "ms", 3.0));
		CHECK(first->find("\"A.Metric\"") < first->find("\"B.Metric\""));

		// The same results give the same file: nothing run-specific, nothing appended.
		REQUIRE_MESSAGE(Tests::Perf::MergeResults(file, results, error), error);
		CHECK(FileSystem::ReadText(file) == first);

		// Metrics of another test process of the run stay; a new value replaces a metric's old one.
		Tests::Perf::ResultTable otherProcess;
		otherProcess["C.Metric"] = { 1.0, "count", 2.0 };
		REQUIRE_MESSAGE(Tests::Perf::MergeResults(file, otherProcess, error), error);
		Tests::Perf::ResultTable update;
		update["B.Metric"] = { 2.0, "ms", 3.0 };
		REQUIRE_MESSAGE(Tests::Perf::MergeResults(file, update, error), error);
		const std::optional<std::string> merged = FileSystem::ReadText(file);
		REQUIRE(merged.has_value());
		const nlohmann::json mergedDocument = nlohmann::json::parse(*merged, nullptr, false);
		REQUIRE(mergedDocument.is_object());
		CHECK(mergedDocument.size() == 3);
		CHECK(IsResultEntry(mergedDocument.value("A.Metric", nlohmann::json()), 12.0, "MB", 15.0));
		CHECK(IsResultEntry(mergedDocument.value("B.Metric", nlohmann::json()), 2.0, "ms", 3.0));
		CHECK(IsResultEntry(mergedDocument.value("C.Metric", nlohmann::json()), 1.0, "count", 2.0));

		// Targets and the attempts of confirmed checks are part of their entries.
		Tests::Perf::ResultTable confirmed;
		confirmed["D.Metric"] = { 9.0, "ms", 10.0, 5.0, { 11.0, 9.0 } };
		REQUIRE_MESSAGE(Tests::Perf::MergeResults(file, confirmed, error), error);
		const std::optional<nlohmann::json> confirmedEntry = ReadResultEntry(file, "D.Metric");
		REQUIRE(confirmedEntry.has_value());
		CHECK(confirmedEntry->size() == 5);
		CHECK(confirmedEntry->value("Target", 0.0) == 5.0);
		CHECK(confirmedEntry->value("Attempts", std::vector<double>()) == std::vector<double> { 11.0, 9.0 });

		// A file that holds no results object is replaced.
		REQUIRE(FileSystem::WriteText(file, "not a results file"));
		REQUIRE_MESSAGE(Tests::Perf::MergeResults(file, update, error), error);
		const std::optional<nlohmann::json> replaced = ReadResultEntry(file, "B.Metric");
		REQUIRE(replaced.has_value());
		CHECK(IsResultEntry(*replaced, 2.0, "ms", 3.0));
		CHECK_FALSE(ReadResultEntry(file, "A.Metric").has_value());
	}

	TEST_CASE("A confirmed check within its budget measures once")
	{
		TestChecker test;
		ScriptedMeasurement measurement { "A", { 7.5 } };
		const std::vector<Tests::Perf::ConfirmedMetric> metrics = test.Checker.CheckConfirmed({ "A" }, std::ref(measurement));
		CHECK(measurement.Calls == 1);
		REQUIRE(metrics.size() == 1);
		CHECK(metrics[0].Key == "A");
		CHECK(metrics[0].Passed);
		CHECK(metrics[0].Value == 7.5);
		CHECK(metrics[0].Attempts == std::vector<double> { 7.5 });
		CHECK(metrics[0].Entry.Budget == 10.0);

		const std::optional<nlohmann::json> recorded = ReadResultEntry(test.ResultsFile, "A");
		REQUIRE(recorded.has_value());
		CHECK(IsResultEntry(*recorded, 7.5, "ms", 10.0, { 7.5 }));

		// A value exactly at the budget is within it.
		ScriptedMeasurement atBudget { "A", { 10.0 } };
		CHECK(test.Checker.CheckConfirmed({ "A" }, std::ref(atBudget))[0].Passed);
		CHECK(atBudget.Calls == 1);
	}

	TEST_CASE("A provisional metric is measured once and recorded without a budget, whatever it measures")
	{
		const std::filesystem::path resultsFile = Tests::CreateTemporaryDirectory("PerfLab") / "PerfResults" / "Release.json";
		Tests::Perf::BudgetEntry provisional;
		provisional.Unit = "ms";
		provisional.Provisional = true;
		Tests::Perf::BudgetChecker checker(Tests::Perf::BudgetTable { { "A", { 8.0, 10.0, "ms", std::nullopt } }, { "P", provisional } },
			"TestBudgets.json", resultsFile);

		ScriptedMeasurement measurement { "P", { 1000.0 } };
		const std::vector<Tests::Perf::ConfirmedMetric> metrics = checker.CheckConfirmed({ "P" }, std::ref(measurement));
		CHECK(measurement.Calls == 1);
		REQUIRE(metrics.size() == 1);
		CHECK(metrics[0].Passed);
		CHECK(metrics[0].Value == 1000.0);
		const std::optional<nlohmann::json> recorded = ReadResultEntry(resultsFile, "P");
		REQUIRE(recorded.has_value());
		CHECK(*recorded == nlohmann::json { { "Value", 1000.0 }, { "Unit", "ms" }, { "Provisional", true }, { "Attempts", { 1000.0 } } });

		// Measured with a metric that has a budget, it is decided by the first attempt: only the other one is confirmed.
		int calls = 0;
		const std::vector<Tests::Perf::ConfirmedMetric> together = checker.CheckConfirmed({ "A", "P" }, [&calls]()
		{
			calls++;
			return calls == 1 ? Tests::Perf::MetricValues { { "A", 12.0 }, { "P", 50.0 } } : Tests::Perf::MetricValues { { "A", 9.0 }, { "P", 60.0 } };
		});
		CHECK(calls == 2);
		REQUIRE(together.size() == 2);
		CHECK(together[0].Attempts == std::vector<double> { 12.0, 9.0 });
		CHECK(together[1].Attempts == std::vector<double> { 50.0 });
		CHECK(together[1].Value == 50.0);

		// A value without a check is recorded the same way.
		checker.Record("P", 3.0);
		const std::optional<nlohmann::json> recordedOnce = ReadResultEntry(resultsFile, "P");
		REQUIRE(recordedOnce.has_value());
		CHECK(*recordedOnce == nlohmann::json { { "Value", 3.0 }, { "Unit", "ms" }, { "Provisional", true } });
	}

	TEST_CASE("Asking for the budget of a provisional metric fails the test" * doctest::expected_failures(1))
	{
		Tests::Perf::BudgetEntry provisional;
		provisional.Unit = "ms";
		provisional.Provisional = true;
		const Tests::Perf::BudgetChecker checker(Tests::Perf::BudgetTable { { "A", { 8.0, 10.0, "ms", std::nullopt } }, { "P", provisional } },
			"TestBudgets.json", Tests::CreateTemporaryDirectory("PerfLab") / "Results.json");
		CHECK(checker.GetBudget("A") == 10.0);
		checker.GetBudget("P");
	}

	TEST_CASE("A confirmed check measures a metric over its budget again until an attempt is within it")
	{
		TestChecker test;
		ScriptedMeasurement secondAttempt { "A", { 12.0, 9.0 } };
		std::vector<Tests::Perf::ConfirmedMetric> metrics = test.Checker.CheckConfirmed({ "A" }, std::ref(secondAttempt));
		CHECK(secondAttempt.Calls == 2);
		REQUIRE(metrics.size() == 1);
		CHECK(metrics[0].Passed);
		CHECK(metrics[0].Value == 9.0);
		CHECK(metrics[0].Attempts == std::vector<double> { 12.0, 9.0 });

		// The last attempt decides as well, and the results file lists every attempt.
		ScriptedMeasurement lastAttempt { "A", { 12.0, 10.5, 6.0 } };
		metrics = test.Checker.CheckConfirmed({ "A" }, std::ref(lastAttempt));
		CHECK(lastAttempt.Calls == Tests::Perf::c_ConfirmationAttempts);
		CHECK(metrics[0].Passed);
		CHECK(metrics[0].Value == 6.0);
		const std::optional<nlohmann::json> recorded = ReadResultEntry(test.ResultsFile, "A");
		REQUIRE(recorded.has_value());
		CHECK(IsResultEntry(*recorded, 6.0, "ms", 10.0, { 12.0, 10.5, 6.0 }));
	}

	// Exactly one failed assertion: the check of metric A, after its last attempt.
	TEST_CASE("A confirmed check fails when every attempt is over the budget" * doctest::expected_failures(1))
	{
		TestChecker test;
		ScriptedMeasurement measurement { "A", { 12.0, 13.0, 11.0 } };
		const std::vector<Tests::Perf::ConfirmedMetric> metrics = test.Checker.CheckConfirmed({ "A" }, std::ref(measurement));
		CHECK(measurement.Calls == Tests::Perf::c_ConfirmationAttempts);
		REQUIRE(metrics.size() == 1);
		CHECK_FALSE(metrics[0].Passed);
		// The value of a failed metric is its best attempt: the regression is at least that large.
		CHECK(metrics[0].Value == 11.0);
		CHECK(metrics[0].Attempts == std::vector<double> { 12.0, 13.0, 11.0 });
		const std::optional<nlohmann::json> recorded = ReadResultEntry(test.ResultsFile, "A");
		REQUIRE(recorded.has_value());
		CHECK(IsResultEntry(*recorded, 11.0, "ms", 10.0, { 12.0, 13.0, 11.0 }));
	}

	TEST_CASE("Metrics measured together are confirmed one by one" * doctest::expected_failures(1))
	{
		TestChecker test;
		// A is within its budget at once, B in the second attempt; A's later values do not count. The second check's B is
		// over its budget in every attempt (the one failure), while A passed in the first.
		const std::vector<Tests::Perf::MetricValues> values = {
			{ { "A", 5.0 }, { "B", 2.5 } },
			{ { "A", 50.0 }, { "B", 1.5 } } };
		uint32_t calls = 0;
		std::vector<Tests::Perf::ConfirmedMetric> metrics = test.Checker.CheckConfirmed({ "A", "B" }, [&] { return values.at(calls++); });
		CHECK(calls == 2);
		REQUIRE(metrics.size() == 2);
		CHECK(metrics[0].Key == "A");
		CHECK(metrics[0].Passed);
		CHECK(metrics[0].Attempts == std::vector<double> { 5.0 });
		CHECK(metrics[1].Key == "B");
		CHECK(metrics[1].Passed);
		CHECK(metrics[1].Attempts == std::vector<double> { 2.5, 1.5 });
		// B's target is recorded with it.
		const std::optional<nlohmann::json> recordedB = ReadResultEntry(test.ResultsFile, "B");
		REQUIRE(recordedB.has_value());
		CHECK(recordedB->value("Target", 0.0) == 1.0);

		calls = 0;
		metrics = test.Checker.CheckConfirmed({ "B", "A" }, [&]
		{
			calls++;
			return Tests::Perf::MetricValues { { "A", 1.0 }, { "B", 3.0 } };
		});
		CHECK(calls == Tests::Perf::c_ConfirmationAttempts);
		REQUIRE(metrics.size() == 2);
		CHECK(metrics[0].Key == "B");
		CHECK_FALSE(metrics[0].Passed);
		CHECK(metrics[0].Attempts.size() == Tests::Perf::c_ConfirmationAttempts);
		CHECK(metrics[1].Passed);
		CHECK(metrics[1].Attempts == std::vector<double> { 1.0 });
	}

	// Exactly one failed assertion: the missing budget, before anything is measured (a measurement would fail too).
	TEST_CASE("A confirmed check of a metric without a budget fails before measuring" * doctest::expected_failures(1))
	{
		TestChecker test;
		test.Checker.CheckConfirmed({ "A", "Missing" }, []
		{
			FAIL_CHECK("a metric without a budget was measured");
			return Tests::Perf::MetricValues {};
		});
	}

	// Exactly one failed assertion: the measurement reported another metric than the check lists.
	TEST_CASE("A confirmed check fails when the measurement does not report its metrics" * doctest::expected_failures(1))
	{
		TestChecker test;
		test.Checker.CheckConfirmed({ "A" }, [] { return Tests::Perf::MetricValues { { "B", 1.0 } }; });
	}
}

TEST_SUITE("Perf.Harness")
{
	TEST_CASE("Measure times each iteration after an untimed warmup")
	{
		constexpr uint32_t c_Warmup = 2;
		constexpr uint32_t c_Iterations = 21;
		// Warmup calls take far longer than timed ones, so a maximum below their duration shows that none was timed.
		constexpr auto c_WarmupDuration = std::chrono::milliseconds(500);
		constexpr auto c_TimedDuration = std::chrono::microseconds(500);

		uint32_t calls = 0;
		const Tests::Perf::Measurement measurement = Tests::Perf::Measure("Perf.Harness.Synthetic", c_Warmup, c_Iterations, [&]
		{
			if (calls++ < c_Warmup)
			{
				std::this_thread::sleep_for(c_WarmupDuration);
				return;
			}
			// Spins rather than sleeps: sleeps last at least a scheduler tick, which would hide the timing of the call.
			const auto end = std::chrono::steady_clock::now() + c_TimedDuration;
			while (std::chrono::steady_clock::now() < end)
			{
			}
		});

		CHECK(calls == c_Warmup + c_Iterations);
		CHECK(measurement.Iterations == c_Iterations);
		CHECK(measurement.MinMs >= 0.5);
		CHECK(measurement.MinMs <= measurement.MedianMs);
		CHECK(measurement.MedianMs <= measurement.P95Ms);
		CHECK(measurement.P95Ms <= measurement.MaxMs);
		CHECK(measurement.MaxMs < 500.0);
	}

	TEST_CASE("The sort canary stays within its budget and is recorded with its attempts")
	{
		constexpr size_t c_Count = 200'000;
		std::vector<uint32_t> input(c_Count);
		uint32_t state = 0x9E3779B9u; // xorshift32: the same values on every machine
		for (uint32_t& value : input)
		{
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			value = state;
		}

		// The median of 31 sorts: a few sorts slowed down by the machine's other work do not move it.
		std::vector<uint32_t> values;
		const Tests::Perf::ConfirmedMetric canary = Tests::Perf::CheckBudgetConfirmed(c_CanaryMetric, [&]
		{
			return Tests::Perf::Measure(c_CanaryMetric, 3, 31, [&]
			{
				values = input;
				std::sort(values.begin(), values.end());
			}).MedianMs;
		});
		CHECK(std::is_sorted(values.begin(), values.end()));

		// The check recorded the metric with the unit and budget of its entry and every attempt.
		const std::optional<Tests::Perf::BudgetEntry> budget = Tests::Perf::FindBudget(c_CanaryMetric);
		REQUIRE(budget.has_value());
		const std::optional<nlohmann::json> recorded = ReadResultEntry(Tests::Perf::GetResultsFile(), c_CanaryMetric);
		REQUIRE(recorded.has_value());
		CHECK(IsResultEntry(*recorded, canary.Value, "ms", budget->Budget, canary.Attempts));
	}
}
