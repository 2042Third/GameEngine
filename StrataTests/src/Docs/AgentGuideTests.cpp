#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"

#include <spdlog/fmt/fmt.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

// AGENTS.md holds the rules every change follows and is loaded into every agent session; Docs/DeveloperGuide.md holds
// the details, which agents read on demand. AGENTS.md stays small enough to load, and the links of both documents name
// files that exist and headings the linked document has (anchors as GitHub makes them).
namespace
{

	constexpr size_t c_MaxAgentsCodePoints = 40'000;
	constexpr const char* c_AgentsDocument = "AGENTS.md";
	constexpr const char* c_GuideDocument = "Docs/DeveloperGuide.md";

	struct Document
	{
		std::string Name; // Relative to the repository root, with '/' separators
		std::string Text;
	};

	std::filesystem::path GetRepositoryRoot()
	{
		return FileSystem::FromUTF8(STRATA_SOURCE_DIR);
	}

	std::string JoinLines(const std::vector<std::string>& lines)
	{
		std::string text;
		for (const std::string& line : lines)
			text += "\n  " + line;
		return text;
	}

	Document ReadDocument(const char* name)
	{
		const std::filesystem::path path = GetRepositoryRoot() / FileSystem::FromUTF8(name);
		const std::optional<std::string> text = FileSystem::ReadText(path);
		INFO(fmt::format("{} cannot be read from '{}'", name, FileSystem::ToUTF8(path)));
		REQUIRE(text.has_value());
		return { name, *text };
	}

	bool IsSpace(char c)
	{
		return c == ' ' || c == '\t' || c == '\r';
	}

	std::string_view Trim(std::string_view text)
	{
		while (!text.empty() && IsSpace(text.front()))
			text.remove_prefix(1);
		while (!text.empty() && IsSpace(text.back()))
			text.remove_suffix(1);
		return text;
	}

	std::string_view WithoutCarriageReturn(std::string_view line)
	{
		if (!line.empty() && line.back() == '\r')
			line.remove_suffix(1);
		return line;
	}

	// The lines of a text, without their line breaks ("\n" or "\r\n").
	std::vector<std::string_view> SplitLines(std::string_view text)
	{
		std::vector<std::string_view> lines;
		size_t start = 0;
		size_t end = text.find('\n');
		while (end != std::string_view::npos)
		{
			lines.push_back(WithoutCarriageReturn(text.substr(start, end - start)));
			start = end + 1;
			end = text.find('\n', start);
		}
		lines.push_back(WithoutCarriageReturn(text.substr(start)));
		return lines;
	}

	// Characters as an editor counts them: UTF-8 code points, line breaks counted as one ("\r\n" too).
	size_t CountCodePoints(std::string_view text)
	{
		size_t count = 0;
		for (const char c : text)
		{
			// Every code point has exactly one byte that is not a continuation byte (10xxxxxx).
			if (c != '\r' && (static_cast<uint8_t>(c) & 0xC0) != 0x80)
				++count;
		}
		return count;
	}

	// Decodes the code point that starts at text[index] and moves index past it. A byte that starts no valid sequence is
	// a code point of its own (its value), so that no input stops a scan.
	uint32_t NextCodePoint(std::string_view text, size_t& index)
	{
		const uint8_t lead = static_cast<uint8_t>(text[index]);
		size_t length = 1;
		uint32_t codePoint = lead;
		if (lead >= 0xC2 && lead <= 0xDF)
		{
			length = 2;
			codePoint = lead & 0x1Fu;
		}
		else if (lead >= 0xE0 && lead <= 0xEF)
		{
			length = 3;
			codePoint = lead & 0x0Fu;
		}
		else if (lead >= 0xF0 && lead <= 0xF4)
		{
			length = 4;
			codePoint = lead & 0x07u;
		}

		bool valid = index + length <= text.size();
		for (size_t i = 1; valid && i < length; ++i)
		{
			const uint8_t next = static_cast<uint8_t>(text[index + i]);
			valid = (next & 0xC0) == 0x80;
			codePoint = (codePoint << 6) | (next & 0x3Fu);
		}
		if (!valid)
		{
			index += 1;
			return lead;
		}
		index += length;
		return codePoint;
	}

