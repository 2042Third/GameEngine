#include <doctest/doctest.h>

#include "FeatureTest/FeatureTestUtils.h"
#include "Strata/Core/FileSystem.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

// Checks mechanically that the feature scripts use the whole public script SDK: every public member function of every
// SDK class outside namespace Detail, and every public SDK macro. The SDK headers are read as text: a small tokenizer
// strips comments, literals and preprocessor lines (recording #define bodies), and a scope-tracking pass collects the
// declarations. The check is name-based: a member counts as used when its name appears in the feature scripts (as
// "Class::Name" for classes with only static members), so overloads and same-named members of different classes are
// covered together; the run of the feature test verifies the behavior.
namespace
{

	struct Token
	{
		std::string Text;
		bool Identifier = false;
	};

	struct SourceTokens
	{
		std::vector<Token> Tokens;
		std::map<std::string, std::vector<Token>> Macros; // #define name -> body
	};

	bool IsIdentifierStart(char character)
	{
		return std::isalpha(static_cast<unsigned char>(character)) || character == '_';
	}

	bool IsIdentifierPart(char character)
	{
		return std::isalnum(static_cast<unsigned char>(character)) || character == '_';
	}

	void TokenizeCode(std::string_view code, std::vector<Token>& out);

	// The macro name and body of a "#define" line (continuations joined); nullopt for other directives.
	std::optional<std::pair<std::string, std::vector<Token>>> ParseDirective(std::string_view directive)
	{
		size_t position = directive.find_first_not_of(" \t", 1);
		if (position == std::string_view::npos || directive.substr(position, 6) != "define")
			return std::nullopt;
		position = directive.find_first_not_of(" \t", position + 6);
		if (position == std::string_view::npos)
			return std::nullopt;

		size_t end = position;
		while (end < directive.size() && IsIdentifierPart(directive[end]))
			end++;
		std::string name(directive.substr(position, end - position));
		if (end < directive.size() && directive[end] == '(')
			end = directive.find(')', end) + 1; // Parameter list
		std::vector<Token> body;
		if (end != 0 && end < directive.size())
			TokenizeCode(directive.substr(end), body);
		return std::make_pair(std::move(name), std::move(body));
	}

	// Identifiers and punctuation of code without comments, literals or directives ("::" and "->" are single tokens).
	void TokenizeCode(std::string_view code, std::vector<Token>& out)
	{
		size_t index = 0;
		while (index < code.size())
		{
			const char character = code[index];
			if (std::isspace(static_cast<unsigned char>(character)) || character == '\\')
			{
				index++;
			}
			else if (IsIdentifierStart(character))
			{
				const size_t start = index;
				while (index < code.size() && IsIdentifierPart(code[index]))
					index++;
				out.push_back({ std::string(code.substr(start, index - start)), true });
			}
			else if (std::isdigit(static_cast<unsigned char>(character)))
			{
				const size_t start = index;
				while (index < code.size() && (IsIdentifierPart(code[index]) || code[index] == '.'))
					index++;
				out.push_back({ std::string(code.substr(start, index - start)), false });
			}
			else if ((character == ':' || character == '-') && index + 1 < code.size() && (code[index + 1] == ':' || (character == '-' && code[index + 1] == '>')))
			{
				out.push_back({ std::string(code.substr(index, 2)), false });
				index += 2;
			}
			else
			{
				out.push_back({ std::string(1, character), false });
				index++;
			}
		}
	}

