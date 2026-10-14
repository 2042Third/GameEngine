#include "Perf/PerfUtils.h"

#include "Strata/Core/FileSystem.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <utility>

namespace Strata::Tests::Perf
{

	namespace
	{

		// Rounding of the decimal values people write into the budget file.
		constexpr double c_HeadroomTolerance = 1e-9;

		struct LoadedBudgets
		{
			std::optional<BudgetTable> Table;
			std::string Error;
		};

		// The budget file, read once per process.
		const LoadedBudgets& GetLoadedBudgets()
		{
			static const LoadedBudgets s_Budgets = []
			{
				LoadedBudgets budgets;
				const std::filesystem::path file = GetBudgetFile();
				const std::optional<std::string> text = FileSystem::ReadText(file);
				if (!text)
				{
					budgets.Error = fmt::format("cannot read '{}'", FileSystem::ToUTF8(file));
					return budgets;
				}
				std::string error;
				budgets.Table = ParseBudgets(*text, error);
				if (!budgets.Table)
					budgets.Error = fmt::format("'{}' is malformed: {}", FileSystem::ToUTF8(file), error);
				return budgets;
			}();
			return s_Budgets;
		}

		bool IsKnownUnit(std::string_view unit)
		{
			return std::find(std::begin(c_BudgetUnits), std::end(c_BudgetUnits), unit) != std::end(c_BudgetUnits);
		}

		// The value of a budget entry member: a finite number that is not negative.
		std::optional<double> ReadBudgetValue(const nlohmann::json& entry, const char* member, const std::string& key, std::string& error)
		{
			const auto value = entry.find(member);
			if (value == entry.end() || !value->is_number())
			{
				error = fmt::format("'{}' needs a number '{}'", key, member);
				return std::nullopt;
			}
			const double number = value->get<double>();
			if (!std::isfinite(number) || number < 0.0)
			{
				error = fmt::format("'{}' has a negative or infinite '{}'", key, member);
				return std::nullopt;
			}
			return number;
		}

		// "<value> <unit> (budget <budget> <unit>, <percent>%[; target <target> <unit>])", the way checks print a value;
		// "<value> <unit> (provisional, no budget[; target <target> <unit>])" for a provisional metric.
		std::string DescribeValue(double value, const BudgetEntry& entry)
		{
			std::string text = entry.Provisional ? fmt::format("{:.4f} {} (provisional, no budget until the reference machine measures it", value, entry.Unit)
				: fmt::format("{:.4f} {} (budget {:.4f} {}, {:.0f}%", value, entry.Unit, entry.Budget, entry.Unit,
					entry.Budget > 0.0 ? value / entry.Budget * 100.0 : 0.0);
			if (entry.Target)
				text += fmt::format("; target {:.4f} {}", *entry.Target, entry.Unit);
			return text + ")";
		}

		std::string JoinValues(const std::vector<double>& values, std::string_view unit)
		{
			std::string text;
			for (const double value : values)
				text += fmt::format("{}{} {}", text.empty() ? "" : ", ", value, unit);
			return text;
		}

		void Print(const std::string& line)
		{
			std::printf("[perf] %s\n", line.c_str());
			std::fflush(stdout);
		}

	}

	Measurement Summarize(std::span<const double> samplesMs)
	{
		Measurement measurement;
		if (samplesMs.empty())
			return measurement;

		std::vector<double> sorted(samplesMs.begin(), samplesMs.end());
		std::sort(sorted.begin(), sorted.end());
		const size_t count = sorted.size();
		measurement.Iterations = static_cast<uint32_t>(count);
		measurement.MinMs = sorted.front();
		measurement.MaxMs = sorted.back();
		measurement.MedianMs = count % 2 == 1 ? sorted[count / 2] : (sorted[count / 2 - 1] + sorted[count / 2]) * 0.5;
		// Nearest rank ceil(0.95 * count), in integers so that no rounding moves it.
		const size_t rank = (count * 95 + 99) / 100;
		measurement.P95Ms = sorted[rank - 1];
		return measurement;
	}