	// Whether GitHub leaves the code point out of anchors: ASCII other than letters, digits, '-', '_' and spaces, and
	// the punctuation and symbol blocks beyond ASCII (Latin-1 punctuation, general punctuation to miscellaneous symbols
	// and arrows, CJK punctuation). Letters of other scripts stay.
	bool IsLeftOutOfAnchors(uint32_t codePoint)
	{
		if (codePoint < 0x80)
		{
			const char c = static_cast<char>(codePoint);
			const bool letterOrDigit = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
			return !(letterOrDigit || c == '-' || c == '_' || c == ' ');
		}
		return (codePoint >= 0xA0 && codePoint <= 0xBF) || codePoint == 0xD7 || codePoint == 0xF7 ||
			(codePoint >= 0x2000 && codePoint <= 0x2BFF) || (codePoint >= 0x3000 && codePoint <= 0x303F);
	}

	// A heading's anchor as GitHub makes it: punctuation left out, ASCII letters lowercased, spaces made '-'.
	std::string MakeAnchor(std::string_view heading)
	{
		std::string anchor;
		size_t index = 0;
		while (index < heading.size())
		{
			const size_t start = index;
			const uint32_t codePoint = NextCodePoint(heading, index);
			if (IsLeftOutOfAnchors(codePoint))
				continue;
			if (codePoint >= 0x80)
			{
				anchor.append(heading.substr(start, index - start));
				continue;
			}
			const char c = static_cast<char>(codePoint);
			if (c == ' ')
				anchor += '-';
			else if (c >= 'A' && c <= 'Z')
				anchor += static_cast<char>(c - 'A' + 'a');
			else
				anchor += c;
		}
		return anchor;
	}

	// A line that opens or closes a fenced code block (up to three spaces of indentation).
	bool IsFence(std::string_view line)
	{
		size_t indent = 0;
		while (indent < line.size() && line[indent] == ' ')
			++indent;
		if (indent > 3)
			return false;
		const std::string_view rest = line.substr(indent);
		return rest.starts_with("```") || rest.starts_with("~~~");
	}

	// The anchors of a document's headings (ATX headings outside fenced code). Repeated anchors get the suffixes -1, -2
	// and so on, as on GitHub.
	std::set<std::string> GetHeadingAnchors(std::string_view text)
	{
		std::set<std::string> anchors;
		std::map<std::string, int> counts;
		bool inFence = false;
		for (const std::string_view line : SplitLines(text))
		{
			if (IsFence(line))
			{
				inFence = !inFence;
				continue;
			}
			if (inFence)
				continue;

			size_t level = 0;
			while (level < line.size() && line[level] == '#')
				++level;
			if (level == 0 || level > 6 || level >= line.size() || line[level] != ' ')
				continue;

			// An optional closing sequence of '#' after a space is not part of the heading.
			std::string_view heading = Trim(line.substr(level + 1));
			size_t end = heading.size();
			while (end > 0 && heading[end - 1] == '#')
				--end;
			if (end < heading.size() && (end == 0 || heading[end - 1] == ' '))
				heading = Trim(heading.substr(0, end));

			const std::string base = MakeAnchor(heading);
			const int count = counts[base]++;
			anchors.insert(count == 0 ? base : fmt::format("{}-{}", base, count));
		}
		return anchors;
	}

	size_t CountRun(std::string_view text, size_t start, char c)
	{
		size_t end = start;
		while (end < text.size() && text[end] == c)
			++end;
		return end - start;
	}

	// Where the next run of exactly `length` backticks at or after `from` starts, or npos.
	size_t FindClosingRun(std::string_view text, size_t from, size_t length)
	{
		size_t index = from;
		while (index < text.size())
		{
			if (text[index] != '`')
			{
				++index;
				continue;
			}
			const size_t run = CountRun(text, index, '`');
			if (run == length)
				return index;
			index += run;
		}
		return std::string_view::npos;
	}

	// The text outside fenced code blocks and code spans, paragraph by paragraph: a code span ends within its paragraph,
	// and a backtick run without a closing one is text. Code spans become the word "code", so nothing in them is a link.
	std::string RemoveCode(std::string_view text)
	{
		std::vector<std::string> paragraphs(1);
		bool inFence = false;
		for (const std::string_view line : SplitLines(text))
		{
			if (IsFence(line))
			{
				inFence = !inFence;
				continue;
			}
			if (inFence)
				continue;
			if (Trim(line).empty())
			{
				paragraphs.emplace_back();
				continue;
			}
			if (!paragraphs.back().empty())
				paragraphs.back() += '\n';
			paragraphs.back() += line;
		}

		std::string prose;
		for (const std::string& paragraph : paragraphs)
		{
			size_t index = 0;
			while (index < paragraph.size())
			{
				if (paragraph[index] != '`')
				{
					prose += paragraph[index];
					++index;
					continue;
				}
				const size_t run = CountRun(paragraph, index, '`');
				const size_t close = FindClosingRun(paragraph, index + run, run);
				if (close == std::string_view::npos)
				{
					prose.append(paragraph, index, run);
					index += run;
				}
				else
				{
					prose += "code";
					index = close + run;
				}
			}
			prose += "\n\n";
		}
		return prose;
	}