	SourceTokens Tokenize(std::string_view source)
	{
		// First pass: blank out comments and literals (keeping line structure), collect directives.
		std::string code;
		code.reserve(source.size());
		SourceTokens result;
		bool lineStart = true;
		size_t index = 0;
		while (index < source.size())
		{
			const char character = source[index];
			const char next = index + 1 < source.size() ? source[index + 1] : '\0';
			if (character == '/' && next == '/')
			{
				while (index < source.size() && source[index] != '\n')
					index++;
			}
			else if (character == '/' && next == '*')
			{
				const size_t end = source.find("*/", index + 2);
				index = end == std::string_view::npos ? source.size() : end + 2;
				code += ' ';
			}
			else if (character == '"' || character == '\'')
			{
				index++;
				while (index < source.size() && source[index] != character)
					index += source[index] == '\\' ? 2 : 1;
				index++;
				code += " 0 "; // A literal is an expression token
				lineStart = false;
			}
			else if (character == '#' && lineStart)
			{
				// A directive runs to the end of the line, including backslash continuations.
				std::string directive;
				while (index < source.size() && source[index] != '\n')
				{
					if (source[index] == '\\' && index + 1 < source.size() && source[index + 1] == '\n')
					{
						directive += ' ';
						index += 2;
						continue;
					}
					if (source[index] == '/' && index + 1 < source.size() && source[index + 1] == '/')
					{
						while (index < source.size() && source[index] != '\n')
							index++;
						break;
					}
					directive += source[index++];
				}
				if (auto macro = ParseDirective(directive))
					result.Macros[macro->first] = std::move(macro->second);
			}
			else
			{
				if (character == '\n')
					lineStart = true;
				else if (!std::isspace(static_cast<unsigned char>(character)))
					lineStart = false;
				code += character;
				index++;
			}
		}
		TokenizeCode(code, result.Tokens);
		return result;
	}

	struct SDKMember
	{
		std::string Class;
		std::string Name;
		bool Static = false;
	};

	enum class ScopeKind
	{
		Namespace,
		Class,
		Other
	};

	struct CodeScope
	{
		ScopeKind Kind = ScopeKind::Other;
		std::string Name;
		bool Public = false;
	};

	// The scope a "{" opens, from the tokens of the statement before it.
	CodeScope ClassifyScope(const std::vector<Token>& header)
	{
		int32_t angleDepth = 0;
		for (size_t index = 0; index < header.size(); index++)
		{
			const std::string& text = header[index].Text;
			if (text == "(")
				return { ScopeKind::Other };
			if (text == "<")
				angleDepth++;
			else if (text == ">")
				angleDepth--;
			else if (text == "namespace")
				return { ScopeKind::Namespace, index + 1 < header.size() && header[index + 1].Identifier ? header[index + 1].Text : std::string() };
			else if (text == "enum")
				return { ScopeKind::Other };
			else if (angleDepth == 0 && (text == "class" || text == "struct") && index + 1 < header.size() && header[index + 1].Identifier)
				return { ScopeKind::Class, header[index + 1].Text, text == "struct" };
		}
		return { ScopeKind::Other };
	}

	bool IsInDetail(const std::vector<CodeScope>& scopes)
	{
		return std::any_of(scopes.begin(), scopes.end(), [](const CodeScope& scope) { return scope.Kind == ScopeKind::Namespace && scope.Name == "Detail"; });
	}

