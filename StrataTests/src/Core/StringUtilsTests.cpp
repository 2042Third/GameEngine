#include <doctest/doctest.h>

#include "Strata/Core/StringUtils.h"

#include <string>

using namespace Strata;

TEST_SUITE("Core.StringUtils")
{
	TEST_CASE("Text that is no valid UTF-8 gets replacement characters, valid text stays as it is")
	{
		const std::string replacement = "\xEF\xBF\xBD";
		// Valid text of every sequence length comes back unchanged.
		const std::string valid = "ASCII, \xC3\xA9t\xC3\xA9, \xE6\x97\xA5\xE6\x9C\xAC, \xF0\x9F\x8E\xAE";
		CHECK(StringUtils::MakeValidUTF8(valid) == valid);
		CHECK(StringUtils::MakeValidUTF8("") == "");

		// The audit's decoder message: a PNG chunk type's bytes quoted as text.
		CHECK(StringUtils::MakeValidUTF8("\x8F" "END PNG chunk not known") == replacement + "END PNG chunk not known");
		// A stray continuation byte, a truncated sequence, an overlong form, a surrogate and a code point past U+10FFFF.
		CHECK(StringUtils::MakeValidUTF8("a\x80" "b") == "a" + replacement + "b");
		CHECK(StringUtils::MakeValidUTF8("\xE6\x97") == replacement + replacement);
		CHECK(StringUtils::MakeValidUTF8("\xC0\xAF") == replacement + replacement);
		CHECK(StringUtils::MakeValidUTF8("\xED\xA0\x80") == replacement + replacement + replacement);
		CHECK(StringUtils::MakeValidUTF8("\xF4\x90\x80\x80") == replacement + replacement + replacement + replacement);
		CHECK(StringUtils::MakeValidUTF8("\xFF") == replacement);
		// What it returns is valid.
		const std::string repaired = StringUtils::MakeValidUTF8("\xE6\x97 \x8F\xFF ok");
		CHECK(StringUtils::MakeValidUTF8(repaired) == repaired);
	}
}