	// The targets of a document's inline links and images: what follows "](" up to ')'. Targets with spaces or a title
	// are skipped (Strata's documents use none).
	std::vector<std::string> GetLinkTargets(std::string_view text)
	{
		const std::string prose = RemoveCode(text);
		std::vector<std::string> targets;
		size_t found = prose.find("](");
		while (found != std::string::npos)
		{
			const size_t start = found + 2;
			size_t end = start;
			while (end < prose.size() && prose[end] != ')' && prose[end] != ' ' && prose[end] != '\t' && prose[end] != '\n')
				++end;
			if (end < prose.size() && prose[end] == ')')
				targets.push_back(prose.substr(start, end - start));
			found = prose.find("](", start);
		}
		return targets;
	}

	struct LinkReport
	{
		std::vector<std::string> Errors;
		size_t CheckedLinks = 0;
		size_t LinksFromAgentsToGuide = 0;
	};

	// Checks the links of the documents to repository files (web links are not followed): each names a file or
	// directory inside the repository that exists, and an anchor of a checked document names one of its headings.
	LinkReport CheckLinks(const std::filesystem::path& root, const std::vector<Document>& documents)
	{
		std::map<std::string, std::set<std::string>> anchors;
		for (const Document& document : documents)
			anchors[document.Name] = GetHeadingAnchors(document.Text);

		LinkReport report;
		for (const Document& document : documents)
		{
			const std::filesystem::path directory = (root / FileSystem::FromUTF8(document.Name)).parent_path();
			for (const std::string& target : GetLinkTargets(document.Text))
			{
				if (target.find("://") != std::string::npos || target.starts_with("mailto:"))
					continue;
				++report.CheckedLinks;

				const size_t hash = target.find('#');
				const std::string path = target.substr(0, hash);
				const std::string fragment = hash == std::string::npos ? std::string() : target.substr(hash + 1);

				std::string linked = document.Name;
				if (!path.empty())
				{
					const std::filesystem::path resolved = path.starts_with('/')
						? (root / FileSystem::FromUTF8(path.substr(1))).lexically_normal()
						: (directory / FileSystem::FromUTF8(path)).lexically_normal();
					if (!FileSystem::IsInside(resolved, root))
					{
						report.Errors.push_back(fmt::format("{}: the link '{}' leaves the repository", document.Name, target));
						continue;
					}
					if (!FileSystem::Exists(resolved))
					{
						report.Errors.push_back(fmt::format("{}: the link '{}' names a file that does not exist", document.Name, target));
						continue;
					}
					linked = FileSystem::ToUTF8(FileSystem::GetRelativePath(resolved, root));
				}

				if (document.Name == c_AgentsDocument && linked == c_GuideDocument)
					++report.LinksFromAgentsToGuide;
				const auto linkedAnchors = anchors.find(linked);
				if (!fragment.empty() && linkedAnchors != anchors.end() && !linkedAnchors->second.contains(fragment))
					report.Errors.push_back(fmt::format("{}: the link '{}' names no heading of {}", document.Name, target, linked));
			}
		}
		return report;
	}

}

