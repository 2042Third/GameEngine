#include <doctest/doctest.h>

#include "Editor/ImGuiHarness.h"
#include "UI/Markdown.h"
#include "UI/TextFormat.h"
#include "UI/Widgets.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/PlatformDetection.h>

#include <imgui.h>

#include <string>

using namespace Strata;
using namespace Strata::Tests;

TEST_SUITE("Editor.UI.Text")
{
	TEST_CASE("Markdown blocks: headings, paragraphs, list items and tables")
	{
		const std::vector<UI::MarkdownBlock> blocks = UI::ParseMarkdown(
			"# Third-Party Notices\r\n"
			"\n"
			"Strata includes the following\n"
			"software as `submodules`.\n"
			"\n"
			"| Library | License |\n"
			"| --- | :-: |\n"
			"| [GLFW](https://github.com/glfw/glfw) | zlib |\n"
			"| **ImGui** \\| docking | MIT |\n"
			"- one item\n"
			"  continued\n"
			"* two <https://example.com>\n"
			"## Fonts\n"
			"#notaheading\n");
		REQUIRE(blocks.size() == 7);
		CHECK(blocks[0].Type == UI::MarkdownBlockType::Heading);
		CHECK(blocks[0].Level == 1);
		CHECK(blocks[0].Text == "Third-Party Notices");
		CHECK(blocks[1].Type == UI::MarkdownBlockType::Paragraph);
		CHECK(blocks[1].Text == "Strata includes the following software as submodules.");
		REQUIRE(blocks[2].Type == UI::MarkdownBlockType::Table);
		REQUIRE(blocks[2].Rows.size() == 3);
		CHECK(blocks[2].Rows[0] == std::vector<std::string> { "Library", "License" });
		CHECK(blocks[2].Rows[1] == std::vector<std::string> { "GLFW", "zlib" });
		CHECK(blocks[2].Rows[2] == std::vector<std::string> { "ImGui | docking", "MIT" });
		CHECK(blocks[3].Type == UI::MarkdownBlockType::ListItem);
		CHECK(blocks[3].Text == "one item continued");
		CHECK(blocks[4].Text == "two https://example.com");
		CHECK(blocks[5].Type == UI::MarkdownBlockType::Heading);
		CHECK(blocks[5].Level == 2);
		// "#" without a space is text.
		CHECK(blocks[6].Type == UI::MarkdownBlockType::Paragraph);
		CHECK(blocks[6].Text == "#notaheading");
	}

	TEST_CASE("Inline Markdown keeps the text")
	{
		CHECK(UI::StripInlineMarkdown("[spdlog](https://x) (bundles [fmt](https://y))") == "spdlog (bundles fmt)");
		CHECK(UI::StripInlineMarkdown("[Inter](https://x) by `The Authors`") == "Inter by The Authors");
		CHECK(UI::StripInlineMarkdown("**bold** and \\*star\\*") == "bold and *star*");
		// Unclosed markup stays as it is.
		CHECK(UI::StripInlineMarkdown("[open (and `tick") == "[open (and `tick");
		CHECK(UI::StripInlineMarkdown("a < b > c") == "a < b > c");
		CHECK(UI::ParseMarkdown("").empty());
		CHECK(UI::ParseMarkdown("| --- |\n").empty());
	}

	TEST_CASE("The real notices parse into headings and tables")
	{
		const std::optional<std::string> notices = FileSystem::ReadText(FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "ThirdPartyNotices.md");
		REQUIRE(notices.has_value());
		const std::vector<UI::MarkdownBlock> blocks = UI::ParseMarkdown(*notices);
		REQUIRE_FALSE(blocks.empty());
		CHECK(blocks.front().Type == UI::MarkdownBlockType::Heading);
		size_t tables = 0;
		bool glfw = false;
		for (const UI::MarkdownBlock& block : blocks)
		{
			if (block.Type != UI::MarkdownBlockType::Table)
				continue;
			tables++;
			for (const std::vector<std::string>& row : block.Rows)
				glfw |= !row.empty() && row.front() == "GLFW";
		}
		CHECK(tables >= 2);
		CHECK(glfw);

		// And draw without a GPU.
		ImGuiHarness harness;
		harness.Frame([&]()
		{
			ImGui::Begin("Notices");
			UI::DrawMarkdown("Notices", blocks);
			ImGui::End();
		});
	}

	TEST_CASE("Times ago, initials and paths read naturally")
	{
		const int64_t now = 1'800'000'000;
		CHECK(UI::DescribeTimeAgo(now, now) == "just now");
		CHECK(UI::DescribeTimeAgo(now + 100, now) == "just now");
		CHECK(UI::DescribeTimeAgo(now - 90, now) == "a minute ago");
		CHECK(UI::DescribeTimeAgo(now - 5 * 60, now) == "5 minutes ago");
		CHECK(UI::DescribeTimeAgo(now - 90 * 60, now) == "an hour ago");
		CHECK(UI::DescribeTimeAgo(now - 3 * 3600, now) == "3 hours ago");
		CHECK(UI::DescribeTimeAgo(now - 30 * 3600, now) == "yesterday");
		CHECK(UI::DescribeTimeAgo(now - 4 * 86400, now) == "4 days ago");
		// 2026-01-01 00:00:00 UTC.
		CHECK(UI::DescribeTimeAgo(1'767'225'600, 1'767'225'600 + 60 * 86400) == "2026-01-01");

		CHECK(UI::GetInitials("Space Game") == "SG");
		CHECK(UI::GetInitials("FeatureTest") == "FT");
		CHECK(UI::GetInitials("tetris") == "T");
		CHECK(UI::GetInitials("MYGAME") == "M");
		CHECK(UI::GetInitials("my-cool_game") == "MC");
		CHECK(UI::GetInitials("\xC3\xA9t\xC3\xA9") == "\xC3\xA9");
		CHECK(UI::GetInitials("  ") == "?");

#if defined(ST_PLATFORM_WINDOWS)
		CHECK(UI::DisplayPath(FileSystem::FromUTF8("C:/Projects/Game")) == "C:\\Projects\\Game");
#else
		CHECK(UI::DisplayPath(FileSystem::FromUTF8("/home/me/Game")) == "/home/me/Game");
#endif
	}

	TEST_CASE("Long paths are shortened at their start")
	{
		ImGuiHarness harness;
		harness.Frame([]()
		{
			const std::string path = "C:/Users/someone/Projects/Strata/Games/A Very Long Project Name/Game.stproj";
			const float full = ImGui::CalcTextSize(path.c_str()).x;
			CHECK(UI::ElideStart(path, full + 1.0f) == path);
			const float limit = full * 0.5f;
			const std::string shortened = UI::ElideStart(path, limit);
			CHECK(shortened.rfind("\xE2\x80\xA6", 0) == 0);
			CHECK(ImGui::CalcTextSize(shortened.c_str()).x <= limit);
			// The end is kept, and as much of it as fits.
			const std::string kept = shortened.substr(3);
			CHECK(path.size() >= kept.size());
			CHECK(path.compare(path.size() - kept.size(), kept.size(), kept) == 0);
			CHECK(ImGui::CalcTextSize(("\xE2\x80\xA6" + path.substr(path.size() - kept.size() - 1)).c_str()).x > limit);
			// Multibyte characters are never cut.
			const std::string accents = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
			const std::string cut = UI::ElideStart(accents, ImGui::CalcTextSize(accents.c_str()).x * 0.5f);
			CHECK((cut.size() - 3) % 2 == 0);
		});
	}
}
