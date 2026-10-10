#include "UI/Markdown.h"

#include "UI/Widgets.h"

#include <imgui.h>

#include <algorithm>

namespace Strata::UI
{

	namespace
	{

		// ImGui tables have a column limit; documents with more columns are cut there.
		constexpr size_t c_MaxTableColumns = 64;

		std::string_view Trim(std::string_view text)
		{
			const size_t first = text.find_first_not_of(" \t");
			if (first == std::string_view::npos)
				return {};
			const size_t last = text.find_last_not_of(" \t");
			return text.substr(first, last - first + 1);
		}

		bool StartsWith(std::string_view text, std::string_view prefix)
		{
			return text.substr(0, prefix.size()) == prefix;
		}

		// "## Title" -> 2 and "Title"; 0 when the line is no heading ("#" must be followed by a space or end the line).
		int ParseHeading(std::string_view line, std::string_view& outText)
		{
			size_t level = 0;
			while (level < line.size() && line[level] == '#')
				level++;
			if (level == 0 || level > 6 || (level < line.size() && line[level] != ' '))
				return 0;
			outText = Trim(line.substr(level));
			return static_cast<int>(level);
		}

		// "- item", "* item" or "+ item" -> "item".
		bool ParseListItem(std::string_view line, std::string_view& outText)
		{
			if (line.size() < 2 || (line[0] != '-' && line[0] != '*' && line[0] != '+') || line[1] != ' ')
				return false;
			outText = Trim(line.substr(2));
			return true;
		}

		// The cells of a pipe table row ("| a | b |"), split at pipes that are not escaped ("\|").
		std::vector<std::string> SplitRow(std::string_view line)
		{
			if (!line.empty() && line.front() == '|')
				line.remove_prefix(1);
			if (!line.empty() && line.back() == '|' && (line.size() < 2 || line[line.size() - 2] != '\\'))
				line.remove_suffix(1);
			std::vector<std::string> cells;
			size_t start = 0;
			for (size_t index = 0; index <= line.size(); index++)
			{
				if (index == line.size() || (line[index] == '|' && (index == 0 || line[index - 1] != '\\')))
				{
					cells.push_back(std::string(Trim(line.substr(start, index - start))));
					start = index + 1;
				}
			}
			return cells;
		}

		// The row under a table's header: dashes, colons and spaces only ("| --- | :-: |").
		bool IsDelimiterRow(const std::vector<std::string>& cells)
		{
			return std::all_of(cells.begin(), cells.end(), [](const std::string& cell)
			{
				return !cell.empty() && cell.find('-') != std::string::npos && cell.find_first_not_of("-: ") == std::string::npos;
			});
		}

	}

	std::string StripInlineMarkdown(std::string_view text)
	{
		std::string result;
		result.reserve(text.size());
		size_t index = 0;
		while (index < text.size())
		{
			const char character = text[index];
			if (character == '\\' && index + 1 < text.size() && std::string_view("\\`*_[]()<>#+-.!|").find(text[index + 1]) != std::string_view::npos)
			{
				result += text[index + 1];
				index += 2;
				continue;
			}
			if (character == '`')
			{
				if (const size_t close = text.find('`', index + 1); close != std::string_view::npos)
				{
					result.append(text.substr(index + 1, close - index - 1));
					index = close + 1;
					continue;
				}
			}
			if (character == '*' && index + 1 < text.size() && text[index + 1] == '*')
			{
				index += 2;
				continue;
			}
			if (character == '[')
			{
				// [text](link), where the text may hold brackets itself.
				size_t depth = 0;
				size_t close = std::string_view::npos;
				for (size_t position = index; position < text.size(); position++)
				{
					if (text[position] == '[')
						depth++;
					else if (text[position] == ']' && --depth == 0)
					{
						close = position;
						break;
					}
				}
				const size_t end = close != std::string_view::npos && close + 1 < text.size() && text[close + 1] == '(' ? text.find(')', close + 2)
					: std::string_view::npos;
				if (end != std::string_view::npos)
				{
					result += StripInlineMarkdown(text.substr(index + 1, close - index - 1));
					index = end + 1;
					continue;
				}
			}
			if (character == '<')
			{
				const size_t close = text.find('>', index + 1);
				const std::string_view inner = close != std::string_view::npos ? text.substr(index + 1, close - index - 1) : std::string_view();
				if (StartsWith(inner, "http://") || StartsWith(inner, "https://") || StartsWith(inner, "mailto:"))
				{
					result.append(inner);
					index = close + 1;
					continue;
				}
			}
			result += character;
			index++;
		}
		return result;
	}

