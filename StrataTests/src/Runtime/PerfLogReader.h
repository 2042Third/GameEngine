#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Reading the CSV files StrataRuntime --perf-log writes (Strata/Runtime/FramePerfLog.h), for tests and the
// check-perf-log helper mode.
namespace Strata::Tests
{

	struct PerfLogTable
	{
		std::vector<std::string> Columns;
		// Per row, one value per column; empty fields (no asset manager) read as NaN.
		std::vector<std::vector<double>> Rows;

		// The index of a column; nullopt when there is none.
		std::optional<size_t> FindColumn(std::string_view name) const;
	};

	// Parses a perf log: a header row (fields may be quoted, RFC 4180) and rows of numbers, each with one field per column.
	// Nullopt with the reason in error for anything else.
	std::optional<PerfLogTable> ParsePerfLog(std::string_view text, std::string& error);
	std::optional<PerfLogTable> ReadPerfLog(const std::filesystem::path& path, std::string& error);

	// The checks of the check-perf-log helper mode and the runtime's perf log tests: exactly `rows` rows of consecutive
	// frames, every built-in frame stage among the columns, finite times. False with the reason in error otherwise.
	bool ValidatePerfLog(const PerfLogTable& table, uint64_t rows, std::string& error);

}
