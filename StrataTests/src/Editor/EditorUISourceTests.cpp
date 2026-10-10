#include <doctest/doctest.h>

#include <Strata/Core/FileSystem.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
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
		// Comments and the contents of string and character literals blanked out (line breaks kept), so prose and text
		// that mention code do not count.
		std::string Code;
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

	// Blanks out the contents of string and character literals (in code without comments), keeping their quotes.
	std::string BlankLiterals(const std::string& code)
	{
		std::string result = code;
		char quote = '\0';
		for (size_t index = 0; index < result.size(); index++)
		{
			const char current = result[index];
			if (quote == '\0')
			{
				if (current == '"' || current == '\'')
					quote = current;
				continue;
			}
			if (current == quote)
			{
				quote = '\0';
				continue;
			}
			if (current == '\\' && index + 1 < result.size())
			{
				result[index] = ' ';
				index++;
			}
			if (result[index] != '\n')
				result[index] = ' ';
		}
		return result;
	}

	SourceFile MakeSourceFile(std::string name, const std::string& text)
	{
		return { std::move(name), BlankLiterals(StripComments(text)) };
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
			sources.push_back(MakeSourceFile(std::move(name), *text));
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

	// Findings by line: rules that find the same literal report it once.
	std::vector<std::string> Report(const SourceFile& file, const std::map<size_t, std::string>& findingsByOffset)
	{
		std::map<size_t, std::string> byLine;
		for (const auto& [offset, what] : findingsByOffset)
			byLine.emplace(GetLine(file.Code, offset), Describe(file, offset, what));
		std::vector<std::string> findings;
		for (const auto& [line, finding] : byLine)
			findings.push_back(finding);
		return findings;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Colors
	////////////////////////////////////////////////////////////////////////////////

	// A literal color, in any of the ways code spells one: a color type built from numbers (ImVec4(1, 0, 0, 1),
	// ImColor{...}, glm::vec4(...)) or declared from them (`const ImVec4 c_Red = { 1.0f, ... }`, `ImVec4 red(1.0f, ...)`),
	// ImGui's packed color macros and packed colors in hex (0xFF2020E0), and numbers in braces passed to what takes colors
	// (PushStyleColor(ImGuiCol_Text, { 1.0f, 0.4f, 0.35f, 1.0f }), drawList->AddRectFilled(min, max, { ... })).
	std::vector<std::string> FindColorLiterals(const SourceFile& file)
	{
		static const std::regex constructed(R"((\bImVec4|\bImColor|\bglm::vec4)\s*[\(\{]\s*[-+]?\.?\d)");
		static const std::regex declared(R"(\b(ImVec4|ImColor|glm::vec4)\s+\w+\s*(\[\w*\]\s*)?(=\s*)?[\(\{](\s*\{)*\s*[-+]?\.?\d)");
		static const std::regex packed(R"(\bIM_COL32\w*|\b0[xX][0-9A-Fa-f]{6,8}[uUlL]*\b)");
		static const std::regex colorCall(R"(\b(\w*Colou?r\w*|Add(Rect|Line|Circle|Triangle|Quad|Ngon|Polyline|ConvexPolyFilled|Text|Bezier|Image)\w*)\s*\()");
		static const std::regex numberList(R"(\{\s*[-+]?(\d+\.?\d*|\.\d+)f?\s*,\s*[-+]?(\d+\.?\d*|\.\d+)f?\s*,\s*[-+]?(\d+\.?\d*|\.\d+)f?)");

		std::map<size_t, std::string> findings;
		for (const std::regex* rule : { &constructed, &declared, &packed })
		{
			for (std::sregex_iterator it(file.Code.begin(), file.Code.end(), *rule), end; it != end; ++it)
				findings.emplace(static_cast<size_t>(it->position()), it->str());
		}
		for (std::sregex_iterator it(file.Code.begin(), file.Code.end(), colorCall), end; it != end; ++it)
		{
			const size_t open = static_cast<size_t>(it->position() + it->length() - 1);
			const std::string arguments = GetArguments(file.Code, open);
			std::smatch list;
			if (std::regex_search(arguments, list, numberList))
				findings.emplace(open + 1 + static_cast<size_t>(list.position()), (*it)[1].str() + "(... " + list.str() + " ...)");
		}
		return Report(file, findings);
	}

	////////////////////////////////////////////////////////////////////////////////
	// Sizes
	////////////////////////////////////////////////////////////////////////////////

	// Sizes that follow the UI scale: the font's, frames' and text's sizes, the style's sizes (the theme scales them) and
	// sizes taken from the layout (windows, available space, items).
	const std::regex& GetScaledSizeRule()
	{
		static const std::regex scaled(R"(\b(GetFontSize|GetFrameHeight\w*|GetTextLineHeight\w*|GetTextSize|CalcTextSize|GetStyle|GetContentRegionAvail)"
			R"(|GetWindowWidth|GetWindowHeight|GetWindowSize|GetItemRect\w*|GetCursor\w*|FontSize|FontSizeBase|FontScaleMain|FontScaleDpi|WorkSize)"
			R"(|DisplaySize)\b|\b\w*[sS]tyle\s*\.\s*\w+)");
		return scaled;
	}

	// Names declared in the code, by whether their value follows the UI scale or holds fixed pixels above 16.
	struct SizeNames
	{
		std::set<std::string> Scaled;
		std::set<std::string> Unscaled;
	};

	bool IsIdentifierCharacter(char character)
	{
		return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
	}

	// The product (or quotient) a value at [begin, end) of `text` is part of, e.g. "ImGui::GetFontSize() * 20.0f" for
	// the 20.0f in "ImVec2(ImGui::GetFontSize() * 20.0f, 300.0f)", and "300.0f" for the 300.0f: what scales it.
	std::string GetTerm(const std::string& text, size_t begin, size_t end)
	{
		auto isBoundary = [&text](size_t index)
		{
			const char character = text[index];
			const char before = index > 0 ? text[index - 1] : '\0';
			const char after = index + 1 < text.size() ? text[index + 1] : '\0';
			if (character == ':' && (before == ':' || after == ':'))
				return false; // Scope resolution
			if (character == '-' && after == '>')
				return false; // Member access
			return std::string_view(",;{}+-?:=|&!").find(character) != std::string_view::npos;
		};
		size_t first = begin;
		for (int depth = 0; first > 0; first--)
		{
			const char character = text[first - 1];
			if (character == ')' || character == ']')
				depth++;
			else if (character == '(' || character == '[')
			{
				if (depth == 0)
					break;
				depth--;
			}
			else if (depth == 0 && isBoundary(first - 1))
				break;
		}
		size_t last = end;
		for (int depth = 0; last < text.size(); last++)
		{
			const char character = text[last];
			if (character == '(' || character == '[')
				depth++;
			else if (character == ')' || character == ']')
			{
				if (depth == 0)
					break;
				depth--;
			}
			else if (depth == 0 && isBoundary(last))
				break;
		}
		return text.substr(first, last - first);
	}

	// The identifiers in `text`, with their offsets. Numbers with their suffixes (20.0f, 0xFF) are none.
	std::vector<std::pair<size_t, std::string>> FindIdentifiers(const std::string& text)
	{
		std::vector<std::pair<size_t, std::string>> identifiers;
		size_t index = 0;
		while (index < text.size())
		{
			if (!IsIdentifierCharacter(text[index]))
			{
				index++;
				continue;
			}
			const size_t start = index;
			const bool number = std::isdigit(static_cast<unsigned char>(text[start])) != 0;
			while (index < text.size() && (IsIdentifierCharacter(text[index]) || (number && text[index] == '.')))
				index++;
			if (!number)
				identifiers.emplace_back(start, text.substr(start, index - start));
		}
		return identifiers;
	}

	// Whether a term follows the UI scale: it uses a scaled size, or a name that only ever holds scaled sizes.
	bool IsScaled(const std::string& term, const SizeNames& names)
	{
		if (std::regex_search(term, GetScaledSizeRule()))
			return true;
		for (const auto& [offset, identifier] : FindIdentifiers(term))
		{
			if (names.Scaled.contains(identifier) && !names.Unscaled.contains(identifier))
				return true;
		}
		return false;
	}

	// The first value in `text` that is fixed pixels above 16: a number, or a name holding one, not scaled by what it is
	// multiplied with. Nothing when every size in it follows the UI scale.
	std::optional<std::string> FindUnscaledSize(const std::string& text, const SizeNames& names)
	{
		static const std::regex number(R"((^|[^\w.])((\d+\.?\d*|\.\d+)f?)\b)");
		for (std::sregex_iterator it(text.begin(), text.end(), number), end; it != end; ++it)
		{
			const size_t begin = static_cast<size_t>(it->position(2));
			const std::string value = (*it)[2].str();
			if (std::stod((*it)[3].str()) > 16.0 && !IsScaled(GetTerm(text, begin, begin + value.size()), names))
				return value;
		}
		for (const auto& [offset, identifier] : FindIdentifiers(text))
		{
			if (names.Unscaled.contains(identifier) && !IsScaled(GetTerm(text, offset, offset + identifier.size()), names))
				return identifier;
		}
		return std::nullopt;
	}

	// The initializer of a declaration that starts at `start` ('=', '(' or '{'): up to the ';' or ',' that ends it.
	std::string GetInitializer(const std::string& code, size_t start)
	{
		size_t index = code[start] == '=' ? start + 1 : start;
		for (int depth = 0; index < code.size(); index++)
		{
			const char character = code[index];
			if (character == '(' || character == '[' || character == '{')
				depth++;
			else if (character == ')' || character == ']' || character == '}')
			{
				if (depth == 0)
					break;
				if (--depth == 0 && code[start] != '=')
				{
					index++;
					break;
				}
			}
			else if (depth == 0 && (character == ';' || character == ','))
				break;
		}
		return code.substr(start, index - start);
	}

	// Sorts the names the code declares (variables and constants of number and size types) into those that follow the UI
	// scale and those that hold fixed pixels above 16, also through other names (`const float width = c_Width * 2.0f`).
	void ClassifySizeNames(const std::string& code, SizeNames& names)
	{
		static const std::regex declaration(R"(\b(float|double|int|unsigned|auto|ImVec2|uint32_t|int32_t|size_t)\s+(\w+)\s*(=|\(|\{))");
		std::vector<std::pair<std::string, std::string>> declarations;
		for (std::sregex_iterator it(code.begin(), code.end(), declaration), end; it != end; ++it)
			declarations.emplace_back((*it)[2].str(), GetInitializer(code, static_cast<size_t>(it->position(3))));
		for (bool changed = true; changed;)
		{
			changed = false;
			for (const auto& [name, initializer] : declarations)
			{
				if (FindUnscaledSize(initializer, names))
					changed |= names.Unscaled.insert(name).second;
				else if (IsScaled(initializer, names))
					changed |= names.Scaled.insert(name).second;
			}
		}
	}

	// Calls that take sizes in pixels with a value among their arguments that is fixed pixels above 16, a number or a
	// name holding one, not scaled by the font (the UI scale multiplies the font size; fixed pixels would not grow on a
	// 150% display). `shared` holds names other files declare (headers).
	std::vector<std::string> FindUnscaledSizes(const SourceFile& file, const SizeNames& shared = {})
	{
		static const std::regex call(R"(\b(Button|SmallButton|InvisibleButton|ImageButton|Image|Selectable|BeginChild|BeginListBox|Dummy|SetNextWindowSize)"
			R"(|SetNextWindowContentSize|SetNextWindowSizeConstraints|SetNextItemWidth|PushItemWidth|SetColumnWidth|TableSetupColumn|Card)"
			R"(|InputTextMultiline|ProgressBar|SameLine|Indent|Unindent|SetCursorPos|SetCursorPosX|SetCursorPosY)\s*\()");
		SizeNames names = shared;
		ClassifySizeNames(file.Code, names);
		std::map<size_t, std::string> findings;
		for (std::sregex_iterator it(file.Code.begin(), file.Code.end(), call), end; it != end; ++it)
		{
			const size_t open = static_cast<size_t>(it->position() + it->length() - 1);
			const std::string arguments = GetArguments(file.Code, open);
			if (const std::optional<std::string> value = FindUnscaledSize(arguments, names))
				findings.emplace(open, (*it)[1].str() + "(" + arguments + "): " + *value);
		}
		return Report(file, findings);
	}

	// Names the UI headers declare, which other files use.
	SizeNames ClassifyHeaderNames(const std::vector<SourceFile>& files)
	{
		SizeNames names;
		for (const SourceFile& file : files)
		{
			if (file.Name.ends_with(".h"))
				ClassifySizeNames(file.Code, names);
		}
		return names;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Panels
	////////////////////////////////////////////////////////////////////////////////

	// The registry begins and ends every panel's window (title, icon, open state, settings).
	std::vector<std::string> FindOwnWindows(const SourceFile& file)
	{
		static const std::regex begin(R"(\bImGui::Begin\s*\()");
		std::vector<std::string> findings;
		for (std::sregex_iterator it(file.Code.begin(), file.Code.end(), begin), end; it != end; ++it)
			findings.push_back(Describe(file, static_cast<size_t>(it->position()), it->str()));
		return findings;
	}

	size_t CountColorFindings(const std::string& code)
	{
		return FindColorLiterals(MakeSourceFile("Test.cpp", code)).size();
	}

	size_t CountSizeFindings(const std::string& code)
	{
		return FindUnscaledSizes(MakeSourceFile("Test.cpp", code)).size();
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
		const std::vector<SourceFile> sources = LoadUISources();
		const SizeNames shared = ClassifyHeaderNames(sources);
		std::vector<std::string> findings;
		for (const SourceFile& file : sources)
		{
			for (std::string& finding : FindUnscaledSizes(file, shared))
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

	TEST_CASE("The color rule finds colors however they are written")
	{
		// The ways the editor's code spelled colors before the theme, and others.
		CHECK(CountColorFindings("ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), \"error\");") == 1);
		CHECK(CountColorFindings("ImGui::PushStyleColor(ImGuiCol_Button, ImColor{ 40, 40, 40 });") == 1);
		CHECK(CountColorFindings("const ImU32 c_OverlayBackground = IM_COL32(0, 0, 0, 160);") == 1);
		CHECK(CountColorFindings("const ImVec4 c_ActiveButtonColor = { 0.24f, 0.42f, 0.70f, 1.0f };") == 1);
		CHECK(CountColorFindings("static const ImVec4 c_StateColors[] = { { 0.3f, 0.6f, 1.0f, 1.0f } };") == 1);
		CHECK(CountColorFindings("ImVec4 color(1.0f, 0.0f, 0.0f, 1.0f);") == 1);
		CHECK(CountColorFindings("ImVec4 color{ .5f, 0.0f, 0.0f, 1.0f };") == 1);
		CHECK(CountColorFindings("ImGui::PushStyleColor(ImGuiCol_Button, { 0.2f, 0.2f, 0.2f, 1.0f });") == 1);
		CHECK(CountColorFindings("drawList->AddRectFilled(min, max, 0xFF202020);") == 1);
		CHECK(CountColorFindings("drawList->AddLine(a, b, ImGui::GetColorU32({ 1.0f, 1.0f, 1.0f, 0.5f }));") == 1);
		CHECK(CountColorFindings("settings.SelectionColor = glm::vec4(1.0f, 0.55f, 0.1f, 1.0f);") == 1);

		// What the rule allows: tokens, colors derived from them, numbers that are no colors, and text.
		CHECK(CountColorFindings("ImGui::TextColored(UI::GetThemeColors().Error, \"error\");") == 0);
		CHECK(CountColorFindings("ImVec4 background = WithAlpha(colors.Control, 0.0f);") == 0);
		CHECK(CountColorFindings("ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));") == 0);
		CHECK(CountColorFindings("constexpr float c_TextSizes[] = { 12.0f, 14.0f, 17.0f, 24.0f };") == 0);
		CHECK(CountColorFindings("constexpr ImWchar c_IconRanges[] = { 0xE000, 0xF8FF, 0 };") == 0);
		CHECK(CountColorFindings("drawList->AddText(ImVec2(x, y), color, \"ImVec4(1, 0, 0, 1) 0xFF202020\");") == 0);
		CHECK(CountColorFindings("ImU32 Color = 0; // No color") == 0);
	}

	TEST_CASE("The size rule finds pixels that do not follow the UI scale")
	{
		// The ways the editor's code spelled sizes before, and others.
		CHECK(CountSizeFindings("if (ImGui::Button(\"Save\", ImVec2(100.0f, 0.0f)))") == 1);
		CHECK(CountSizeFindings("ImGui::SetNextItemWidth(120.0f);") == 1);
		CHECK(CountSizeFindings("if (ImGui::BeginChild(\"Assets\", ImVec2(320.0f, 280.0f)))") == 1);
		// A name holding pixels, used later (InspectorPanel's Add Component button).
		CHECK(CountSizeFindings("const float buttonWidth = 200.0f;\nImGui::Button(\"Add Component\", ImVec2(buttonWidth, 0.0f));") == 1);
		CHECK(CountSizeFindings("constexpr float c_PanelWidth = 320.0f;\nconst float width = c_PanelWidth * 0.5f;\nImGui::BeginChild(\"Panel\", ImVec2(width, 0.0f));") == 1);
		// One scaled size does not excuse another.
		CHECK(CountSizeFindings("ImGui::Button(\"Wide\", ImVec2(ImGui::GetFontSize() * 10.0f, 300.0f));") == 1);
		CHECK(CountSizeFindings("ImGui::SetNextWindowSize(ImVec2(std::max(ImGui::GetFontSize() * 20.0f, 400.0f), 0.0f));") == 1);
		CHECK(CountSizeFindings("ImGui::SameLine(0.0f, 40.0f);") == 1);

		// What the rule allows: sizes from the font, the style and the layout, small pixel counts, and text.
		CHECK(CountSizeFindings("if (ImGui::Button(\"Save\", ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f)))") == 0);
		CHECK(CountSizeFindings("ImGui::SetNextItemWidth(-FLT_MIN);") == 0);
		CHECK(CountSizeFindings("ImGui::Dummy(ImVec2(4.0f, ImGui::GetFrameHeight()));") == 0);
		CHECK(CountSizeFindings("ImGui::Dummy(ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y * 20.0f));") == 0);
		CHECK(CountSizeFindings("ImGui::BeginChild(\"Messages\", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);") == 0);
		CHECK(CountSizeFindings("const float fieldWidth = ImGui::GetFontSize() * 20.0f;\nImGui::SetNextItemWidth(fieldWidth);") == 0);
		CHECK(CountSizeFindings("ImGui::SetNextItemWidth(static_cast<float>(count) * ImGui::GetFontSize() * 18.0f);") == 0);
		CHECK(CountSizeFindings("if (ImGui::Button(\"Save 200 files\"))") == 0);
		CHECK(CountSizeFindings("UI::ToolbarButton(\"Toolbar.Play\", Icons::Play, nullptr, \"Play\");") == 0);
	}

	TEST_CASE("The panel rule finds windows panels begin themselves")
	{
		const SourceFile panel = MakeSourceFile("Panels/Old.cpp", "if (!ImGui::Begin(\"Inspector\"))\n\treturn;\nImGui::BeginPopup(\"Settings\");\n");
		CHECK(FindOwnWindows(panel).size() == 1);
	}

	TEST_CASE("The source scan sees through comments and text but not into code")
	{
		const std::string stripped = StripComments("a // ImVec4(1, 0, 0, 1)\nb /* IM_COL32(1, 2, 3, 4) */ c \"// kept\" '/'");
		CHECK(stripped.find("ImVec4") == std::string::npos);
		CHECK(stripped.find("IM_COL32") == std::string::npos);
		CHECK(stripped.find("\"// kept\"") != std::string::npos);
		CHECK(stripped.find('\n') == 23);
		// Parentheses and quotes in text do not confuse the argument lists.
		const std::string blanked = BlankLiterals("Button(\"(x\\\")\", ImVec2(f(1), 2)) + '\\'' + 1");
		CHECK(blanked == "Button(\"     \", ImVec2(f(1), 2)) + '  ' + 1");
		CHECK(GetArguments(blanked, 6) == "\"     \", ImVec2(f(1), 2)");
		// A value's term is the product it is part of.
		const std::string sizes = "ImVec2(ImGui::GetFontSize() * 20.0f, 300.0f)";
		CHECK(GetTerm(sizes, 30, 35) == "ImGui::GetFontSize() * 20.0f");
		CHECK(GetTerm(sizes, 37, 43) == " 300.0f");
		CHECK(GetTerm("a + style->Padding * 30.0f", 21, 26) == " style->Padding * 30.0f");
		CHECK(FindIdentifiers("width * 20.0f + 1e-6f") == std::vector<std::pair<size_t, std::string>> { { 0, "width" } });
	}
}
