#include <doctest/doctest.h>

#include <Strata/Core/FileSystem.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

// Rules for the editor's UI code that a compiler cannot check (AGENTS.md, "Editor"): colors come only from the theme's
// tokens (UI/Theme.h), sizes scale with the font (they follow the UI scale), panels draw through the panel registry, and
// ImGui's built-in font is never used. The allowlists are empty: fix the code, never list it here.

namespace
{

	struct SourceFile
	{
		std::string Name; // Relative to StrataEditor/src, '/' separators
		std::string Code; // Comments blanked out (line breaks kept), so prose that mentions code does not count
	};

	// Blanks out // and /* */ comments outside string and character literals, keeping line breaks for line numbers.
	std::string StripComments(const std::string& text)
	{
		std::string result = text;
		enum class State { Code, LineComment, BlockComment, String, Character } state = State::Code;
		for (size_t index = 0; index < result.size(); index++)
		{
			const char current = result[index];
			const char next = index + 1 < result.size() ? result[index + 1] : '\0';
			switch (state)
			{
				case State::Code:
					if (current == '/' && next == '/')
						state = State::LineComment;
					else if (current == '/' && next == '*')
						state = State::BlockComment;
					else if (current == '"')
						state = State::String;
					else if (current == '\'')
						state = State::Character;
					break;
				case State::String:
				case State::Character:
					if (current == '\\')
						index++;
					else if ((state == State::String && current == '"') || (state == State::Character && current == '\''))
						state = State::Code;
					break;
				case State::LineComment:
					if (current == '\n')
						state = State::Code;
					break;
				case State::BlockComment:
					if (current == '*' && next == '/')
					{
						result[index] = ' ';
						result[index + 1] = ' ';
						index++;
						state = State::Code;
					}
					break;
			}
			if ((state == State::LineComment || state == State::BlockComment) && result[index] != '\n')
				result[index] = ' ';
		}
		return result;
	}

	std::filesystem::path GetEditorSourceDirectory()
	{
		return FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "StrataEditor" / "src";
	}

	// The UI code: EditorLayer, the panels and the UI library.
	std::vector<SourceFile> LoadSources(const std::vector<std::string>& directories, const std::vector<std::string>& files)
	{
		const std::filesystem::path root = GetEditorSourceDirectory();
		std::vector<std::filesystem::path> paths;
		for (const std::string& directory : directories)
		{
			std::error_code error;
			for (std::filesystem::recursive_directory_iterator it(root / directory, error), end; !error && it != end; it.increment(error))
			{
				const std::filesystem::path extension = it->path().extension();
				if (it->is_regular_file() && (extension == ".cpp" || extension == ".h"))
					paths.push_back(it->path());
			}
			REQUIRE_MESSAGE(!error, "Cannot list ", FileSystem::ToUTF8(root / directory));
		}
		for (const std::string& file : files)
			paths.push_back(root / file);
		std::sort(paths.begin(), paths.end());

		std::vector<SourceFile> sources;
		for (const std::filesystem::path& path : paths)
		{
			const std::optional<std::string> text = FileSystem::ReadText(path);
			REQUIRE_MESSAGE(text.has_value(), "Cannot read ", FileSystem::ToUTF8(path));
			std::string name = FileSystem::ToUTF8(path.lexically_relative(root));
			std::replace(name.begin(), name.end(), '\\', '/');
			sources.push_back({ std::move(name), StripComments(*text) });
		}
		REQUIRE(!sources.empty());
		return sources;
	}

	std::vector<SourceFile> LoadUISources()
	{
		return LoadSources({ "Panels", "UI" }, { "EditorLayer.cpp" });
	}

	size_t GetLine(const std::string& text, size_t offset)
	{
		return 1 + static_cast<size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
	}

	// The text between the parenthesis at `open` and the one that closes it.
	std::string GetArguments(const std::string& text, size_t open)
	{
		int depth = 0;
		for (size_t index = open; index < text.size(); index++)
		{
			if (text[index] == '(')
				depth++;
			else if (text[index] == ')' && --depth == 0)
				return text.substr(open + 1, index - open - 1);
		}
		return std::string();
	}

	std::string Describe(const SourceFile& file, size_t offset, const std::string& what)
	{
		return file.Name + ":" + std::to_string(GetLine(file.Code, offset)) + ": " + what;
	}

	// A literal color: ImVec4/ImColor built from numbers, or ImGui's packed color macros.
	std::vector<std::string> FindColorLiterals(const SourceFile& file)
	{
		static const std::regex literal(R"((\bImVec4|\bImColor)\s*[\(\{]\s*[-+]?(\d|\.\d)|\bIM_COL32\w*)");
		std::vector<std::string> findings;
		for (std::sregex_iterator it(file.Code.begin(), file.Code.end(), literal), end; it != end; ++it)
			findings.push_back(Describe(file, static_cast<size_t>(it->position()), it->str()));
		return findings;
	}

	// Calls that take sizes in pixels with a number above 16 among their arguments that is not scaled by the font (the
	// UI scale multiplies the font size; fixed pixels would not grow on a 150% display).
	std::vector<std::string> FindUnscaledSizes(const SourceFile& file)
	{
		static const std::regex call(R"(\b(Button|BeginChild|SetNextWindowSize|SetNextItemWidth|PushItemWidth|InvisibleButton|Dummy)\s*\()");
		static const std::regex number(R"((^|[^\w.])((\d+\.?\d*|\.\d+)f?)\b)");
		static const std::regex scaled(R"(GetFontSize|GetFrameHeight|GetTextLineHeight|GetTextSize|FontSize)");
		std::vector<std::string> findings;
		for (std::sregex_iterator it(file.Code.begin(), file.Code.end(), call), end; it != end; ++it)
		{
			const size_t open = static_cast<size_t>(it->position() + it->length() - 1);
			const std::string arguments = GetArguments(file.Code, open);
			if (std::regex_search(arguments, scaled))
				continue;
			for (std::sregex_iterator value(arguments.begin(), arguments.end(), number); value != std::sregex_iterator(); ++value)
			{
				if (std::stod((*value)[3].str()) > 16.0)
				{
					findings.push_back(Describe(file, open, (*it)[1].str() + "(" + arguments + ")"));
					break;
				}
			}
		}
		return findings;
	}