TEST_SUITE("Docs.AgentGuide")
{
	TEST_CASE("AGENTS.md stays small enough to load into every session")
	{
		const Document agents = ReadDocument(c_AgentsDocument);
		const size_t codePoints = CountCodePoints(agents.Text);
		INFO(fmt::format("AGENTS.md has {} characters, more than {}: agents load it into every session. Move details into "
						 "Docs/DeveloperGuide.md and keep only the rules, with a link to their details, in AGENTS.md.",
			codePoints, c_MaxAgentsCodePoints));
		CHECK(codePoints <= c_MaxAgentsCodePoints);
	}

	TEST_CASE("The links of AGENTS.md and the developer guide name existing files and headings")
	{
		const std::vector<Document> documents = { ReadDocument(c_AgentsDocument), ReadDocument(c_GuideDocument) };
		const LinkReport report = CheckLinks(GetRepositoryRoot(), documents);

		INFO("Broken links (fix the target, or the heading it names):", JoinLines(report.Errors));
		CHECK(report.Errors.empty());
		// AGENTS.md leads into the guide, and a scan that found no links would pass for nothing.
		CHECK(report.LinksFromAgentsToGuide >= 10);
		CHECK(report.CheckedLinks >= 50);
	}

	TEST_CASE("Characters are counted as code points")
	{
		CHECK(CountCodePoints("") == 0);
		CHECK(CountCodePoints("abc") == 3);
		CHECK(CountCodePoints("a\xC3\xA9\xE2\x80\x94\xF0\x9F\x98\x80") == 4); // a, e acute, em dash, an emoji
		CHECK(CountCodePoints("a\r\nb\n") == 4);
	}

	TEST_CASE("Anchors are made as GitHub makes them")
	{
		CHECK(MakeAnchor("Automation (editor RPC + MCP)") == "automation-editor-rpc--mcp");
		CHECK(MakeAnchor("Code style (Hazel conventions)") == "code-style-hazel-conventions");
		CHECK(MakeAnchor("The editor's script engine") == "the-editors-script-engine");
		CHECK(MakeAnchor("Hierarchy, Content Browser and thumbnails") == "hierarchy-content-browser-and-thumbnails");
		CHECK(MakeAnchor("snake_case and kebab-case") == "snake_case-and-kebab-case");
		CHECK(MakeAnchor("Strata Engine \xE2\x80\x94 Agent & Developer Guide") == "strata-engine--agent--developer-guide");
		CHECK(MakeAnchor("Caf\xC3\xA9") == "caf\xC3\xA9");

		const std::set<std::string> anchors = GetHeadingAnchors(
			"# Title\n"
			"\n"
			"## Notes\n"
			"\n"
			"```sh\n"
			"## Not a heading\n"
			"```\n"
			"\n"
			"### Notes\n"
			"## Closed ##\n"
			"#No space\n"
			"####### Seven\n");
		CHECK(anchors.size() == 4);
		CHECK(anchors.contains("title"));
		CHECK(anchors.contains("notes"));
		CHECK(anchors.contains("notes-1"));
		CHECK(anchors.contains("closed"));
	}

	TEST_CASE("Links are found outside code only")
	{
		const std::vector<std::string> targets = GetLinkTargets(
			"See [the guide](Docs/DeveloperGuide.md#testing), not `[a span](span.md)`.\n"
			"\n"
			"```\n"
			"[a fence](fence.md)\n"
			"```\n"
			"\n"
			"A span ``with ` inside [x](double.md)`` ends; [an image](Brand/mark.png), [titled](file.md \"title\").\n"
			"\n"
			"An unmatched ` backtick and [z](z.md).\n");
		REQUIRE(targets.size() == 3);
		CHECK(targets[0] == "Docs/DeveloperGuide.md#testing");
		CHECK(targets[1] == "Brand/mark.png");
		CHECK(targets[2] == "z.md");
	}

	TEST_CASE("Broken links are reported")
	{
		const std::filesystem::path root = GetRepositoryRoot();
		const std::vector<Document> documents = {
			{ c_AgentsDocument, "# Rules\n\n## Testing\n\n[ok](#testing) [guide](Docs/DeveloperGuide.md#guide-section) "
								"[gone](#nothing) [missing](Docs/NoSuchFile.md) [out](../outside.md) [web](https://example.com/x)\n" },
			{ c_GuideDocument, "# Guide\n\n## Guide section\n\n[back](../AGENTS.md#testing) [wrong](../AGENTS.md#guide-section)\n" },
		};
		const LinkReport report = CheckLinks(root, documents);
		INFO("Errors:", JoinLines(report.Errors));
		CHECK(report.CheckedLinks == 7);
		CHECK(report.LinksFromAgentsToGuide == 1);
		REQUIRE(report.Errors.size() == 4);
		CHECK(report.Errors[0] == "AGENTS.md: the link '#nothing' names no heading of AGENTS.md");
		CHECK(report.Errors[1] == "AGENTS.md: the link 'Docs/NoSuchFile.md' names a file that does not exist");
		CHECK(report.Errors[2] == "AGENTS.md: the link '../outside.md' leaves the repository");
		CHECK(report.Errors[3] == "Docs/DeveloperGuide.md: the link '../AGENTS.md#guide-section' names no heading of AGENTS.md");
	}
}
