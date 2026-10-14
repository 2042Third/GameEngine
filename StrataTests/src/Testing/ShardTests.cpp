#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Process.h"
#include "TestHelpers.h"

#include <spdlog/fmt/fmt.h>

#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

// How CTest runs the suites: StrataTests/CoreShards.json against the suites this binary registers.
namespace
{

	// doctest's filter semantics: '*' any characters, '?' one, case-insensitive.
	bool MatchesFilter(std::string_view name, std::string_view filter)
	{
		size_t nameIndex = 0;
		size_t filterIndex = 0;
		size_t starFilter = std::string_view::npos;
		size_t starName = 0;
		const auto same = [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
		while (nameIndex < name.size())
		{
			if (filterIndex < filter.size() && filter[filterIndex] == '*')
			{
				starFilter = filterIndex++;
				starName = nameIndex;
			}
			else if (filterIndex < filter.size() && (filter[filterIndex] == '?' || same(filter[filterIndex], name[nameIndex])))
			{
				filterIndex++;
				nameIndex++;
			}
			else if (starFilter != std::string_view::npos)
			{
				filterIndex = starFilter + 1;
				nameIndex = ++starName;
			}
			else
			{
				return false;
			}
		}
		while (filterIndex < filter.size() && filter[filterIndex] == '*')
			filterIndex++;
		return filterIndex == filter.size();
	}

	bool MatchesAny(std::string_view name, const std::vector<std::string>& filters)
	{
		for (const std::string& filter : filters)
		{
			if (MatchesFilter(name, filter))
				return true;
		}
		return false;
	}

	struct Shard
	{
		std::string Name; // "Core" for StrataTests.Core.Core, or an own CTest's name
		std::vector<std::string> Suites;
		std::vector<std::string> Exclude;
	};

	struct ShardTable
	{
		std::vector<Shard> Runs; // The Core shards and the own CTests: each suite runs in exactly one
		std::vector<std::string> FuzzSuites;
	};

	std::vector<std::string> GetStrings(const nlohmann::json& object, const char* member)
	{
		std::vector<std::string> values;
		const auto it = object.find(member);
		if (it == object.end() || !it->is_array())
			return values;
		for (const nlohmann::json& value : *it)
		{
			if (value.is_string())
				values.push_back(value.get<std::string>());
		}
		return values;
	}

	std::optional<ShardTable> ParseShards(const nlohmann::json& json)
	{
		ShardTable table;
		const auto shards = json.find("Shards");
		const auto ownTests = json.find("OwnTests");
		if (shards == json.end() || !shards->is_array() || ownTests == json.end() || !ownTests->is_array())
			return std::nullopt;
		for (const nlohmann::json& own : *ownTests)
			table.Runs.push_back({ JsonUtils::GetString(own, "Test"), GetStrings(own, "Suites"), {} });
		for (const nlohmann::json& shard : *shards)
			table.Runs.push_back({ "StrataTests.Core." + JsonUtils::GetString(shard, "Name"), GetStrings(shard, "Suites"), GetStrings(shard, "Exclude") });
		table.FuzzSuites = GetStrings(json, "FuzzSuites");
		return table;
	}

	struct ShardReport
	{
		std::vector<std::string> Problems;      // Suites in no run or in several
		std::vector<std::string> UnusedFilters; // Filters that match no suite (reported, not failed: later suites)
	};

	ShardReport CheckShards(const ShardTable& table, const std::vector<std::string>& suites)
	{
		ShardReport report;
		for (const std::string& suite : suites)
		{
			std::vector<std::string> runs;
			for (const Shard& run : table.Runs)
			{
				if (MatchesAny(suite, run.Suites) && !MatchesAny(suite, run.Exclude))
					runs.push_back(run.Name);
			}
			if (runs.empty())
			{
				report.Problems.push_back(fmt::format("{} runs in no Core shard and no CTest of its own: add it to StrataTests/CoreShards.json", suite));
			}
			else if (runs.size() > 1)
			{
				std::string names;
				for (const std::string& run : runs)
					names += (names.empty() ? "" : ", ") + run;
				report.Problems.push_back(fmt::format("{} runs in several CTests ({}): exclude it from all but one", suite, names));
			}
		}
		for (const Shard& run : table.Runs)
		{
			for (const std::string& filter : run.Suites)
			{
				bool used = false;
				for (const std::string& suite : suites)
					used = used || MatchesFilter(suite, filter);
				if (!used)
					report.UnusedFilters.push_back(fmt::format("{}: {}", run.Name, filter));
			}
		}
		return report;
	}

	std::string JoinLines(const std::vector<std::string>& lines)
	{
		std::string text;
		for (const std::string& line : lines)
			text += "\n  " + line;
		return text;
	}

	// The suites this binary registers, as `--list-test-suites` prints them (a child process: doctest's registry is not
	// part of its interface).
	std::vector<std::string> ListRegisteredSuites()
	{
		ProcessSpecification specification;
		specification.Executable = Tests::GetTestExecutablePath();
		specification.Arguments = { "--list-test-suites", "--no-intro=true" };
		const Process::RunResult result = Process::Run(specification, std::chrono::milliseconds(60000));
		REQUIRE(result.Started);
		REQUIRE_FALSE(result.TimedOut);
		REQUIRE(result.ExitCode == 0);
		std::vector<std::string> suites;
		std::istringstream lines(result.Output);
		for (std::string line; std::getline(lines, line);)
		{
			while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
				line.pop_back();
			// doctest's own lines: "[doctest] ..." and its rulers.
			if (line.empty() || line.starts_with("[doctest]") || line.starts_with("==="))
				continue;
			suites.push_back(line);
		}
		return suites;
	}

}

TEST_SUITE("Testing.Shards")
{
	TEST_CASE("Filters match like doctest's: wildcards, any case")
	{
		CHECK(MatchesFilter("Asset.Pack", "Asset*"));
		CHECK(MatchesFilter("Asset", "Asset*"));
		CHECK(MatchesFilter("asset.pack.sweep", "*.Sweep"));
		CHECK(MatchesFilter("Editor.UI.Hierarchy", "Editor.UI*"));
		CHECK(MatchesFilter("Perf.Scene", "Perf.?cene"));
		CHECK(MatchesFilter("FeatureTest", "FeatureTest"));
		CHECK_FALSE(MatchesFilter("Editor.FeatureTest", "FeatureTest"));
		CHECK_FALSE(MatchesFilter("PerfLab", "Perf.*"));
		CHECK_FALSE(MatchesFilter("PerfGPU.Editor", "Perf.*"));
		CHECK_FALSE(MatchesFilter("Assets", "Asset"));
		CHECK(MatchesFilter("a.b.c", "*b*"));
		CHECK_FALSE(MatchesFilter("abc", "a?"));
	}

	TEST_CASE("Suites in no shard or in several are found, filters that match nothing only reported")
	{
		ShardTable table;
		table.Runs.push_back({ "StrataTests.GPU", { "GPU*" }, {} });
		table.Runs.push_back({ "StrataTests.Core.Editor", { "Editor*" }, { "Editor.UI*" } });
		table.Runs.push_back({ "StrataTests.Core.EditorUI", { "Editor.UI*" }, {} });
		table.Runs.push_back({ "StrataTests.Core.Scene", { "Scene*", "World*" }, {} });
		table.Runs.push_back({ "StrataTests.Core.Core", { "Core*", "Scene.Odd" }, {} });

		const ShardReport good = CheckShards(table, { "GPU.Renderer", "Editor.Commands", "Editor.UI.Hierarchy", "Scene.Loader", "Core.Log" });
		CHECK(good.Problems.empty());
		CHECK(good.UnusedFilters == std::vector<std::string> { "StrataTests.Core.Scene: World*", "StrataTests.Core.Core: Scene.Odd" });

		const ShardReport bad = CheckShards(table, { "Audio.Engine", "Scene.Odd" });
		REQUIRE(bad.Problems.size() == 2);
		CHECK(bad.Problems[0].find("Audio.Engine runs in no Core shard") != std::string::npos);
		CHECK(bad.Problems[1].find("Scene.Odd runs in several CTests (StrataTests.Core.Scene, StrataTests.Core.Core)") != std::string::npos);
	}

	TEST_CASE("Every suite runs in exactly one Core shard or in its own CTest")
	{
		const std::optional<std::string> text = FileSystem::ReadText(FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "StrataTests" / "CoreShards.json");
		REQUIRE(text);
		const std::optional<nlohmann::json> json = JsonUtils::Parse(*text);
		REQUIRE(json);
		const std::optional<ShardTable> table = ParseShards(*json);
		REQUIRE(table);
		CHECK_FALSE(table->FuzzSuites.empty());

		const std::vector<std::string> suites = ListRegisteredSuites();
		REQUIRE(suites.size() > 100);
		const ShardReport report = CheckShards(*table, suites);
		INFO("StrataTests/CoreShards.json:", JoinLines(report.Problems));
		CHECK(report.Problems.empty());
		// Filters for suites this build does not have (other streams' layers, modules a build leaves out) are allowed.
		if (!report.UnusedFilters.empty())
			MESSAGE("Shard filters that match no suite of this build:", JoinLines(report.UnusedFilters));

		// Every sweep runs in StrataTests.Fuzz too.
		for (const std::string& suite : suites)
		{
			if (MatchesFilter(suite, "*.Sweep"))
			{
				CAPTURE(suite);
				CHECK(MatchesAny(suite, table->FuzzSuites));
			}
		}
	}
}
