#include "UI/TextFormat.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/PlatformDetection.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace Strata::UI
{

	namespace
	{

		bool IsSeparator(char character)
		{
			return character == ' ' || character == '-' || character == '_' || character == '.';
		}

		bool IsAsciiLower(char character)
		{
			return character >= 'a' && character <= 'z';
		}

		bool IsAsciiUpper(char character)
		{
			return character >= 'A' && character <= 'Z';
		}

		// The UTF-8 character starting at `offset`, ASCII letters uppercased.
		std::string GetCharacter(std::string_view text, size_t offset)
		{
			size_t end = offset + 1;
			while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
				end++;
			std::string character(text.substr(offset, end - offset));
			if (character.size() == 1 && IsAsciiLower(character[0]))
				character[0] = static_cast<char>(character[0] - 'a' + 'A');
			return character;
		}

	}

	std::string DisplayPath(const std::filesystem::path& path)
	{
		std::string text = FileSystem::ToUTF8(path);
#if defined(ST_PLATFORM_WINDOWS)
		std::replace(text.begin(), text.end(), '/', '\\');
#endif
		return text;
	}

	std::string DescribeTimeAgo(int64_t then, int64_t now)
	{
		constexpr int64_t c_Minute = 60;
		constexpr int64_t c_Hour = 60 * c_Minute;
		constexpr int64_t c_Day = 24 * c_Hour;
		const int64_t elapsed = now - then;
		if (elapsed < c_Minute)
			return "just now";
		if (elapsed < c_Hour)
			return elapsed < 2 * c_Minute ? "a minute ago" : fmt::format("{} minutes ago", elapsed / c_Minute);
		if (elapsed < c_Day)
			return elapsed < 2 * c_Hour ? "an hour ago" : fmt::format("{} hours ago", elapsed / c_Hour);
		if (elapsed < 2 * c_Day)
			return "yesterday";
		if (elapsed < 30 * c_Day)
			return fmt::format("{} days ago", elapsed / c_Day);
		// Older: the date (UTC).
		const std::chrono::sys_days day = std::chrono::floor<std::chrono::days>(std::chrono::sys_seconds(std::chrono::seconds(then)));
		const std::chrono::year_month_day date(day);
		return fmt::format("{:04}-{:02}-{:02}", static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()));
	}

	std::string GetInitials(std::string_view name)
	{
		std::vector<size_t> wordStarts;
		for (size_t index = 0; index < name.size(); index++)
		{
			if (!IsSeparator(name[index]) && (index == 0 || IsSeparator(name[index - 1])))
				wordStarts.push_back(index);
		}
		if (wordStarts.empty())
			return "?";
		if (wordStarts.size() >= 2)
			return GetCharacter(name, wordStarts[0]) + GetCharacter(name, wordStarts[1]);

		// One word: its first character, and the next capital that follows a lowercase letter ("FeatureTest").
		const size_t start = wordStarts[0];
		std::string initials = GetCharacter(name, start);
		for (size_t index = start + 1; index < name.size() && !IsSeparator(name[index]); index++)
		{
			if (IsAsciiUpper(name[index]) && IsAsciiLower(name[index - 1]))
			{
				initials += name[index];
				break;
			}
		}
		return initials;
	}

}