	Measurement Measure(std::string_view name, uint32_t warmup, uint32_t iterations, const std::function<void()>& function)
	{
		REQUIRE_MESSAGE(iterations > 0, "Measure needs at least one timed iteration");

		for (uint32_t call = 0; call < warmup; call++)
			function();

		std::vector<double> samples;
		samples.reserve(iterations);
		for (uint32_t call = 0; call < iterations; call++)
		{
			const auto start = std::chrono::steady_clock::now();
			function();
			const auto end = std::chrono::steady_clock::now();
			samples.push_back(std::chrono::duration<double, std::milli>(end - start).count());
		}

		const Measurement measurement = Summarize(samples);
		std::printf("[perf] %.*s: median %.4f ms, p95 %.4f ms, min %.4f ms, max %.4f ms (%u iterations, %u warmup)\n",
			static_cast<int>(name.size()), name.data(), measurement.MedianMs, measurement.P95Ms, measurement.MinMs, measurement.MaxMs,
			measurement.Iterations, warmup);
		std::fflush(stdout);
		return measurement;
	}

	std::optional<BudgetTable> ParseBudgets(std::string_view json, std::string& error)
	{
		const nlohmann::json document = nlohmann::json::parse(json, nullptr, false);
		if (document.is_discarded() || !document.is_object())
		{
			error = "the file is not a JSON object";
			return std::nullopt;
		}

		BudgetTable table;
		for (const auto& [key, value] : document.items())
		{
			if (key.empty())
			{
				error = "a metric has an empty name";
				return std::nullopt;
			}
			if (key.front() == '_')
				continue;
			if (!value.is_object())
			{
				error = fmt::format("'{}' is not an object", key);
				return std::nullopt;
			}
			for (const auto& member : value.items())
			{
				if (member.key() != "Measured" && member.key() != "Budget" && member.key() != "Unit" && member.key() != "Target"
					&& member.key() != "Provisional")
				{
					error = fmt::format("'{}' has an unknown member '{}' (Measured, Budget, Unit, Target and Provisional are known)", key,
						member.key());
					return std::nullopt;
				}
			}

			// A provisional entry names its unit (and a target, if the metric has one): its values come from the reference
			// machine, so it has none to be compared with until then.
			bool provisional = false;
			if (const auto marker = value.find("Provisional"); marker != value.end())
			{
				if (!marker->is_boolean() || !marker->get<bool>())
				{
					error = fmt::format("'{}' has a 'Provisional' that is not true (leave it out of entries that are not provisional)", key);
					return std::nullopt;
				}
				if (value.contains("Measured") || value.contains("Budget"))
				{
					error = fmt::format("the provisional '{}' has a 'Measured' or 'Budget': the reference machine measures them, then the "
						"entry is no longer provisional", key);
					return std::nullopt;
				}
				provisional = true;
			}

			const std::optional<double> measured = provisional ? std::optional<double>(0.0) : ReadBudgetValue(value, "Measured", key, error);
			const std::optional<double> budget = provisional ? std::optional<double>(0.0)
				: measured ? ReadBudgetValue(value, "Budget", key, error) : std::nullopt;
			if (!measured || !budget)
				return std::nullopt;
			const auto unit = value.find("Unit");
			if (unit == value.end() || !unit->is_string() || !IsKnownUnit(unit->get<std::string>()))
			{
				std::string units;
				for (std::string_view known : c_BudgetUnits)
					units += units.empty() ? std::string(known) : ", " + std::string(known);
				error = fmt::format("'{}' needs a 'Unit' of {}", key, units);
				return std::nullopt;
			}
			if (*budget > *measured * c_BudgetHeadroom * (1.0 + c_HeadroomTolerance))
			{
				error = fmt::format("the budget of '{}' ({}) exceeds {} times its measured value ({})", key, *budget, c_BudgetHeadroom, *measured);
				return std::nullopt;
			}
			std::optional<double> target;
			if (value.contains("Target"))
			{
				target = ReadBudgetValue(value, "Target", key, error);
				if (!target)
					return std::nullopt;
				// The budget enforces a target it does not exceed: such a target says nothing and is removed with the budget
				// change that reaches it.
				if (!provisional && *target >= *budget)
				{
					error = fmt::format("the target of '{}' ({}) is not below its budget ({}), which enforces it: remove the target", key, *target,
						*budget);
					return std::nullopt;
				}
			}

			BudgetEntry& entry = table[key];
			entry.Measured = *measured;
			entry.Budget = *budget;
			entry.Unit = unit->get<std::string>();
			entry.Target = target;
			entry.Provisional = provisional;
		}
		return table;
	}

	BudgetChecker::BudgetChecker(BudgetTable budgets, std::string budgetSource, std::filesystem::path resultsFile)
		: m_Budgets(std::move(budgets)), m_BudgetSource(std::move(budgetSource)), m_ResultsFile(std::move(resultsFile))
	{
	}

