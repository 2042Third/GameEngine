#include "stpch.h"
#include "Strata/Asset/ColorLookupImporter.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/StringUtils.h"
#include "Strata/Renderer/ColorLookup.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>

namespace Strata
{

	namespace
	{

		// The stored table's sizes the "Size" setting chooses from ("File", 0: the file's own).
		struct SizeChoice
		{
			std::string_view Name;
			uint32_t Size = 0;
		};
		constexpr std::array<SizeChoice, 4> c_SizeChoices = { { { "File", 0 }, { "17", 17 }, { "33", 33 }, { "65", 65 } } };
		constexpr SizeChoice c_DefaultSize = c_SizeChoices[2];
		// Cancellation is checked this often while the values are read.
		constexpr uint32_t c_CancelCheckLines = 4096;
		// The most of a line a message quotes.
		constexpr size_t c_MaxQuotedCharacters = 60;

		// 10^0 to 10^22, the powers of ten a double holds exactly.
		constexpr double c_PowersOfTen[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
			1e20, 1e21, 1e22 };
		// Significands take another digit only below this ((2^53 - 9) / 10), so they stay below 2^53, exact in a double.
		constexpr uint64_t c_MaxSignificand = 900719925474098ull;

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

		bool IsSpace(char character)
		{
			return character == ' ' || character == '\t' || character == '\r' || character == '\f' || character == '\v';
		}

		bool IsDigit(char character)
		{
			return character >= '0' && character <= '9';
		}

		bool IsKeywordStart(char character)
		{
			return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
		}

		std::string_view Trim(std::string_view text)
		{
			while (!text.empty() && IsSpace(text.front()))
				text.remove_prefix(1);
			while (!text.empty() && IsSpace(text.back()))
				text.remove_suffix(1);
			return text;
		}

		// The next whitespace-separated token of `text`, removed from it; empty at its end.
		std::string_view NextToken(std::string_view& text)
		{
			text = Trim(text);
			size_t end = 0;
			while (end < text.size() && !IsSpace(text[end]))
				end++;
			const std::string_view token = text.substr(0, end);
			text.remove_prefix(end);
			return token;
		}

		// A part of the file for a message: valid UTF-8, shortened.
		std::string Quote(std::string_view text)
		{
			if (text.size() > c_MaxQuotedCharacters)
				return StringUtils::MakeValidUTF8(text.substr(0, c_MaxQuotedCharacters)) + "...";
			return StringUtils::MakeValidUTF8(text);
		}

		// A decimal number, the whole token: [+-] digits [. digits] [(e|E) [+-] digits], at least one digit before the exponent.
		// The C locale plays no part. Significant digits beyond a double's exact range are dropped (the table is stored in half
		// floats). Nullopt for anything else, and for numbers beyond a double's range.
		std::optional<double> ParseNumber(std::string_view token)
		{
			size_t position = 0;
			const bool negative = position < token.size() && token[position] == '-';
			if (position < token.size() && (token[position] == '-' || token[position] == '+'))
				position++;
			uint64_t significand = 0;
			int exponent = 0;
			bool digits = false;
			for (; position < token.size() && IsDigit(token[position]); position++)
			{
				digits = true;
				if (significand < c_MaxSignificand)
					significand = significand * 10 + static_cast<uint64_t>(token[position] - '0');
				else
					exponent++;
			}
			if (position < token.size() && token[position] == '.')
			{
				for (position++; position < token.size() && IsDigit(token[position]); position++)
				{
					digits = true;
					if (significand < c_MaxSignificand)
					{
						significand = significand * 10 + static_cast<uint64_t>(token[position] - '0');
						exponent--;
					}
				}
			}
			if (!digits)
				return std::nullopt;
			if (position < token.size() && (token[position] == 'e' || token[position] == 'E'))
			{
				position++;
				const bool negativeExponent = position < token.size() && token[position] == '-';
				if (position < token.size() && (token[position] == '-' || token[position] == '+'))
					position++;
				int value = 0;
				bool exponentDigits = false;
				for (; position < token.size() && IsDigit(token[position]); position++)
				{
					exponentDigits = true;
					value = std::min(value * 10 + (token[position] - '0'), 100000);
				}
				if (!exponentDigits)
					return std::nullopt;
				exponent += negativeExponent ? -value : value;
			}
			if (position != token.size())
				return std::nullopt;

			double number = static_cast<double>(significand);
			if (significand != 0)
			{
				if (exponent >= 0)
					number = exponent <= 22 ? number * c_PowersOfTen[exponent] : number * std::pow(10.0, exponent);
				else
					number = -exponent <= 22 ? number / c_PowersOfTen[-exponent] : number / std::pow(10.0, -exponent);
			}
			if (!std::isfinite(number))
				return std::nullopt;
			return negative ? -number : number;
		}

