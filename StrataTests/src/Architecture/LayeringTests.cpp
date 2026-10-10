#include <doctest/doctest.h>

#include "Architecture/LayeringAnalyzer.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

using namespace Strata;
using namespace Strata::Tests::Layering;

// The engine's sources against the layer table of StrataTests/Architecture (Layers.json, LayeringAllowlist.txt), and the
// analyzer itself on in-memory file sets.
namespace
{

	std::filesystem::path GetArchitectureDirectory()
	{
		return FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "StrataTests" / "Architecture";
	}

	struct EngineSources
	{
		LayerTable Table;
		std::vector<AllowlistEntry> Allowlist;
		std::map<std::string, std::string> Files; // Relative to the table's source root
	};

	std::string JoinLines(const std::vector<std::string>& lines)
	{
		std::string text;
		for (const std::string& line : lines)
			text += "\n  " + line;
		return text;
	}

	// The layer table, the allowlist and every engine source file under the source root (IsSourceFile: C, C++ and
	// Objective-C++, such as the macOS platform code).
	EngineSources ReadEngineSources()
	{
		EngineSources sources;
		std::vector<std::string> errors;

		const std::optional<std::string> tableText = FileSystem::ReadText(GetArchitectureDirectory() / "Layers.json");
		REQUIRE(tableText);
		const std::optional<nlohmann::json> tableJson = JsonUtils::Parse(*tableText);
		REQUIRE(tableJson);
		const bool tableParsed = ParseLayerTable(*tableJson, sources.Table, errors);
		INFO("Layers.json:", JoinLines(errors));
		REQUIRE(tableParsed);
		const std::string sourceRoot = JsonUtils::GetString(*tableJson, "SourceRoot");
		REQUIRE_FALSE(sourceRoot.empty());

		const std::optional<std::string> allowlistText = FileSystem::ReadText(GetArchitectureDirectory() / "LayeringAllowlist.txt");
		REQUIRE(allowlistText);
		const bool allowlistParsed = ParseAllowlist(*allowlistText, sources.Allowlist, errors);
		INFO("LayeringAllowlist.txt:", JoinLines(errors));
		REQUIRE(allowlistParsed);

		const std::filesystem::path root = FileSystem::FromUTF8(STRATA_SOURCE_DIR) / FileSystem::FromUTF8(sourceRoot);
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error))
		{
			const std::string path = FileSystem::ToUTF8(it->path().lexically_relative(root));
			if (!it->is_regular_file() || !IsSourceFile(path))
				continue;
			const std::optional<std::string> text = FileSystem::ReadText(it->path());
			REQUIRE(text);
			sources.Files.emplace(path, *text);
		}
		REQUIRE_FALSE(error);
		REQUIRE(sources.Files.size() > 200);
		return sources;
	}

	std::vector<std::string> DescribeAll(const std::vector<Violation>& violations)
	{
		std::vector<std::string> lines;
		for (const Violation& violation : violations)
			lines.push_back(Describe(violation));
		return lines;
	}

	std::vector<std::string> DescribeAll(const std::vector<AllowlistEntry>& entries)
	{
		std::vector<std::string> lines;
		for (const AllowlistEntry& entry : entries)
			lines.push_back(fmt::format("line {}: {}:{}", entry.Line, entry.File, entry.Header));
		return lines;
	}

	// Two layers: Base (base/**) and Top (top/**), which may include Base.
	LayerTable MakeTwoLayerTable()
	{
		LayerTable table;
		table.Layers.push_back({ "Base", { "base/**" }, {}, {} });
		table.Layers.push_back({ "Top", { "top/**" }, {}, { "Base" } });
		return table;
	}

}