	const BudgetEntry* BudgetChecker::Find(std::string_view key, std::string& error) const
	{
		const auto entry = m_Budgets.find(key);
		if (entry == m_Budgets.end())
		{
			error = fmt::format("metric '{}' has no budget in '{}': measure it on the reference machine and add it (see the file's _comment)", key,
				m_BudgetSource);
			return nullptr;
		}
		return &entry->second;
	}

	double BudgetChecker::GetBudget(std::string_view key) const
	{
		std::string error;
		const BudgetEntry* entry = Find(key, error);
		REQUIRE_MESSAGE(entry != nullptr, error);
		REQUIRE_MESSAGE(!entry->Provisional, fmt::format("metric '{}' is provisional: it has no budget until the reference machine measures it "
			"(check it with CheckBudgetConfirmed, which records it)", key));
		return entry->Budget;
	}

	void BudgetChecker::Record(std::string_view key, double value)
	{
		std::string error;
		const BudgetEntry* entry = Find(key, error);
		REQUIRE_MESSAGE(entry != nullptr, error);
		REQUIRE_MESSAGE(std::isfinite(value), fmt::format("'{}' measured a value that is not finite", key));

		Result result;
		result.Value = value;
		result.Unit = entry->Unit;
		result.Budget = entry->Budget;
		result.Target = entry->Target;
		result.Provisional = entry->Provisional;
		Store(key, std::move(result));
	}


	std::vector<ConfirmedMetric> BudgetChecker::CheckConfirmed(const std::vector<std::string>& keys, const std::function<MetricValues()>& measure,
		uint32_t attempts)
	{
		REQUIRE_MESSAGE(attempts > 0, "a confirmed check needs at least one attempt");
		REQUIRE_MESSAGE(!keys.empty(), "a confirmed check needs at least one metric");

		// Every metric is known before anything is measured: measurements can take minutes.
		std::vector<ConfirmedMetric> metrics;
		metrics.reserve(keys.size());
		std::set<std::string_view> distinctKeys;
		for (const std::string& key : keys)
		{
			REQUIRE_MESSAGE(distinctKeys.insert(key).second, fmt::format("a confirmed check lists the metric '{}' twice", key));
			std::string error;
			const BudgetEntry* entry = Find(key, error);
			REQUIRE_MESSAGE(entry != nullptr, error);
			ConfirmedMetric& metric = metrics.emplace_back();
			metric.Key = key;
			metric.Entry = *entry;
		}

		// A metric that has been within its budget once is decided: noise only adds to a measurement (time taken by the
		// machine's other work, memory the graphics driver keeps), so an attempt within the budget shows that the code meets
		// it. The metrics measured together with an undecided one are measured again with it; their new values do not count.
		size_t undecided = metrics.size();
		for (uint32_t attempt = 1; attempt <= attempts && undecided > 0; attempt++)
		{
			const MetricValues values = measure();
			bool reportedKeys = values.size() == keys.size();
			for (const std::string& key : keys)
				reportedKeys = reportedKeys && values.contains(key);
			if (!reportedKeys)
			{
				std::string reported;
				for (const auto& [key, value] : values)
					reported += fmt::format("{}'{}'", reported.empty() ? "" : ", ", key);
				std::string listed;
				for (const std::string& key : keys)
					listed += fmt::format("{}'{}'", listed.empty() ? "" : ", ", key);
				FAIL(fmt::format("the measurement reported the metrics {} instead of {}", reported.empty() ? "none" : reported, listed));
			}

			for (ConfirmedMetric& metric : metrics)
			{
				if (metric.Passed)
					continue;
				const double value = values.find(metric.Key)->second;
				REQUIRE_MESSAGE(std::isfinite(value), fmt::format("'{}' measured a value that is not finite", metric.Key));
				metric.Attempts.push_back(value);
				metric.Passed = metric.Entry.Provisional || value <= metric.Entry.Budget;

				std::string line = fmt::format("{}: {}", metric.Key, DescribeValue(value, metric.Entry));
				if (metric.Passed && attempt > 1)
					line += fmt::format(": within the budget in attempt {} of {}", attempt, attempts);
				else if (!metric.Passed && attempt < attempts)
					line += fmt::format(": over the budget in attempt {} of {}, measuring again", attempt, attempts);
				else if (!metric.Passed)
					line += fmt::format(": over the budget in all {} attempts", attempts);
				Print(line);
				if (metric.Passed)
					undecided--;
			}
		}

		for (ConfirmedMetric& metric : metrics)
		{
			metric.Value = metric.Passed ? metric.Attempts.back() : *std::min_element(metric.Attempts.begin(), metric.Attempts.end());
			Result result;
			result.Value = metric.Value;
			result.Unit = metric.Entry.Unit;
			result.Budget = metric.Entry.Budget;
			result.Target = metric.Entry.Target;
			result.Attempts = metric.Attempts;
			result.Provisional = metric.Entry.Provisional;
			Store(metric.Key, std::move(result));
			CHECK_MESSAGE(metric.Passed, fmt::format("'{}' measured {} in {} attempts, each over its budget of {} {}", metric.Key,
				JoinValues(metric.Attempts, metric.Entry.Unit), metric.Attempts.size(), metric.Entry.Budget, metric.Entry.Unit));
		}
		return metrics;
	}