	// The registry begins and ends every panel's window (title, icon, open state, settings).
	std::vector<std::string> FindOwnWindows(const SourceFile& file)
	{
		static const std::regex begin(R"(\bImGui::Begin\s*\()");
		std::vector<std::string> findings;
		for (std::sregex_iterator it(file.Code.begin(), file.Code.end(), begin), end; it != end; ++it)
			findings.push_back(Describe(file, static_cast<size_t>(it->position()), it->str()));
		return findings;
	}

}

TEST_SUITE("Editor.UI.Source")
{
	TEST_CASE("Colors come only from the theme")
	{
		std::vector<std::string> findings;
		for (const SourceFile& file : LoadUISources())
		{
			if (file.Name == "UI/Theme.cpp")
				continue;
			for (std::string& finding : FindColorLiterals(file))
				findings.push_back(std::move(finding));
		}
		for (const std::string& finding : findings)
			FAIL_CHECK("A color literal outside UI/Theme.cpp (use the theme's tokens): " << finding);
		CHECK(findings.empty());
	}

	TEST_CASE("Sizes scale with the font")
	{
		std::vector<std::string> findings;
		for (const SourceFile& file : LoadUISources())
		{
			for (std::string& finding : FindUnscaledSizes(file))
				findings.push_back(std::move(finding));
		}
		for (const std::string& finding : findings)
			FAIL_CHECK("A pixel size that does not scale with the font: " << finding);
		CHECK(findings.empty());
	}

	TEST_CASE("Panels draw through the panel registry")
	{
		std::vector<std::string> findings;
		for (const SourceFile& file : LoadSources({ "Panels" }, {}))
		{
			for (std::string& finding : FindOwnWindows(file))
				findings.push_back(std::move(finding));
		}
		for (const std::string& finding : findings)
			FAIL_CHECK("A panel that begins its own window (register it with EditorPanelRegistry instead): " << finding);
		CHECK(findings.empty());
	}

	TEST_CASE("The editor never uses ImGui's built-in font")
	{
		std::vector<std::string> findings;
		for (const SourceFile& file : LoadSources({ "" }, {}))
		{
			const size_t found = file.Code.find("AddFontDefault");
			if (found != std::string::npos)
				findings.push_back(Describe(file, found, "AddFontDefault"));
		}
		for (const std::string& finding : findings)
			FAIL_CHECK("The editor uses its own fonts (UI/EditorFonts.h): " << finding);
		CHECK(findings.empty());
	}

	TEST_CASE("The source rules catch what they are meant to")
	{
		// The editor's code before the theme: every line breaks a rule.
		const SourceFile old { "Old.cpp", StripComments(
			"ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), \"error\");\n"
			"const ImU32 c_OverlayBackground = IM_COL32(0, 0, 0, 160);\n"
			"if (ImGui::Button(\"Save\", ImVec2(100.0f, 0.0f)))\n"
			"ImGui::SetNextItemWidth(120.0f);\n"
			"if (ImGui::BeginChild(\"Assets\", ImVec2(320.0f, 280.0f)))\n"
			"if (!ImGui::Begin(\"Inspector\"))\n") };
		CHECK(FindColorLiterals(old).size() == 2);
		CHECK(FindUnscaledSizes(old).size() == 3);
		CHECK(FindOwnWindows(old).size() == 1);

		// What the rules allow: tokens, sizes from the font, small pixel counts, and prose in comments.
		const SourceFile fine { "Fine.cpp", StripComments(
			"ImGui::TextColored(UI::GetThemeColors().Error, \"error\"); // ImVec4(1, 0, 0, 1)\n"
			"if (ImGui::Button(\"Save\", ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f)))\n"
			"ImGui::SetNextItemWidth(-FLT_MIN);\n"
			"ImGui::Dummy(ImVec2(4.0f, ImGui::GetFrameHeight()));\n"
			"ImGui::BeginChild(\"Messages\", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);\n"
			"UI::ToolbarButton(\"Toolbar.Play\", Icons::Play, nullptr, \"Play\");\n"
			"ImGui::BeginPopup(\"Settings\");\n") };
		CHECK(FindColorLiterals(fine).empty());
		CHECK(FindUnscaledSizes(fine).empty());
		CHECK(FindOwnWindows(fine).empty());
	}

	TEST_CASE("The source scan sees through comments but not into code")
	{
		const std::string stripped = StripComments("a // ImVec4(1, 0, 0, 1)\nb /* IM_COL32(1, 2, 3, 4) */ c \"// kept\" '/'");
		CHECK(stripped.find("ImVec4") == std::string::npos);
		CHECK(stripped.find("IM_COL32") == std::string::npos);
		CHECK(stripped.find("\"// kept\"") != std::string::npos);
		CHECK(stripped.find('\n') == 23);
		CHECK(GetArguments("Button(\"x\", ImVec2(f(1), 2)) + 1", 6) == "\"x\", ImVec2(f(1), 2)");
	}
}
