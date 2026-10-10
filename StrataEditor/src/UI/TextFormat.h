#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Strata::UI
{

	// A path as people read it on this system: UTF-8 with the platform's separators ("C:\Projects\Game" on Windows).
	std::string DisplayPath(const std::filesystem::path& path);

	// The last second of the year 9999 (UTC), the latest time DescribeTimeAgo tells apart.
	constexpr int64_t c_LatestTime = 253'402'300'799;

	// How long ago a moment was, for people: "just now", "5 minutes ago", "3 hours ago", "yesterday", "4 days ago", then
	// the date ("2026-03-12"). Both times are seconds since the Unix epoch, taken within 1970 to the year 9999 (any value is
	// safe); a moment in the future counts as just now.
	std::string DescribeTimeAgo(int64_t then, int64_t now);

	// One or two initials for a badge: the first letters of the first two words ("Space Game" -> "SG"), or of the first
	// two capitalized parts of one word ("FeatureTest" -> "FT"), else its first character, uppercased if it is ASCII.
	// Words are separated by spaces, '-', '_' and '.'; "?" for a name without letters or digits.
	std::string GetInitials(std::string_view name);

}
