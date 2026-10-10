#include "Perf/PerfUtils.h"

#include "Strata/Core/FileSystem.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

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

		// The budget entry of key, or the reason there is none.
		const BudgetEntry* LookUpBudget(std::string_view key, std::string& error)
		{
			const LoadedBudgets& budgets = GetLoadedBudgets();
			if (!budgets.Table)
			{
				error = budgets.Error;
				return nullptr;
			}
			const auto entry = budgets.Table->find(key);
			if (entry == budgets.Table->end())
			{
				error = fmt::format("metric '{}' has no budget in '{}': measure it on the reference machine and add it (see the "
					"file's _comment)", key, FileSystem::ToUTF8(GetBudgetFile()));
				return nullptr;
			}
			return &entry->second;
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

		// The process's records, written whole on every Record so a file never misses one.
		std::mutex s_ResultsMutex;
		ResultTable s_Results;

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
				if (member.key() != "Measured" && member.key() != "Budget" && member.key() != "Unit")
				{
					error = fmt::format("'{}' has an unknown member '{}' (Measured, Budget and Unit are known)", key, member.key());
					return std::nullopt;
				}
			}

			const std::optional<double> measured = ReadBudgetValue(value, "Measured", key, error);
			const std::optional<double> budget = measured ? ReadBudgetValue(value, "Budget", key, error) : std::nullopt;
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

			BudgetEntry& entry = table[key];
			entry.Measured = *measured;
			entry.Budget = *budget;
			entry.Unit = unit->get<std::string>();
		}
		return table;
	}

	std::optional<BudgetEntry> FindBudget(std::string_view key)
	{
		std::string error;
		const BudgetEntry* entry = LookUpBudget(key, error);
		if (!entry)
			return std::nullopt;
		return *entry;
	}

	double Budget(std::string_view key)
	{
		std::string error;
		const BudgetEntry* entry = LookUpBudget(key, error);
		REQUIRE_MESSAGE(entry != nullptr, error);
		return entry->Budget;
	}

	void Record(std::string_view key, double value)
	{
		std::string error;
		const BudgetEntry* entry = LookUpBudget(key, error);
		REQUIRE_MESSAGE(entry != nullptr, error);
		REQUIRE_MESSAGE(std::isfinite(value), fmt::format("'{}' measured a value that is not finite", key));

		std::lock_guard lock(s_ResultsMutex);
		Result& result = s_Results[std::string(key)];
		result.Value = value;
		result.Unit = entry->Unit;
		result.Budget = entry->Budget;
		const bool written = MergeResults(GetResultsFile(), s_Results, error);
		REQUIRE_MESSAGE(written, error);
	}

	void CheckBudget(std::string_view key, double value)
	{
		REQUIRE_MESSAGE(IsOptimizedBuild(), "perf budgets apply to optimized builds (Release, Dist) only");
		Record(key, value);

		// Record has failed the test unless the metric has a budget.
		const BudgetEntry entry = *FindBudget(key);
		std::printf("[perf] %.*s: %.4f %s (budget %.4f %s, %.0f%%)\n", static_cast<int>(key.size()), key.data(), value, entry.Unit.c_str(),
			entry.Budget, entry.Unit.c_str(), entry.Budget > 0.0 ? value / entry.Budget * 100.0 : 0.0);
		std::fflush(stdout);
		CHECK_MESSAGE(value <= entry.Budget, fmt::format("'{}' measured {} {}, over its budget of {} {}", key, value, entry.Unit, entry.Budget,
			entry.Unit));
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
			entry["Budget"] = result.Budget;
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