TEST_SUITE("Architecture.Layering")
{
	TEST_CASE("The engine's includes follow the layer table")
	{
		const EngineSources sources = ReadEngineSources();
		const LayeringReport report = CheckLayering(sources.Table, sources.Files, sources.Allowlist);

		INFO("Files without exactly one layer:", JoinLines(report.Errors));
		CHECK(report.Errors.empty());
		INFO("Forbidden includes (fix the dependency; see Layers.json):", JoinLines(DescribeAll(report.Violations)));
		CHECK(report.Violations.empty());
		INFO("Allowlist lines that match no forbidden include (remove them):", JoinLines(DescribeAll(report.StaleEntries)));
		CHECK(report.StaleEntries.empty());
		CHECK(report.CheckedIncludes > 500);

		// The list only shrinks: its length is recorded, and lowered with every line that goes.
		CHECK(sources.Allowlist.size() == sources.Table.MaxAllowlistEntries);
		CHECK(sources.Allowlist.size() <= 14);
		for (const AllowlistEntry& entry : sources.Allowlist)
		{
			CAPTURE(entry.Line);
			CHECK_FALSE(entry.Owner.empty());
			CHECK_FALSE(entry.Reason.empty());
		}
	}

	TEST_CASE("Only the composition root reaches into every module")
	{
		const EngineSources sources = ReadEngineSources();
		const LayeringReport report = CheckLayering(sources.Table, sources.Files, sources.Allowlist);

		// The registries and the scene know no module above them: no exception covers them.
		for (const Violation& violation : report.Allowlisted)
		{
			CAPTURE(Describe(violation));
			CHECK(violation.File != "Strata/Reflection/ComponentRegistry.cpp");
			CHECK(violation.File != "Strata/Scene/Scene.cpp");
			CHECK(violation.File != "Strata/Asset/AssetImporter.cpp");
			CHECK(violation.FileLayer != "Scene");
			CHECK(violation.FileLayer != "Reflection");
		}

		// A module's registration header is included by its own module and by Engine/BuiltinModules.cpp only.
		for (const auto& [path, contents] : sources.Files)
		{
			for (const IncludeDirective& include : ParseIncludes(contents))
			{
				if (!include.Path.ends_with("Registration.h"))
					continue;
				CAPTURE(path);
				CAPTURE(include.Path);
				const std::string module = include.Path.substr(0, include.Path.rfind('/') + 1);
				CHECK((path == "Strata/Engine/BuiltinModules.cpp" || path.starts_with(module)));
			}
		}
	}

	TEST_CASE("A forbidden include is reported with its file and line")
	{
		const EngineSources sources = ReadEngineSources();
		const std::string file = "Strata/Scripting/ScriptEngine.cpp";
		REQUIRE(sources.Files.contains(file));

		// Quoted, and with angle brackets: Strata/src is a public include directory, so both forms compile.
		for (const char* directive : { "#include \"Strata/Physics/PhysicsSystem.h\"", "#include <Strata/Physics/PhysicsSystem.h>" })
		{
			CAPTURE(directive);
			std::map<std::string, std::string> files = sources.Files;

			// Inject the include after the file's last line.
			std::string& contents = files[file];
			if (!contents.empty() && contents.back() != '\n')
				contents += '\n';
			const uint32_t line = static_cast<uint32_t>(std::count(contents.begin(), contents.end(), '\n')) + 1;
			contents += std::string(directive) + "\n";

			const LayeringReport report = CheckLayering(sources.Table, files, sources.Allowlist);
			REQUIRE(report.Violations.size() == 1);
			const Violation& violation = report.Violations.front();
			CHECK(violation.File == file);
			CHECK(violation.Line == line);
			CHECK(violation.Header == "Strata/Physics/PhysicsSystem.h");
			CHECK(Describe(violation) == fmt::format("Strata/Scripting/ScriptEngine.cpp({}): includes Strata/Physics/PhysicsSystem.h, but layer Scripting may not include layer Physics", line));
			CHECK(report.StaleEntries.empty());
		}
	}

	TEST_CASE("A stale allowlist line fails")
	{
		EngineSources sources = ReadEngineSources();

		// A line for an include that does not exist.
		sources.Allowlist.push_back({ "Strata/Scene/Scene.cpp", "Strata/Physics/PhysicsSystem.h", "nobody", "invented", 999 });
		LayeringReport report = CheckLayering(sources.Table, sources.Files, sources.Allowlist);
		REQUIRE(report.StaleEntries.size() == 1);
		CHECK(report.StaleEntries.front().Line == 999);
		CHECK(report.Violations.empty());
		sources.Allowlist.pop_back();

		// A line whose include went away.
		std::string& contents = sources.Files["Strata/Scripting/ScriptSystem.h"];
		const size_t include = contents.find("#include \"Strata/Physics/PhysicsTypes.h\"");
		REQUIRE(include != std::string::npos);
		contents.insert(include, "// ");
		report = CheckLayering(sources.Table, sources.Files, sources.Allowlist);
		REQUIRE(report.StaleEntries.size() == 1);
		CHECK(report.StaleEntries.front().File == "Strata/Scripting/ScriptSystem.h");
		CHECK(report.StaleEntries.front().Header == "Strata/Physics/PhysicsTypes.h");
	}

	TEST_CASE("Includes are read from code only: comments, literals and #if 0 blocks are skipped")
	{
		const std::string source =
			"#include \"a.h\"\n"                                // 1
			"  #  include   \"b/c.h\"  // trailing comment\n"   // 2
			"#include <vector>\n"                               // 3: angle brackets
			"// #include \"commented.h\"\n"                     // 4
			"/* #include \"block.h\"\n"                         // 5
			"#include \"still-block.h\" */\n"                   // 6
			"const char* text = \"/* not a comment\";\n"        // 7
			"#include \"after-string.h\"\n"                     // 8
			"const char* raw = R\"x(\n"                         // 9
			"#include \"in-raw-string.h\"\n"                    // 10
			")x\";\n"                                           // 11
			"char quote = '\"'; int big = 1'000'000;\n"         // 12
			"#include \"after-literals.h\"\n"                   // 13
			"#if 0\n"                                           // 14
			"#include \"disabled.h\"\n"                         // 15
			"#if defined(X)\n"                                  // 16
			"#include \"nested-disabled.h\"\n"                  // 17
			"#endif\n"                                          // 18
			"#elif defined(Y)\n"                                // 19
			"#include \"elif-branch.h\"\n"                      // 20
			"#else\n"                                           // 21
			"#include \"else-branch.h\"\n"                      // 22
			"#endif\n"                                          // 23
			"#ifdef ST_PLATFORM_WINDOWS\n"                      // 24
			"#include \"conditional.h\"\n"                      // 25
			"#endif\n"                                          // 26
			"\t#include <Strata/Engine/x.h> /* engine */\n"     // 27
			"// #include <commented-angled.h>\n"                // 28
			"#include <unterminated.h\n"                        // 29: not a directive the compiler accepts
			"#include <>\n";                                    // 30: neither

		const std::vector<IncludeDirective> includes = ParseIncludes(source);
		std::vector<std::tuple<std::string, uint32_t, bool>> found;
		for (const IncludeDirective& include : includes)
			found.emplace_back(include.Path, include.Line, include.Angled);
		CHECK(found == std::vector<std::tuple<std::string, uint32_t, bool>> {
			{ "a.h", 1, false }, { "b/c.h", 2, false }, { "vector", 3, true }, { "after-string.h", 8, false }, { "after-literals.h", 13, false },
			{ "elif-branch.h", 20, false }, { "else-branch.h", 22, false }, { "conditional.h", 25, false }, { "Strata/Engine/x.h", 27, true } });

		// Windows line endings and an #if 0 at the end of an unterminated file.
		CHECK(ParseIncludes("#include \"crlf.h\"\r\n#if 0\r\n#include \"x.h\"").size() == 1);
	}

	TEST_CASE("C, C++ and Objective-C sources and headers are read")
	{
		for (const char* path : { "Strata/Core/Log.h", "Strata/Core/Log.cpp", "a/b.hpp", "a/b.inl", "a/b.c", "Platform/MacOS/MacOSWindow.mm",
			"Platform/MacOS/Bridge.m", "Platform/Windows/Upper.CPP", "stpch.h" })
		{
			CAPTURE(path);
			CHECK(IsSourceFile(path));
		}
		for (const char* path : { "Strata/Core/Version.h.in", "Strata/Core/Notes.txt", "a/mm", "a/.h", "a.cpp/README", "" })
		{
			CAPTURE(path);
			CHECK_FALSE(IsSourceFile(path));
		}
	}

	TEST_CASE("Globs match within and across path components")
	{
		CHECK(MatchesGlob("Strata/Core/**", "Strata/Core/Application.cpp"));
		CHECK(MatchesGlob("Strata/Core/**", "Strata/Core/Sub/Deep.h"));
		CHECK_FALSE(MatchesGlob("Strata/Core/**", "Strata/CoreX/File.h"));
		CHECK(MatchesGlob("Strata/Core/Application.*", "Strata/Core/Application.h"));
		CHECK_FALSE(MatchesGlob("Strata/Core/Application.*", "Strata/Core/ApplicationEvents.h"));
		CHECK(MatchesGlob("Platform/*/*Socket.cpp", "Platform/Windows/WindowsSocket.cpp"));
		CHECK_FALSE(MatchesGlob("Platform/*/*Socket.cpp", "Platform/Windows/Deep/WindowsSocket.cpp"));
		CHECK(MatchesGlob("a/**/b.h", "a/b.h"));
		CHECK(MatchesGlob("a/**/b.h", "a/x/y/b.h"));
		CHECK(MatchesGlob("a/?.h", "a/x.h"));
		CHECK_FALSE(MatchesGlob("a?b", "a/b"));
		CHECK(MatchesGlob("stpch.h", "stpch.h"));
		CHECK_FALSE(MatchesGlob("stpch.h", "Strata/stpch.h"));
	}

	TEST_CASE("Layer tables list each layer once and allow only lower layers")
	{
		LayerTable table;
		std::vector<std::string> errors;
		const nlohmann::json valid = { { "MaxAllowlistEntries", 0 }, { "Layers", {
			{ { "Name", "Base" }, { "Paths", { "base/**" } } },
			{ { "Name", "Top" }, { "Paths", { "top/**" } }, { "Exclude", { "top/x.h" } }, { "MayInclude", { "Base" } } } } } };
		REQUIRE(ParseLayerTable(valid, table, errors));
		REQUIRE(table.Layers.size() == 2);
		CHECK(table.Layers[1].Exclude == std::vector<std::string> { "top/x.h" });

		const nlohmann::json upward = { { "MaxAllowlistEntries", 0 }, { "Layers", {
			{ { "Name", "Base" }, { "Paths", { "base/**" } }, { "MayInclude", { "Top" } } },
			{ { "Name", "Top" }, { "Paths", { "top/**" } } } } } };
		errors.clear();
		CHECK_FALSE(ParseLayerTable(upward, table, errors));
		CHECK(JoinLines(errors).find("Layer 'Base' may include 'Top', which is not a layer listed below it") != std::string::npos);

		const nlohmann::json duplicate = { { "MaxAllowlistEntries", 0 }, { "Layers", {
			{ { "Name", "Base" }, { "Paths", { "base/**" } } },
			{ { "Name", "Base" }, { "Paths", { "other/**" } } } } } };
		errors.clear();
		CHECK_FALSE(ParseLayerTable(duplicate, table, errors));
		CHECK(JoinLines(errors).find("Layer 'Base' is listed twice") != std::string::npos);

		const nlohmann::json incomplete = { { "Layers", { { { "Name", "Empty" } }, { { "Paths", { "x/**" } } } } } };
		errors.clear();
		CHECK_FALSE(ParseLayerTable(incomplete, table, errors));
		CHECK(errors.size() == 3); // No MaxAllowlistEntries, a layer without paths, a layer without a name
		errors.clear();
		CHECK_FALSE(ParseLayerTable(nlohmann::json::array(), table, errors));
	}

	TEST_CASE("Allowlist lines need a file, a header, a workstream and a reason")
	{
		std::vector<AllowlistEntry> entries;
		std::vector<std::string> errors;
		const std::string text =
			"# A comment\n"
			"\n"
			"top/a.cpp:base/b.h | wave-x | Needed until the split | which may contain bars\n";
		REQUIRE(ParseAllowlist(text, entries, errors));
		REQUIRE(entries.size() == 1);
		CHECK(entries[0].File == "top/a.cpp");
		CHECK(entries[0].Header == "base/b.h");
		CHECK(entries[0].Owner == "wave-x");
		CHECK(entries[0].Reason == "Needed until the split | which may contain bars");
		CHECK(entries[0].Line == 3);

		for (const char* malformed : { "top/a.cpp:base/b.h | wave-x", "top/a.cpp:base/b.h | | reason", "top/a.cpp | wave-x | reason", "top/a.cpp: | wave-x | reason" })
		{
			CAPTURE(malformed);
			entries.clear();
			errors.clear();
			CHECK_FALSE(ParseAllowlist(malformed, entries, errors));
			CHECK(entries.empty());
		}

		entries.clear();
		errors.clear();
		CHECK_FALSE(ParseAllowlist("a.cpp:b.h | w | r\na.cpp:b.h | w | again\n", entries, errors));
		CHECK(JoinLines(errors).find("line 2: a.cpp:b.h is listed twice") != std::string::npos);
	}

	TEST_CASE("Files map to exactly one layer and includes resolve like the preprocessor's")
	{
		LayerTable table = MakeTwoLayerTable();
		table.Layers[0].Exclude = { "base/excluded.h" };
		const std::map<std::string, std::string> files = {
			{ "base/a.h", "#include \"top/t.h\"\n#include <vector>\n#include \"StrataScript/ScriptABI.h\"\n" },
			{ "base/b.cpp", "#include \"a.h\"\n#include \"../top/t.h\"\n" },
			{ "top/t.h", "#include \"base/a.h\"\n#include \"base/Generated.h\"\n" },
			{ "base/excluded.h", "" },
			{ "orphan/o.h", "" }
		};
		const LayeringReport report = CheckLayering(table, files, {});
		CHECK(report.Errors.size() == 2); // base/excluded.h and orphan/o.h are in no layer
		CHECK(JoinLines(report.Errors).find("orphan/o.h is in no layer") != std::string::npos);
		// base/a.h -> top/t.h and base/b.cpp -> ../top/t.h are upward; "a.h" next to base/b.cpp, top -> base and the
		// generated base header are allowed; <vector> and the other target's header are not engine includes.
		REQUIRE(report.Violations.size() == 2);
		CHECK(Describe(report.Violations[0]) == "base/a.h(1): includes top/t.h, but layer Base may not include layer Top");
		CHECK(Describe(report.Violations[1]) == "base/b.cpp(2): includes top/t.h, but layer Base may not include layer Top");
		CHECK(report.CheckedIncludes == 5);

		// A file in two layers.
		table.Layers[1].Paths.push_back("base/a.h");
		CHECK(JoinLines(CheckLayering(table, files, {}).Errors).find("base/a.h is in several layers ('Base', 'Top')") != std::string::npos);
	}

	TEST_CASE("Angle-bracket includes are checked, resolved from the source root only")
	{
		const LayerTable table = MakeTwoLayerTable();
		const std::map<std::string, std::string> files = {
			{ "base/a.h", "#include <top/t.h>\n#include <vector>\n#include <glm/glm.hpp>\n" },
			{ "base/b.cpp", "#include <a.h>\n#include <base/a.h>\n#include \"a.h\"\n" },
			{ "top/t.h", "#include <base/Generated.h>\n" }
		};
		const LayeringReport report = CheckLayering(table, files, {});
		CHECK(report.Errors.empty());
		// base/a.h -> <top/t.h> is upward. <a.h> names no file at the root (only "a.h" finds base/a.h, next to base/b.cpp);
		// <vector> and <glm/glm.hpp> are no engine headers; <base/a.h>, "a.h" and the generated base header are allowed.
		REQUIRE(report.Violations.size() == 1);
		CHECK(Describe(report.Violations[0]) == "base/a.h(1): includes top/t.h, but layer Base may not include layer Top");
		CHECK(report.CheckedIncludes == 4);

		// An allowlist entry covers the include whatever its form.
		const LayeringReport allowlisted = CheckLayering(table, files, { { "base/a.h", "top/t.h", "wave-x", "until the split", 1 } });
		CHECK(allowlisted.Violations.empty());
		CHECK(allowlisted.Allowlisted.size() == 1);
		CHECK(allowlisted.StaleEntries.empty());
	}

	TEST_CASE("Allowlisted includes pass and unused allowlist lines are reported")
	{
		const LayerTable table = MakeTwoLayerTable();
		const std::map<std::string, std::string> files = {
			{ "base/a.h", "#include \"top/t.h\"\n" },
			{ "top/t.h", "" }
		};
		const std::vector<AllowlistEntry> allowlist = {
			{ "base/a.h", "top/t.h", "wave-x", "until the split", 1 },
			{ "base/a.h", "top/gone.h", "wave-x", "no longer included", 2 }
		};
		const LayeringReport report = CheckLayering(table, files, allowlist);
		CHECK(report.Violations.empty());
		REQUIRE(report.Allowlisted.size() == 1);
		CHECK(report.Allowlisted[0].Header == "top/t.h");
		REQUIRE(report.StaleEntries.size() == 1);
		CHECK(report.StaleEntries[0].Header == "top/gone.h");
	}
}