	// Public member functions of SDK classes (and free functions) outside namespace Detail.
	void CollectMembers(const std::vector<Token>& tokens, std::vector<SDKMember>& out)
	{
		static const std::set<std::string_view> c_Keywords = { "static_assert", "decltype", "sizeof", "alignof", "alignas", "noexcept", "requires" };

		std::vector<CodeScope> scopes;
		std::vector<Token> statement;
		bool declaratorDone = false;
		bool afterEquals = false;
		bool isStatic = false;
		int32_t parenDepth = 0;
		int32_t angleDepth = 0;
		auto resetStatement = [&]()
		{
			statement.clear();
			declaratorDone = false;
			afterEquals = false;
			isStatic = false;
			parenDepth = 0;
			angleDepth = 0;
		};

		for (size_t index = 0; index < tokens.size(); index++)
		{
			const Token& token = tokens[index];
			if (token.Text == "{")
			{
				scopes.push_back(ClassifyScope(statement));
				resetStatement();
				continue;
			}
			if (token.Text == "}")
			{
				if (!scopes.empty())
					scopes.pop_back();
				resetStatement();
				continue;
			}
			if (token.Text == ";")
			{
				resetStatement();
				continue;
			}

			CodeScope* scope = scopes.empty() ? nullptr : &scopes.back();
			if (scope && scope->Kind == ScopeKind::Class && index + 1 < tokens.size() && tokens[index + 1].Text == ":"
				&& (token.Text == "public" || token.Text == "private" || token.Text == "protected"))
			{
				scope->Public = token.Text == "public";
				index++;
				resetStatement();
				continue;
			}

			statement.push_back(token);
			// Only declarations directly in a public class section or in a namespace (free functions) are of interest.
			const bool inClass = scope && scope->Kind == ScopeKind::Class && scope->Public;
			const bool inNamespace = scope && scope->Kind == ScopeKind::Namespace && scope->Name == "Strata";
			if (!(inClass || inNamespace) || IsInDetail(scopes))
				continue;

			if (token.Text == "(")
				parenDepth++;
			else if (token.Text == ")")
				parenDepth--;
			else if (parenDepth == 0 && token.Text == "<")
				angleDepth++;
			else if (parenDepth == 0 && token.Text == ">")
				angleDepth--;
			else if (parenDepth == 0 && token.Text == "=")
				afterEquals = true;
			else if (token.Text == "static")
				isStatic = true;

			if (!token.Identifier || declaratorDone || afterEquals || parenDepth != 0 || angleDepth != 0)
				continue;
			if (token.Text == "operator")
			{
				declaratorDone = true;
				continue;
			}
			if (index + 1 >= tokens.size() || tokens[index + 1].Text != "(" || c_Keywords.contains(token.Text))
				continue;

			declaratorDone = true;
			const std::string& previous = statement.size() >= 2 ? statement[statement.size() - 2].Text : std::string();
			if (previous == "~" || previous == "::" || (inClass && token.Text == scope->Name))
				continue; // Destructors, out-of-class definitions and constructors
			out.push_back({ inClass ? scope->Name : std::string(), token.Text, isStatic });
		}
	}

	struct SDKSurface
	{
		std::vector<SDKMember> Members;
		std::set<std::string> Classes;
		std::map<std::string, std::vector<Token>> Macros; // Every macro of the SDK headers
		std::set<std::string> PublicMacros;
	};

	std::vector<std::filesystem::path> ListFiles(const std::filesystem::path& directory)
	{
		std::vector<std::filesystem::path> files;
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
		{
			const std::string extension = FileSystem::ToUTF8(it->path().extension());
			if (it->is_regular_file() && (extension == ".h" || extension == ".hpp" || extension == ".cpp"))
				files.push_back(it->path());
		}
		std::sort(files.begin(), files.end());
		return files;
	}

	SDKSurface ReadSDKSurface()
	{
		SDKSurface surface;
		const std::filesystem::path sdkDirectory = FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "StrataScriptCore" / "Include" / "StrataScript";
		const std::vector<std::filesystem::path> headers = ListFiles(sdkDirectory);
		REQUIRE(headers.size() >= 8);
		for (const std::filesystem::path& header : headers)
		{
			// The C ABI is covered by the host function call counters instead.
			if (header.filename() == "ScriptABI.h")
				continue;
			const std::optional<std::string> text = FileSystem::ReadText(header);
			REQUIRE(text.has_value());
			SourceTokens tokens = Tokenize(*text);
			CollectMembers(tokens.Tokens, surface.Members);
			for (auto& [name, body] : tokens.Macros)
			{
				if (name.rfind("ST_SCRIPT_", 0) == 0 && name.rfind("ST_SCRIPT_DETAIL_", 0) != 0)
					surface.PublicMacros.insert(name);
				surface.Macros[name] = std::move(body);
			}
		}
		for (const SDKMember& member : surface.Members)
		{
			if (!member.Class.empty())
				surface.Classes.insert(member.Class);
		}
		return surface;
	}

	struct Usage
	{
		std::set<std::string> Identifiers;
		std::set<std::string> Qualified; // "A::B" for every "A :: B" token pair
	};

	void AddUsage(const std::vector<Token>& tokens, const std::map<std::string, std::vector<Token>>& macros, Usage& usage, std::set<std::string>& expanded)
	{
		for (size_t index = 0; index < tokens.size(); index++)
		{
			const Token& token = tokens[index];
			if (!token.Identifier)
				continue;
			usage.Identifiers.insert(token.Text);
			if (index + 2 < tokens.size() && tokens[index + 1].Text == "::" && tokens[index + 2].Identifier)
				usage.Qualified.insert(token.Text + "::" + tokens[index + 2].Text);

			// SDK macros used by the scripts count with everything they expand to.
			auto macro = macros.find(token.Text);
			if (macro != macros.end() && expanded.insert(token.Text).second)
				AddUsage(macro->second, macros, usage, expanded);
		}
	}