		// Exactly `count` numbers, the whole of `text`.
		bool ParseNumbers(std::string_view text, float* outValues, size_t count)
		{
			for (size_t index = 0; index < count; index++)
			{
				const std::optional<double> value = ParseNumber(NextToken(text));
				if (!value || std::abs(*value) > static_cast<double>(std::numeric_limits<float>::max()))
					return false;
				outValues[index] = static_cast<float>(*value);
			}
			return Trim(text).empty();
		}

		// The setting's size (0: the file's own); nullopt for a name that is none of the choices.
		std::optional<uint32_t> ParseSize(const nlohmann::json& settings)
		{
			const std::string size = JsonUtils::GetString(settings, "Size", std::string(c_DefaultSize.Name));
			for (const SizeChoice& choice : c_SizeChoices)
			{
				if (choice.Name == size)
					return choice.Size;
			}
			return std::nullopt;
		}

	}

	std::optional<CubeTable> ParseCubeFile(std::span<const uint8_t> bytes, std::string* outError, const std::atomic<bool>* cancel)
	{
		std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		if (text.starts_with("\xEF\xBB\xBF"))
			text.remove_prefix(3); // A UTF-8 byte order mark

		CubeTable table;
		bool hasDomain = false;     // DOMAIN_MIN or DOMAIN_MAX
		bool hasInputRange = false; // LUT_3D_INPUT_RANGE
		uint64_t expected = 0;
		uint32_t lineNumber = 0;
		auto fail = [&](std::string message) -> std::optional<CubeTable>
		{
			Fail(outError, fmt::format("line {}: {}", lineNumber, message));
			return std::nullopt;
		};
		while (!text.empty())
		{
			lineNumber++;
			const size_t end = text.find('\n');
			std::string_view line = text.substr(0, end);
			text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
			if (lineNumber % c_CancelCheckLines == 0 && cancel && cancel->load(std::memory_order_relaxed))
				return fail("cancelled");

			line = Trim(line);
			const bool title = line.starts_with("TITLE") && (line.size() == 5 || IsSpace(line[5]));
			// Comments: whole lines, or the rest of a line (but in a title, which may quote a '#').
			if (!title)
				line = Trim(line.substr(0, line.find('#')));
			if (line.empty())
				continue;

			if (IsKeywordStart(line.front()))
			{
				std::string_view rest = line;
				const std::string_view keyword = NextToken(rest);
				if (!table.Colors.empty())
					return fail(fmt::format("the keyword {} comes after the table's values", Quote(keyword)));
				if (keyword == "TITLE")
				{
					std::string_view value = Trim(rest);
					if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
						value = value.substr(1, value.size() - 2);
					table.Title = StringUtils::MakeValidUTF8(value);
				}
				else if (keyword == "LUT_3D_SIZE")
				{
					const std::string_view token = NextToken(rest);
					const std::optional<double> size = ParseNumber(token);
					if (table.Size != 0)
						return fail("LUT_3D_SIZE is given twice");
					if (!size || !Trim(rest).empty() || *size != std::floor(*size))
						return fail(fmt::format("LUT_3D_SIZE needs a whole number, not {}", Quote(Trim(line.substr(keyword.size())))));
					if (*size < ColorLookup::c_MinSize || *size > ColorLookup::c_MaxSize)
						return fail(fmt::format("LUT_3D_SIZE {} is outside {} to {}", Quote(token), ColorLookup::c_MinSize, ColorLookup::c_MaxSize));
					table.Size = static_cast<uint32_t>(*size);
					expected = static_cast<uint64_t>(table.Size) * table.Size * table.Size;
					table.Colors.reserve(static_cast<size_t>(expected));
				}
				else if (keyword == "LUT_1D_SIZE")
				{
					return fail("a 1D table (LUT_1D_SIZE): only 3D tables (LUT_3D_SIZE) are imported; export the grade as a 3D lookup");
				}
				else if (keyword == "DOMAIN_MIN" || keyword == "DOMAIN_MAX")
				{
					float values[3] = {};
					if (!ParseNumbers(rest, values, 3))
						return fail(fmt::format("{} needs three numbers, not {}", keyword, Quote(Trim(rest))));
					if (hasInputRange)
						return fail(fmt::format("{} and LUT_3D_INPUT_RANGE both give the domain", keyword));
					glm::vec3& domain = keyword == "DOMAIN_MIN" ? table.DomainMin : table.DomainMax;
					domain = glm::vec3(values[0], values[1], values[2]);
					hasDomain = true;
				}
				else if (keyword == "LUT_3D_INPUT_RANGE")
				{
					float values[2] = {};
					if (!ParseNumbers(rest, values, 2))
						return fail(fmt::format("LUT_3D_INPUT_RANGE needs two numbers, not {}", Quote(Trim(rest))));
					if (hasDomain)
						return fail("LUT_3D_INPUT_RANGE and DOMAIN_MIN or DOMAIN_MAX both give the domain");
					table.DomainMin = glm::vec3(values[0]);
					table.DomainMax = glm::vec3(values[1]);
					hasInputRange = true;
				}
				else
				{
					table.Warnings.push_back(fmt::format("line {}: the keyword {} is not known and was ignored", lineNumber, Quote(keyword)));
				}
				continue;
			}

			// A value line.
			if (table.Size == 0)
				return fail(fmt::format("the value {} comes before LUT_3D_SIZE", Quote(line)));
			if (table.Colors.size() >= expected)
				return fail(fmt::format("more than the {}^3 values LUT_3D_SIZE gives", table.Size));
			float values[3] = {};
			if (!ParseNumbers(line, values, 3))
				return fail(fmt::format("expected three numbers, found {}", Quote(line)));
			table.Colors.emplace_back(values[0], values[1], values[2]);
		}

		if (table.Size == 0)
		{
			Fail(outError, "no LUT_3D_SIZE: not a 3D .cube table");
			return std::nullopt;
		}
		if (table.Colors.size() < expected)
			return fail(fmt::format("the table ends after {} of its {}^3 values", table.Colors.size(), table.Size));
		for (int channel = 0; channel < 3; channel++)
		{
			if (!(table.DomainMin[channel] < table.DomainMax[channel]))
			{
				Fail(outError, fmt::format("the domain is empty: its minimum {} is not below its maximum {}", table.DomainMin, table.DomainMax));
				return std::nullopt;
			}
		}
		return table;
	}

