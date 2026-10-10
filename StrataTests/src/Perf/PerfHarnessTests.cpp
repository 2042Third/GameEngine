#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Strata/Core/FileSystem.h"
#include "TestHelpers.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Strata;

namespace
{

	// The harness's own metric: a fixed CPU workload. When it exceeds its budget, the machine itself is slow or busy,
	// which explains other perf failures of the same run.
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

	// Whether entry holds exactly a value, unit and budget with these values.
	bool IsResultEntry(const nlohmann::json& entry, double value, std::string_view unit, double budget)
	{
		if (!entry.is_object() || entry.size() != 3)
			return false;
		const auto valueMember = entry.find("Value");
		const auto unitMember = entry.find("Unit");
		const auto budgetMember = entry.find("Budget");
		return valueMember != entry.end() && valueMember->is_number() && valueMember->get<double>() == value && unitMember != entry.end()
			&& unitMember->is_string() && unitMember->get<std::string>() == unit && budgetMember != entry.end()
			&& budgetMember->is_number() && budgetMember->get<double>() == budget;
	}

}

TEST_SUITE("Perf.Harness")
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
		};
		for (const InvalidFile& invalidFile : invalidFiles)
		{
			CAPTURE(invalidFile.Json);
			std::string error;
			CHECK_FALSE(Tests::Perf::ParseBudgets(invalidFile.Json, error).has_value());
			CHECK(error.find(invalidFile.Reason) != std::string::npos);
		}

		// Comments are skipped, a budget may be exactly 1.5 times the measurement, and a zero count is a budget too.
		const char* validFile = R"({
			"_comment": ["About this file"],
			"A": { "Measured": 3.21, "Budget": 4.815, "Unit": "ms" },
			"B": { "Measured": 0, "Budget": 0, "Unit": "count" }
		})";
		std::string error;
		const std::optional<Tests::Perf::BudgetTable> table = Tests::Perf::ParseBudgets(validFile, error);
		REQUIRE_MESSAGE(table.has_value(), error);
		REQUIRE(table->size() == 2);
		CHECK(table->at("A").Measured == 3.21);
		CHECK(table->at("A").Budget == 4.815);
		CHECK(table->at("A").Unit == "ms");
		CHECK(table->at("B").Budget == 0.0);
		CHECK(table->at("B").Unit == "count");
	}

	TEST_CASE("A metric without a budget is reported missing without failing")
	{
		CHECK_FALSE(Tests::Perf::FindBudget(c_MissingMetric).has_value());
		CHECK(Tests::Perf::FindBudget(c_CanaryMetric).has_value());
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

		// A file that holds no results object is replaced.
		REQUIRE(FileSystem::WriteText(file, "not a results file"));
		REQUIRE_MESSAGE(Tests::Perf::MergeResults(file, update, error), error);
		const std::optional<nlohmann::json> replaced = ReadResultEntry(file, "B.Metric");
		REQUIRE(replaced.has_value());
		CHECK(IsResultEntry(*replaced, 2.0, "ms", 3.0));
		CHECK_FALSE(ReadResultEntry(file, "A.Metric").has_value());
	}

	TEST_CASE("The sort canary stays within its budget and is recorded")
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

		std::vector<uint32_t> values;
		const Tests::Perf::Measurement measurement = Tests::Perf::Measure(c_CanaryMetric, 3, 31, [&]
		{
			values = input;
			std::sort(values.begin(), values.end());
		});
		CHECK(std::is_sorted(values.begin(), values.end()));
		Tests::Perf::CheckBudget(c_CanaryMetric, measurement.MedianMs);

		// CheckBudget recorded the metric with the unit and budget of its entry.
		const std::optional<Tests::Perf::BudgetEntry> budget = Tests::Perf::FindBudget(c_CanaryMetric);
		REQUIRE(budget.has_value());
		const std::optional<nlohmann::json> recorded = ReadResultEntry(Tests::Perf::GetResultsFile(), c_CanaryMetric);
		REQUIRE(recorded.has_value());
		CHECK(IsResultEntry(*recorded, measurement.MedianMs, "ms", budget->Budget));
	}
}
