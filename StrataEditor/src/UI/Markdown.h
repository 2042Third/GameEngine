#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata::UI
{

	enum class MarkdownBlockType : uint8_t
	{
		Heading = 0,
		Paragraph,
		ListItem,
		Table
	};

	// A block of a Markdown document as the editor shows it (e.g. ThirdPartyNotices.md in About Strata): headings,
	// paragraphs, list items and tables, with the inline markup taken out (StripInlineMarkdown).
	struct MarkdownBlock
	{
		MarkdownBlockType Type = MarkdownBlockType::Paragraph;
		int Level = 0;                              // Headings: 1 for "#", 2 for "##", ...
		std::string Text;                           // Headings, paragraphs and list items
		std::vector<std::vector<std::string>> Rows; // Tables: the header row first, then the body rows (no delimiter row)
	};

	// Splits Markdown into blocks: "#" headings, paragraphs (consecutive lines joined with spaces), "-", "*" or "+" list
	// items (with their continuation lines) and pipe tables. Anything else is paragraph text, so no input fails.
	std::vector<MarkdownBlock> ParseMarkdown(std::string_view text);
	// Inline markup out of a line: [text](link) and <link> keep their text, `code` and **strong** lose their marks, "\|"
	// becomes "|".
	std::string StripInlineMarkdown(std::string_view text);

	// Draws the blocks at the width available: headings in the SemiBold font, paragraphs and list items wrapped, tables as
	// ImGui tables with a header row. `id` scopes the tables' ids.
	void DrawMarkdown(const char* id, std::span<const MarkdownBlock> blocks);

}
