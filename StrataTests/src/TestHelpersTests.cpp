#include <doctest/doctest.h>

#include "TestHelpers.h"

#include "Strata/Core/FileSystem.h"

#include <filesystem>
#include <string>

// The shared test helpers that tests rely on to explain their failures.

using namespace Strata;

TEST_SUITE("TestHelpers")
{
	TEST_CASE("DescribeJsonFile tells valid, damaged and missing JSON files apart")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("DescribeJsonFile");

		SUBCASE("A valid file is reported with its size")
		{
			const std::filesystem::path file = directory / "Valid.json";
			REQUIRE(FileSystem::WriteText(file, "{\"Entities\": [1, 2]}\n"));
			const std::string description = Tests::DescribeJsonFile(file);
			CAPTURE(description);
			CHECK(description.find("Valid.json") != std::string::npos);
			CHECK(description.find("(21 bytes) is valid JSON") != std::string::npos);
		}

		SUBCASE("A truncated file is reported with its end")
		{
			const std::filesystem::path file = directory / "Truncated.json";
			REQUIRE(FileSystem::WriteText(file, "[1, 2, 3"));
			const std::string description = Tests::DescribeJsonFile(file);
			CAPTURE(description);
			CHECK(description.find("(8 bytes) is not valid JSON") != std::string::npos);
			CHECK(description.find("unexpected end of input") != std::string::npos);
			CHECK(description.find("bytes 0 to 8: \"[1, 2, 3\"") != std::string::npos);
		}

		SUBCASE("The bytes up to a damaged byte are shown, non-printable ones escaped")
		{
			const std::filesystem::path file = directory / "Damaged.json";
			REQUIRE(FileSystem::WriteText(file, "{\"a\": \x01}"));
			const std::string description = Tests::DescribeJsonFile(file);
			CAPTURE(description);
			CHECK(description.find("(8 bytes) is not valid JSON") != std::string::npos);
			CHECK(description.find("bytes 0 to 7: \"{\"a\": \\x01\"") != std::string::npos);
		}

		SUBCASE("Only the last 64 bytes before the error are shown")
		{
			const std::filesystem::path file = directory / "Long.json";
			REQUIRE(FileSystem::WriteText(file, std::string(100, ' ') + "x"));
			const std::string description = Tests::DescribeJsonFile(file);
			CAPTURE(description);
			CHECK(description.find("(101 bytes) is not valid JSON") != std::string::npos);
			CHECK(description.find("bytes 37 to 101: \"" + std::string(63, ' ') + "x\"") != std::string::npos);
		}

		SUBCASE("A missing file cannot be read")
		{
			const std::string description = Tests::DescribeJsonFile(directory / "Missing.json");
			CAPTURE(description);
			CHECK(description.find("Missing.json' cannot be read") != std::string::npos);
		}
	}
}