	void BudgetChecker::Store(std::string_view key, Result result)
	{
		std::lock_guard lock(m_ResultsMutex);
		m_Results[std::string(key)] = std::move(result);
		std::string error;
		const bool written = MergeResults(m_ResultsFile, m_Results, error);
		REQUIRE_MESSAGE(written, error);
	}

	std::optional<BudgetEntry> FindBudget(std::string_view key)
	{
		const LoadedBudgets& budgets = GetLoadedBudgets();
		if (!budgets.Table)
			return std::nullopt;
		const auto entry = budgets.Table->find(key);
		if (entry == budgets.Table->end())
			return std::nullopt;
		return entry->second;
	}

	double Budget(std::string_view key)
	{
		return GetBudgetChecker().GetBudget(key);
	}

	BudgetChecker& GetBudgetChecker()
	{
		const LoadedBudgets& budgets = GetLoadedBudgets();
		REQUIRE_MESSAGE(budgets.Table.has_value(), budgets.Error);
		static BudgetChecker s_Checker(*budgets.Table, FileSystem::ToUTF8(GetBudgetFile()), GetResultsFile());
		return s_Checker;
	}

	void Record(std::string_view key, double value)
	{
		GetBudgetChecker().Record(key, value);
	}


	ConfirmedMetric CheckBudgetConfirmed(std::string_view key, const std::function<double()>& measure, uint32_t attempts)
	{
		REQUIRE_MESSAGE(IsOptimizedBuild(), "perf budgets apply to optimized builds (Release, Dist) only");
		const std::string metric(key);
		return GetBudgetChecker().CheckConfirmed({ metric }, [&] { return MetricValues { { metric, measure() } }; }, attempts).front();
	}

	std::vector<ConfirmedMetric> CheckBudgetsConfirmed(const std::vector<std::string>& keys, const std::function<MetricValues()>& measure,
		uint32_t attempts)
	{
		REQUIRE_MESSAGE(IsOptimizedBuild(), "perf budgets apply to optimized builds (Release, Dist) only");
		return GetBudgetChecker().CheckConfirmed(keys, measure, attempts);
	}

	bool MergeResults(const std::filesystem::path& path, const ResultTable& results, std::string& error)
	{
		nlohmann::json document = nlohmann::json::object();
		if (const std::optional<std::string> existing = FileSystem::ReadText(path))
		{
			nlohmann::json parsed = nlohmann::json::parse(*existing, nullptr, false);
			if (!parsed.is_discarded() && parsed.is_object())
				document = std::move(parsed);
		}

		for (const auto& [key, result] : results)
		{
			nlohmann::json entry = nlohmann::json::object();
			entry["Value"] = result.Value;
			entry["Unit"] = result.Unit;
			if (result.Provisional)
				entry["Provisional"] = true;
			else
				entry["Budget"] = result.Budget;
			if (result.Target)
				entry["Target"] = *result.Target;
			if (!result.Attempts.empty())
				entry["Attempts"] = result.Attempts;
			document[key] = std::move(entry);
		}

		// Replaced atomically (missing directories are created), so a reader never sees half a file.
		if (!FileSystem::WriteText(path, document.dump(1, '\t') + "\n"))
		{
			error = fmt::format("cannot write '{}'", FileSystem::ToUTF8(path));
			return false;
		}
		return true;
	}

	bool IsOptimizedBuild()
	{
#if defined(ST_RELEASE) || defined(ST_DIST)
		return true;
#else
		return false;
#endif
	}

	std::filesystem::path GetBudgetFile()
	{
		return FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "StrataTests" / "Perf" / "Budgets.json";
	}

	std::filesystem::path GetResultsFile()
	{
		return FileSystem::FromUTF8(STRATA_PERF_RESULTS_FILE);
	}

}
