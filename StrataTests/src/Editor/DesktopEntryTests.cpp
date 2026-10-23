#include <doctest/doctest.h>

#include "Editor/DesktopEntry.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>

#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	std::string_view TrimSpaces(std::string_view text)
	{
		while (!text.empty() && text.front() == ' ')
			text.remove_prefix(1);
		while (!text.empty() && text.back() == ' ')
			text.remove_suffix(1);
		return text;
	}

	// A value of the specification's string type with its escapes undone; nothing for an escape it does not define.
	std::optional<std::string> UnescapeString(std::string_view raw)
	{
		std::string value;
		for (size_t index = 0; index < raw.size(); index++)
		{
			if (raw[index] != '\\')
			{
				value += raw[index];
				continue;
			}
			if (++index == raw.size())
				return std::nullopt;
			switch (raw[index])
			{
				case 's': value += ' '; break;
				case 'n': value += '\n'; break;
				case 't': value += '\t'; break;
				case 'r': value += '\r'; break;
				case '\\': value += '\\'; break;
				default: return std::nullopt;
			}
		}
		return value;
	}

	// Reads a desktop entry as the specification has readers do: the [Desktop Entry] group's Key=Value lines, the spaces
	// around '=' and at the line's end trimmed, each value unescaped as a string. Nothing for a malformed line or a key
	// given twice.
	std::optional<std::map<std::string, std::string>> ParseEntry(const std::string& text)
	{
		std::map<std::string, std::string> values;
		std::istringstream lines(text);
		std::string line;
		bool inGroup = false;
		while (std::getline(lines, line))
		{
			if (line.empty() || line.front() == '#')
				continue;
			if (line.front() == '[')
			{
				inGroup = line == "[Desktop Entry]";
				continue;
			}
			const size_t equals = line.find('=');
			if (!inGroup || equals == std::string::npos)
				return std::nullopt;
			const std::string key(TrimSpaces(std::string_view(line).substr(0, equals)));
			const std::optional<std::string> value = UnescapeString(TrimSpaces(std::string_view(line).substr(equals + 1)));
			if (!value || !values.emplace(key, *value).second)
				return std::nullopt;
		}
		return values;
	}

	// Exec's arguments (its value already unescaped as a string) with their quotes undone and %% read as %; nothing for an
	// escape the quoting rules do not define, a field code, or quotes left open.
	std::optional<std::vector<std::string>> SplitExec(std::string_view exec)
	{
		std::vector<std::string> arguments;
		std::string argument;
		bool inArgument = false;
		bool quoted = false;
		for (size_t index = 0; index < exec.size(); index++)
		{
			const char character = exec[index];
			if (character == '%')
			{
				if (index + 1 == exec.size() || exec[index + 1] != '%')
					return std::nullopt;
				argument += '%';
				inArgument = true;
				index++;
			}
			else if (quoted && character == '\\')
			{
				if (index + 1 == exec.size() || std::string_view("\"`$\\").find(exec[index + 1]) == std::string_view::npos)
					return std::nullopt;
				argument += exec[++index];
			}
			else if (character == '"')
			{
				quoted = !quoted;
				inArgument = true;
			}
			else if (character == ' ' && !quoted)
			{
				if (inArgument)
					arguments.push_back(std::move(argument));
				argument.clear();
				inArgument = false;
			}
			else
			{
				argument += character;
				inArgument = true;
			}
		}
		if (quoted)
			return std::nullopt;
		if (inArgument)
			arguments.push_back(std::move(argument));
		return arguments;
	}

}

TEST_SUITE("Editor.Export.DesktopEntry")
{
	TEST_CASE("The desktop entry parses back with escaped values")
	{
		// Every character the rules escape, in the paths and the name: quotes, backticks, dollars, backslashes, percents,
		// spaces at either end, a tab and a newline, and text beyond ASCII.
		const std::filesystem::path directory = CreateTemporaryDirectory("DesktopEntry") / FileSystem::FromUTF8(" My \"Games\" `$HOME` 100% back\\slash\ttab\nline Caf\xC3\xA9 ");
		DesktopEntry entry;
		entry.Name = "  Space \"Race\" \\ 100% \xE2\x98\x95 ";
		entry.Executable = directory / "Space Race";
		entry.Icon = directory / "Space Race.png";
		entry.WindowClass = "Space Race";
		std::string error;
		const std::optional<std::string> text = entry.ToText(&error);
		REQUIRE_MESSAGE(text, error);
		CHECK(text->starts_with("[Desktop Entry]\n"));
		CHECK(text->ends_with("\n"));

		const std::optional<std::map<std::string, std::string>> values = ParseEntry(*text);
		REQUIRE(values);
		CHECK(values->size() == 7);
		CHECK(values->at("Type") == "Application");
		CHECK(values->at("Name") == entry.Name);
		CHECK(values->at("Icon") == FileSystem::ToUTF8(entry.Icon));
		CHECK(values->at("StartupWMClass") == "Space Race");
		CHECK(values->at("Terminal") == "false");
		CHECK(values->at("Categories") == "Game;");
		const std::optional<std::vector<std::string>> exec = SplitExec(values->at("Exec"));
		REQUIRE(exec);
		CHECK(*exec == std::vector<std::string> { FileSystem::ToUTF8(entry.Executable) });

		// Without an icon there is no Icon key.
		entry.Icon.clear();
		const std::optional<std::string> withoutIcon = entry.ToText(&error);
		REQUIRE_MESSAGE(withoutIcon, error);
		const std::optional<std::map<std::string, std::string>> plain = ParseEntry(*withoutIcon);
		REQUIRE(plain);
		CHECK_FALSE(plain->contains("Icon"));
		CHECK(SplitExec(plain->at("Exec")) == std::vector<std::string> { FileSystem::ToUTF8(entry.Executable) });
	}

	TEST_CASE("A desktop entry refuses empty values, text that is not UTF-8 and relative paths")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("DesktopEntryRefusals");
		DesktopEntry entry;
		entry.Name = "Game";
		entry.Executable = directory / "Game";
		entry.WindowClass = "Game";
		REQUIRE(entry.ToText());

		std::string error;
		DesktopEntry refused = entry;
		refused.Name.clear();
		CHECK_FALSE(refused.ToText(&error));
		CHECK(error == "its name is empty");
		refused = entry;
		refused.Name = "Bad \xFF name";
		CHECK_FALSE(refused.ToText(&error));
		CHECK(error == "its name is not valid UTF-8");
		refused = entry;
		refused.WindowClass.clear();
		CHECK_FALSE(refused.ToText(&error));
		CHECK(error == "its window class is empty");
		refused = entry;
		refused.Executable = "Game";
		CHECK_FALSE(refused.ToText(&error));
		CHECK(error == "its executable 'Game' is not an absolute path");
		refused = entry;
		refused.Icon = "Game.png";
		CHECK_FALSE(refused.ToText(&error));
		CHECK(error == "its icon 'Game.png' is not an absolute path");
	}
}