	std::vector<MarkdownBlock> ParseMarkdown(std::string_view text)
	{
		std::vector<MarkdownBlock> blocks;
		// The block later lines may continue: a paragraph, a list item or a table.
		bool continuable = false;
		size_t lineStart = 0;
		while (lineStart <= text.size())
		{
			size_t lineEnd = text.find('\n', lineStart);
			if (lineEnd == std::string_view::npos)
				lineEnd = text.size();
			std::string_view line = text.substr(lineStart, lineEnd - lineStart);
			lineStart = lineEnd + 1;
			if (!line.empty() && line.back() == '\r')
				line.remove_suffix(1);
			line = Trim(line);

			std::string_view content;
			if (line.empty())
			{
				continuable = false;
			}
			else if (const int level = ParseHeading(line, content); level > 0)
			{
				blocks.push_back({ MarkdownBlockType::Heading, level, StripInlineMarkdown(content), {} });
				continuable = false;
			}
			else if (line.front() == '|')
			{
				if (!continuable || blocks.back().Type != MarkdownBlockType::Table)
					blocks.push_back({ MarkdownBlockType::Table, 0, {}, {} });
				std::vector<std::string> cells = SplitRow(line);
				if (!IsDelimiterRow(cells))
				{
					for (std::string& cell : cells)
						cell = StripInlineMarkdown(cell);
					if (cells.size() > c_MaxTableColumns)
						cells.resize(c_MaxTableColumns);
					blocks.back().Rows.push_back(std::move(cells));
				}
				continuable = true;
			}
			else if (ParseListItem(line, content))
			{
				blocks.push_back({ MarkdownBlockType::ListItem, 0, StripInlineMarkdown(content), {} });
				continuable = true;
			}
			else if (continuable && blocks.back().Type != MarkdownBlockType::Table)
			{
				blocks.back().Text += " " + StripInlineMarkdown(line);
			}
			else
			{
				blocks.push_back({ MarkdownBlockType::Paragraph, 0, StripInlineMarkdown(line), {} });
				continuable = true;
			}
		}
		// A table whose rows were all delimiters says nothing.
		std::erase_if(blocks, [](const MarkdownBlock& block) { return block.Type == MarkdownBlockType::Table && block.Rows.empty(); });
		return blocks;
	}

	void DrawMarkdown(const char* id, std::span<const MarkdownBlock> blocks)
	{
		ImGui::PushID(id);
		for (size_t index = 0; index < blocks.size(); index++)
		{
			const MarkdownBlock& block = blocks[index];
			if (index > 0)
				ImGui::Spacing();
			switch (block.Type)
			{
				case MarkdownBlockType::Heading:
				{
					if (index > 0)
						ImGui::Spacing();
					Heading(block.Text, block.Level <= 1 ? TextSize::Title : TextSize::Body);
					break;
				}
				case MarkdownBlockType::Paragraph:
				{
					ImGui::PushTextWrapPos(0.0f);
					ImGui::TextUnformatted(block.Text.c_str(), block.Text.c_str() + block.Text.size());
					ImGui::PopTextWrapPos();
					break;
				}
				case MarkdownBlockType::ListItem:
				{
					ImGui::Bullet();
					ImGui::SameLine();
					ImGui::PushTextWrapPos(0.0f);
					ImGui::TextUnformatted(block.Text.c_str(), block.Text.c_str() + block.Text.size());
					ImGui::PopTextWrapPos();
					break;
				}
				case MarkdownBlockType::Table:
				{
					size_t columns = 0;
					for (const std::vector<std::string>& row : block.Rows)
						columns = std::max(columns, row.size());
					ImGui::PushID(static_cast<int>(index));
					if (columns > 0 && ImGui::BeginTable("Table", static_cast<int>(columns), ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
					{
						const std::vector<std::string>& header = block.Rows.front();
						for (size_t column = 0; column < columns; column++)
							ImGui::TableSetupColumn(column < header.size() ? header[column].c_str() : "");
						ImGui::TableHeadersRow();
						for (size_t row = 1; row < block.Rows.size(); row++)
						{
							ImGui::TableNextRow();
							for (size_t column = 0; column < block.Rows[row].size(); column++)
							{
								const std::string& cell = block.Rows[row][column];
								ImGui::TableSetColumnIndex(static_cast<int>(column));
								ImGui::PushTextWrapPos(0.0f);
								ImGui::TextUnformatted(cell.c_str(), cell.c_str() + cell.size());
								ImGui::PopTextWrapPos();
							}
						}
						ImGui::EndTable();
					}
					ImGui::PopID();
					break;
				}
			}
		}
		ImGui::PopID();
	}

}