	std::vector<glm::vec3> ResampleCubeTable(const CubeTable& table, uint32_t size, const std::atomic<bool>* cancel)
	{
		const uint64_t count = static_cast<uint64_t>(table.Size) * table.Size * table.Size;
		if (table.Size < 2 || table.Colors.size() != count || size < 2)
			return {};
		if (size == table.Size && table.DomainMin == glm::vec3(0.0f) && table.DomainMax == glm::vec3(1.0f))
			return table.Colors;

		std::vector<glm::vec3> colors;
		colors.reserve(static_cast<size_t>(size) * size * size);
		const glm::vec3 domainScale = 1.0f / (table.DomainMax - table.DomainMin);
		const double last = static_cast<double>(size - 1);
		for (uint32_t blue = 0; blue < size; blue++)
		{
			if (cancel && cancel->load(std::memory_order_relaxed))
				return {};
			for (uint32_t green = 0; green < size; green++)
			{
				for (uint32_t red = 0; red < size; red++)
				{
					const glm::vec3 input(static_cast<float>(red / last), static_cast<float>(green / last), static_cast<float>(blue / last));
					colors.push_back(ColorLookup::Sample(table.Size, table.Colors, (input - table.DomainMin) * domainScale));
				}
			}
		}
		return colors;
	}

	nlohmann::json ColorLookupImporter::GetDefaultSettings(const std::filesystem::path&) const
	{
		nlohmann::json settings = nlohmann::json::object();
		settings["Size"] = std::string(c_DefaultSize.Name);
		return settings;
	}

	std::vector<PropertyInfo> ColorLookupImporter::GetSettingsProperties() const
	{
		std::vector<PropertyInfo> properties;
		std::vector<std::string> choices;
		for (const SizeChoice& choice : c_SizeChoices)
			choices.emplace_back(choice.Name);
		ImportSettingsBuilder(properties)
			.Enum("Size", choices, c_DefaultSize.Name)
				.Tooltip("Points of the stored table each way: more follow a grade's fine changes more closely and take more memory (17: 39 KB, "
					"33: 287 KB, 65: 2.2 MB). File keeps the file's own size; other sizes are resampled");
		return properties;
	}

	bool ColorLookupImporter::Import(const ImportContext& context, ImportResult& result, std::string* outError) const
	{
		const std::string name = FileSystem::ToUTF8(context.SourcePath.filename());
		const std::optional<std::vector<uint8_t>> source = ReadImportSource(context);
		if (!source)
			return Fail(outError, fmt::format("Could not read '{}'", FileSystem::ToUTF8(context.SourcePath)));

		std::string error;
		std::optional<CubeTable> table = ParseCubeFile(*source, &error, context.Cancel);
		if (!table)
			return Fail(outError, context.IsCancelled() ? std::string("cancelled") : fmt::format("{}: {}", name, error));

		std::optional<uint32_t> size = ParseSize(context.Settings);
		if (!size)
		{
			result.Warnings.push_back(fmt::format("Unknown color lookup size '{}'; expected File, 17, 33 or 65", JsonUtils::GetString(context.Settings, "Size", "")));
			size = c_DefaultSize.Size;
		}
		if (*size == 0)
			size = table->Size;
		const std::vector<glm::vec3> colors = ResampleCubeTable(*table, *size, context.Cancel);
		if (context.IsCancelled())
			return Fail(outError, "cancelled");
		result.Data = ColorLookup::Serialize(*size, colors, &error);
		if (result.Data.empty())
			return Fail(outError, fmt::format("{}: {}", name, error));
		for (std::string& warning : table->Warnings)
			result.Warnings.push_back(fmt::format("{}: {}", name, warning));
		return true;
	}

}
