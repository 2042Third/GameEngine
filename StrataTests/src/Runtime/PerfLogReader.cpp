#include "Runtime/PerfLogReader.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/FrameProfiler.h"

#include <spdlog/fmt/fmt.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>

namespace Strata::Tests
{

	namespace
	{

		// Splits one CSV line into fields, unquoting quoted ones. False for an unterminated quote.
		bool SplitFields(std::string_view line, std::vector<std::string>& outFields)
		{
			outFields.clear();
			std::string field;
			bool quoted = false;
			bool fieldStarted = false;
			for (size_t index = 0; index < line.size(); index++)
			{
				const char character = line[index];
				if (quoted)
				{
					if (character == '"' && index + 1 < line.size() && line[index + 1] == '"')
					{
						field += '"';
						index++;
					}
					else if (character == '"')
					{
						quoted = false;
					}
					else
					{
						field += character;
					}
					continue;
				}
				if (character == '"' && !fieldStarted)
				{
					quoted = true;
					fieldStarted = true;
				}
				else if (character == ',')
				{
					outFields.push_back(std::move(field));
					field.clear();
					fieldStarted = false;
				}
				else
				{
					field += character;
					fieldStarted = true;
				}
			}
			if (quoted)
				return false;
			outFields.push_back(std::move(field));
			return true;
		}

	}

	std::optional<size_t> PerfLogTable::FindColumn(std::string_view name) const
	{
		for (size_t index = 0; index < Columns.size(); index++)
		{
			if (Columns[index] == name)
				return index;
		}
		return std::nullopt;
	}

	std::optional<PerfLogTable> ParsePerfLog(std::string_view text, std::string& error)
	{
		PerfLogTable table;
		std::vector<std::string> fields;
		size_t lineNumber = 0;
		size_t position = 0;
		while (position < text.size())
		{
			const size_t end = text.find('\n', position);
			std::string_view line = text.substr(position, end == std::string_view::npos ? std::string_view::npos : end - position);
			position = end == std::string_view::npos ? text.size() : end + 1;
			lineNumber++;
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);
			if (!SplitFields(line, fields))
			{
				error = fmt::format("line {}: unterminated quote", lineNumber);
				return std::nullopt;
			}
			if (lineNumber == 1)
			{
				table.Columns = fields;
				continue;
			}
			if (fields.size() != table.Columns.size())
			{
				error = fmt::format("line {}: {} fields for {} columns", lineNumber, fields.size(), table.Columns.size());
				return std::nullopt;
			}
			std::vector<double>& row = table.Rows.emplace_back();
			for (const std::string& field : fields)
			{
				if (field.empty())
				{
					row.push_back(std::numeric_limits<double>::quiet_NaN());
					continue;
				}
				// JSON's number grammar is CSV's here, and nlohmann's parser ignores the C locale (std::from_chars for floating
				// point is missing from older standard libraries).
				const nlohmann::json value = nlohmann::json::parse(field, nullptr, false);
				if (!value.is_number())
				{
					error = fmt::format("line {}: '{}' is not a number", lineNumber, field);
					return std::nullopt;
				}
				row.push_back(value.get<double>());
			}
		}
		if (table.Columns.empty())
		{
			error = "the file has no header";
			return std::nullopt;
		}
		return table;
	}

	std::optional<PerfLogTable> ReadPerfLog(const std::filesystem::path& path, std::string& error)
	{
		const std::optional<std::string> text = FileSystem::ReadText(path);
		if (!text)
		{
			error = fmt::format("cannot read '{}'", FileSystem::ToUTF8(path));
			return std::nullopt;
		}
		return ParsePerfLog(*text, error);
	}

	bool ValidatePerfLog(const PerfLogTable& table, uint64_t rows, std::string& error)
	{
		std::vector<size_t> timeColumns;
		for (const char* column : { "Frame", "WorkMs", "IntervalMs", "WorkerBusyPercent", "IOBusyPercent" })
		{
			const std::optional<size_t> index = table.FindColumn(column);
			if (!index)
			{
				error = fmt::format("no column '{}'", column);
				return false;
			}
			timeColumns.push_back(*index);
		}
		for (const BuiltinFrameStage& stage : c_BuiltinFrameStages)
		{
			const std::optional<size_t> index = table.FindColumn(stage.Name);
			if (!index)
			{
				error = fmt::format("no column for the stage '{}'", stage.Name);
				return false;
			}
			timeColumns.push_back(*index);
		}
		if (table.Rows.size() != rows)
		{
			error = fmt::format("{} rows instead of {}", table.Rows.size(), rows);
			return false;
		}
		const size_t frameColumn = timeColumns[0];
		for (size_t row = 0; row < table.Rows.size(); row++)
		{
			for (const size_t column : timeColumns)
			{
				const double value = table.Rows[row][column];
				if (!std::isfinite(value) || value < 0.0)
				{
					error = fmt::format("row {}: {} is {}", row + 1, table.Columns[column], value);
					return false;
				}
			}
			if (row > 0 && table.Rows[row][frameColumn] != table.Rows[row - 1][frameColumn] + 1.0)
			{
				error = fmt::format("row {}: frame {} follows frame {}", row + 1, table.Rows[row][frameColumn], table.Rows[row - 1][frameColumn]);
				return false;
			}
		}
		return true;
	}

}