	Usage ReadFeatureScriptUsage(const SDKSurface& surface)
	{
		Usage usage;
		std::set<std::string> expanded;
		const std::vector<std::filesystem::path> sources = ListFiles(GetFeatureProjectSourceDirectory() / "Scripts");
		REQUIRE_FALSE(sources.empty());
		for (const std::filesystem::path& source : sources)
		{
			const std::optional<std::string> text = FileSystem::ReadText(source);
			REQUIRE(text.has_value());
			AddUsage(Tokenize(*text).Tokens, surface.Macros, usage, expanded);
		}
		return usage;
	}

	bool HasMember(const SDKSurface& surface, std::string_view className, std::string_view name)
	{
		return std::any_of(surface.Members.begin(), surface.Members.end(), [&](const SDKMember& member) { return member.Class == className && member.Name == name; });
	}

}

TEST_SUITE("FeatureTest")
{
	TEST_CASE("The SDK reader finds the script SDK's public API")
	{
		const SDKSurface surface = ReadSDKSurface();
		// Guards against a parser regression that would make the coverage check below vacuous.
		CHECK(surface.Members.size() >= 80);
		for (const char* className : { "Entity", "Component", "TransformComponent", "AssetHandle", "Assets", "Scene", "Script", "ScriptClassBuilder",
				 "Input", "Log", "Time" })
		{
			INFO("Class ", className);
			CHECK(surface.Classes.contains(className));
		}
		CHECK(HasMember(surface, "Entity", "GetProperty"));
		CHECK(HasMember(surface, "Scene", "Instantiate"));
		CHECK(HasMember(surface, "Script", "OnReload"));
		CHECK(HasMember(surface, "ScriptClassBuilder", "Field"));
		CHECK(HasMember(surface, "Input", "GetScrollDelta"));
		CHECK(HasMember(surface, "TransformComponent", "SetLocalTransform"));
		// Constructors, operators, private members and Detail are not part of the surface.
		CHECK_FALSE(HasMember(surface, "Entity", "Entity"));
		CHECK_FALSE(HasMember(surface, "TransformComponent", "GetLocal"));
		CHECK_FALSE(HasMember(surface, "Log", "Write"));
		CHECK_FALSE(surface.Classes.contains("ClassRecord"));
		CHECK(surface.PublicMacros == std::set<std::string> { "ST_SCRIPT_CLASS", "ST_SCRIPT_FIELD" });
	}

	TEST_CASE("The feature scripts use every function of the script SDK")
	{
		const SDKSurface surface = ReadSDKSurface();
		const Usage usage = ReadFeatureScriptUsage(surface);

		// Classes whose public functions are all static are used through "Class::Function".
		std::set<std::string> staticClasses = surface.Classes;
		for (const SDKMember& member : surface.Members)
		{
			if (!member.Static)
				staticClasses.erase(member.Class);
		}

		for (const std::string& className : surface.Classes)
		{
			INFO("SDK class ", className, " is not used by the feature scripts (StrataTests/FeatureTest/Scripts)");
			CHECK(usage.Identifiers.contains(className));
		}
		for (const SDKMember& member : surface.Members)
		{
			const std::string qualifiedName = member.Class.empty() ? member.Name : member.Class + "::" + member.Name;
			INFO("SDK function ", qualifiedName, " is not used by the feature scripts (StrataTests/FeatureTest/Scripts)");
			if (staticClasses.contains(member.Class))
				CHECK(usage.Qualified.contains(qualifiedName));
			else
				CHECK(usage.Identifiers.contains(member.Name));
		}
		for (const std::string& macro : surface.PublicMacros)
		{
			INFO("SDK macro ", macro, " is not used by the feature scripts (StrataTests/FeatureTest/Scripts)");
			CHECK(usage.Identifiers.contains(macro));
		}
	}
}
