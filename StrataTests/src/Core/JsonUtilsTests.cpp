#include <doctest/doctest.h>

#include "Strata/Core/JsonUtils.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// A document of `count` objects, a few kilobytes per hundred.
	std::string MakeDocument(size_t count)
	{
		nlohmann::json items = nlohmann::json::array();
		for (size_t index = 0; index < count; index++)
			items.push_back({ { "Name", "Item " + std::to_string(index) }, { "Value", static_cast<double>(index) * 0.5 }, { "Tags", { "a", "b" } } });
		return nlohmann::json { { "Items", std::move(items) } }.dump();
	}

}

TEST_SUITE("Core.Json")
{
	TEST_CASE("A parse with progress reads what a plain parse reads and reports its position every interval and at the end")
	{
		const std::string text = MakeDocument(500);
		REQUIRE(text.size() > 4096);
		const std::optional<nlohmann::json> plain = JsonUtils::Parse(text);
		REQUIRE(plain.has_value());

		std::vector<size_t> reports;
		JsonParseProgress progress;
		progress.Interval = 1000;
		progress.Callback = [&reports](size_t parsedBytes) { reports.push_back(parsedBytes); };
		const std::optional<nlohmann::json> reported = JsonUtils::Parse(text, progress);
		REQUIRE(reported.has_value());
		CHECK(*reported == *plain);

		// One report per interval passed, each an interval after the last, and the end once.
		REQUIRE(reports.size() == (text.size() - 1) / progress.Interval + 1);
		for (size_t index = 0; index + 1 < reports.size(); index++)
			CHECK(reports[index] == (index + 1) * progress.Interval);
		CHECK(reports.back() == text.size());

		// A text shorter than the interval reports only its end.
		reports.clear();
		REQUIRE(JsonUtils::Parse("[1, 2, 3]", progress).has_value());
		CHECK(reports == std::vector<size_t> { 9 });
	}

	TEST_CASE("A parse with progress fails like a plain parse on invalid text, and without a callback is a plain parse")
	{
		size_t reports = 0;
		JsonParseProgress progress;
		progress.Interval = 4;
		progress.Callback = [&reports](size_t) { reports++; };
		for (const char* invalid : { "", "{", "[1, 2", "{\"a\": }", "[1] [2]", "nul" })
		{
			std::string error;
			CHECK_FALSE(JsonUtils::Parse(invalid, progress, &error).has_value());
			CHECK(error == "Invalid JSON");
		}

		// A zero interval reports after every character rather than never.
		reports = 0;
		progress.Interval = 0;
		REQUIRE(JsonUtils::Parse("[10, 20]", progress).has_value());
		CHECK(reports == 8);

		const JsonParseProgress none;
		const std::optional<nlohmann::json> parsed = JsonUtils::Parse("{\"a\": [1, 2]}", none);
		REQUIRE(parsed.has_value());
		CHECK((*parsed)["a"].size() == 2);
	}

	TEST_CASE("Strings are appended as Dump writes them, escapes and invalid UTF-8 included")
	{
		const std::string texts[] = { "", "plain text 123", "Cube 999999", "quote \" and backslash \\", "tab\tnew line\ncarriage\r",
			"control \x01\x1f and delete \x7f", "UTF-8: \xC3\xA9t\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80",
			"invalid: \xC3 \xFF\xFE end", "truncated \xE2\x82", "/slash" };
		for (const std::string& text : texts)
		{
			CAPTURE(text);
			std::string appended = "prefix";
			JsonUtils::AppendString(appended, text);
			CHECK(appended == "prefix" + JsonUtils::Dump(nlohmann::json(text)));
		}
	}
}

