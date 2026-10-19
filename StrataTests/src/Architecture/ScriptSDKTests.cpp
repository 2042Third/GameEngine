#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

namespace
{

	// The header an include line names when it is one of the script SDK's C++ headers (StrataScript/<Name>.h other than
	// the C ABI, ScriptABI.h).
	std::optional<std::string> FindSDKInclude(std::string_view line)
	{
		const size_t start = line.find_first_not_of(" \t");
		if (start == std::string_view::npos || line[start] != '#')
			return std::nullopt;
		const size_t directive = line.find_first_not_of(" \t", start + 1);
		if (directive == std::string_view::npos || line.substr(directive, 7) != "include")
			return std::nullopt;
		const size_t open = line.find_first_of("\"<", directive + 7);
		if (open == std::string_view::npos)
			return std::nullopt;
		const size_t close = line.find_first_of("\">", open + 1);
		if (close == std::string_view::npos)
			return std::nullopt;
		const std::string_view path = line.substr(open + 1, close - open - 1);
		if (!path.starts_with("StrataScript/") || path == "StrataScript/ScriptABI.h")
			return std::nullopt;
		return std::string(path);
	}

	// Follows raw string literals (R"delimiter( ... )delimiter") across lines: the code they hold (the editor's script
	// templates, tests' sources) is text, not includes of this translation unit. Returns whether the line starts inside one.
	class RawStringTracker
	{
	public:
		bool StartsInside() const { return !m_Closing.empty(); }

		void Advance(std::string_view line)
		{
			size_t position = 0;
			while (position < line.size())
			{
				if (!m_Closing.empty())
				{
					const size_t close = line.find(m_Closing, position);
					if (close == std::string_view::npos)
						return;
					position = close + m_Closing.size();
					m_Closing.clear();
					continue;
				}
				const size_t open = line.find("R\"", position);
				if (open == std::string_view::npos)
					return;
				position = open + 2;
				// A raw string's R starts a token (or follows an encoding prefix: u8R, LR, uR, UR), unlike the R that ends a
				// word in "ERROR"; its delimiter is at most 16 characters without spaces, parentheses, backslashes or quotes.
				size_t prefix = open;
				while (prefix > 0 && (line[prefix - 1] == 'u' || line[prefix - 1] == 'U' || line[prefix - 1] == 'L' || line[prefix - 1] == '8'))
					prefix--;
				if (prefix > 0 && (std::isalnum(static_cast<unsigned char>(line[prefix - 1])) || line[prefix - 1] == '_'))
					continue;
				const size_t parenthesis = line.find('(', open + 2);
				if (parenthesis == std::string_view::npos || parenthesis - open - 2 > 16)
					continue;
				const std::string_view delimiter = line.substr(open + 2, parenthesis - open - 2);
				if (delimiter.find_first_of(" )\\\"\t") != std::string_view::npos)
					continue;
				m_Closing = ")" + std::string(delimiter) + "\"";
				position = parenthesis + 1;
			}
		}
	private:
		std::string m_Closing; // What ends the raw string the scan is in (empty: none)
	};

}

TEST_SUITE("Architecture.ScriptSDK")
{
	TEST_CASE("Include lines that name the SDK's C++ headers are found, and the C ABI is not one of them")
	{
		CHECK(FindSDKInclude("#include \"StrataScript/StrataScript.h\"") == std::optional<std::string>("StrataScript/StrataScript.h"));
		CHECK(FindSDKInclude("  #  include <StrataScript/UI.h>") == std::optional<std::string>("StrataScript/UI.h"));
		CHECK_FALSE(FindSDKInclude("#include \"StrataScript/ScriptABI.h\""));
		CHECK_FALSE(FindSDKInclude("#include \"Strata/UI/Widgets.h\""));
		CHECK_FALSE(FindSDKInclude("// #include \"StrataScript/UI.h\""));
		CHECK_FALSE(FindSDKInclude("const char* code = \"#include \\\"StrataScript/StrataScript.h\\\"\";"));

		// Lines inside a raw string are text.
		RawStringTracker tracker;
		tracker.Advance("return R\"CPP(// a template");
		CHECK(tracker.StartsInside());
		tracker.Advance("#include \"StrataScript/StrataScript.h\"");
		CHECK(tracker.StartsInside());
		tracker.Advance("int x;)CPP\"; const char* other = R\"(one line)\";");
		CHECK_FALSE(tracker.StartsInside());
		tracker.Advance("ST_CORE_ERROR(\"AN ERROR\", Format(x));");
		CHECK_FALSE(tracker.StartsInside());
		tracker.Advance("auto text = u8R\"x(starts");
		CHECK(tracker.StartsInside());
	}

	// The SDK's C++ headers are for script modules only. The engine, the editor, the tools and the tests see the C ABI
	// (ScriptABI.h): the SDK has classes named like engine ones (Random, Timer, Entity, and UI, which is a namespace of the
	// editor), so including it there would collide or, worse, silently pick the wrong one.
	TEST_CASE("No engine, editor, tool or test translation unit includes the SDK's C++ headers")
	{
		const std::filesystem::path root = FileSystem::FromUTF8(STRATA_SOURCE_DIR);
		std::vector<std::string> offenders;
		size_t scanned = 0;
		for (const char* directory : { "Strata/src", "StrataEditor/src", "StrataRuntime/src", "StrataCLI/src", "StrataTests/src" })
		{
			const std::filesystem::path base = root / directory;
			REQUIRE(std::filesystem::is_directory(base));
			for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(base))
			{
				const std::filesystem::path extension = entry.path().extension();
				if (!entry.is_regular_file() || (extension != ".h" && extension != ".cpp" && extension != ".mm"))
					continue;
				scanned++;
				const std::optional<std::string> text = FileSystem::ReadText(entry.path());
				REQUIRE(text.has_value());
				std::string_view remaining = *text;
				size_t lineNumber = 0;
				RawStringTracker rawStrings;
				while (!remaining.empty())
				{
					const size_t end = remaining.find('\n');
					const std::string_view line = remaining.substr(0, end);
					lineNumber++;
					if (!rawStrings.StartsInside())
					{
						if (const std::optional<std::string> header = FindSDKInclude(line))
							offenders.push_back(FileSystem::ToUTF8(std::filesystem::relative(entry.path(), root)) + ":" + std::to_string(lineNumber) + " includes " + *header);
					}
					rawStrings.Advance(line);
					remaining = end == std::string_view::npos ? std::string_view() : remaining.substr(end + 1);
				}
			}
		}
		CHECK(scanned > 100);
		INFO("Use StrataScript/ScriptABI.h outside script modules: ", [&offenders]()
		{
			std::string list;
			for (const std::string& offender : offenders)
				list += "\n  " + offender;
			return list;
		}());
		CHECK(offenders.empty());
	}
}
