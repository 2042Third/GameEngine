#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

// Fragment shaders take their derivatives (fwidth, dFdx, implicit-level texture sampling, helpers calling those) before any
// discard. A discard that differs per pixel ends its invocation (OpKill), which leaves the derivatives of the rest of its
// 2x2 quad undefined: some drivers (Mesa on Linux) really drop the lane, and edges come out with garbage widths and wrong
// mip levels, while others (MoltenVK) keep it, so no image on the Mac shows the mistake. The check is textual: after the
// first discard in a shader's source, nothing may take derivatives.
namespace
{

	// The built-in functions that take screen derivatives (texture* without an explicit level or gradients).
	constexpr std::array<std::string_view, 13> c_DerivativeBuiltins = {
		"fwidth", "fwidthFine", "fwidthCoarse", "dFdx", "dFdy", "dFdxFine", "dFdyFine", "dFdxCoarse", "dFdyCoarse",
		"texture", "textureOffset", "textureProj", "textureProjOffset",
	};

	// Keywords followed by parentheses and a block, which are no function definitions.
	constexpr std::array<std::string_view, 4> c_Statements = { "if", "for", "while", "switch" };

	bool IsIdentifierCharacter(char character)
	{
		return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
	}

	// The source without its comments, newlines kept (line numbers stay).
	std::string StripComments(std::string_view source)
	{
		std::string result;
		result.reserve(source.size());
		for (size_t index = 0; index < source.size(); index++)
		{
			if (source.compare(index, 2, "//") == 0)
			{
				while (index < source.size() && source[index] != '\n')
					index++;
				if (index < source.size())
					result.push_back('\n');
				continue;
			}
			if (source.compare(index, 2, "/*") == 0)
			{
				const size_t end = source.find("*/", index + 2);
				const size_t stop = end == std::string_view::npos ? source.size() : end + 2;
				for (size_t skipped = index; skipped < stop; skipped++)
				{
					if (source[skipped] == '\n')
						result.push_back('\n');
				}
				index = stop - 1;
				continue;
			}
			result.push_back(source[index]);
		}
		return result;
	}

	struct Token
	{
		std::string Name;
		size_t Offset = 0;
		bool Call = false; // Followed by '('
	};

	std::vector<Token> Tokenize(std::string_view source)
	{
		std::vector<Token> tokens;
		size_t index = 0;
		while (index < source.size())
		{
			const char character = source[index];
			if (std::isdigit(static_cast<unsigned char>(character)) != 0)
			{
				// A number whole (1e5, 0xFFu), so that its suffix is no identifier.
				while (index < source.size() && IsIdentifierCharacter(source[index]))
					index++;
				continue;
			}
			if (!IsIdentifierCharacter(character))
			{
				index++;
				continue;
			}
			const size_t start = index;
			while (index < source.size() && IsIdentifierCharacter(source[index]))
				index++;
			size_t next = index;
			while (next < source.size() && std::isspace(static_cast<unsigned char>(source[next])) != 0)
				next++;
			tokens.push_back({ std::string(source.substr(start, index - start)), start, next < source.size() && source[next] == '(' });
		}
		return tokens;
	}

	size_t LineOf(std::string_view source, size_t offset)
	{
		return 1 + static_cast<size_t>(std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
	}

	// Functions defined in the sources whose bodies take derivatives, directly or through other such functions.
	std::set<std::string> FindDerivativeFunctions(const std::vector<std::string>& sources)
	{
		std::set<std::string> functions(c_DerivativeBuiltins.begin(), c_DerivativeBuiltins.end());
		struct Definition
		{
			std::string Name;
			std::vector<std::string> Calls;
		};
		std::vector<Definition> definitions;
		for (const std::string& source : sources)
		{
			const std::vector<Token> tokens = Tokenize(source);
			for (const Token& token : tokens)
			{
				// Statements look like definitions here (if (...) { ... }): only functions count.
				if (!token.Call || c_Statements.end() != std::find(c_Statements.begin(), c_Statements.end(), token.Name))
					continue;
				// A definition: the call's closing parenthesis is followed by a body.
				const size_t close = source.find(')', token.Offset);
				if (close == std::string::npos)
					continue;
				size_t open = close + 1;
				while (open < source.size() && std::isspace(static_cast<unsigned char>(source[open])) != 0)
					open++;
				if (open >= source.size() || source[open] != '{' || source.find('(', token.Offset + token.Name.size() + 1) < close)
					continue;
				int depth = 0;
				size_t end = open;
				for (; end < source.size(); end++)
				{
					if (source[end] == '{')
						depth++;
					else if (source[end] == '}' && --depth == 0)
						break;
				}
				Definition& definition = definitions.emplace_back();
				definition.Name = token.Name;
				for (const Token& inner : Tokenize(std::string_view(source).substr(open, end - open)))
				{
					if (inner.Call)
						definition.Calls.push_back(inner.Name);
				}
			}
		}
		for (bool grew = true; grew;)
		{
			grew = false;
			for (const Definition& definition : definitions)
			{
				if (functions.contains(definition.Name))
					continue;
				const bool takesDerivatives = std::any_of(definition.Calls.begin(), definition.Calls.end(), [&](const std::string& call) { return functions.contains(call); });
				if (takesDerivatives)
				{
					functions.insert(definition.Name);
					grew = true;
				}
			}
		}
		return functions;
	}

	// "<name>:<line> takes derivatives (<call>) after the discard at line <n>" for each such call.
	std::vector<std::string> FindDerivativesAfterDiscard(std::string_view name, std::string_view source, const std::set<std::string>& derivativeFunctions)
	{
		std::vector<std::string> problems;
		const std::string stripped = StripComments(source);
		std::optional<size_t> discardLine;
		for (const Token& token : Tokenize(stripped))
		{
			if (token.Name == "discard")
			{
				if (!discardLine)
					discardLine = LineOf(stripped, token.Offset);
				continue;
			}
			if (discardLine && token.Call && derivativeFunctions.contains(token.Name))
				problems.push_back(fmt::format("{}:{} takes derivatives ({}) after the discard at line {}", name, LineOf(stripped, token.Offset), token.Name, *discardLine));
		}
		return problems;
	}

}

TEST_SUITE("Architecture.Shaders")
{
	TEST_CASE("The derivative check finds derivatives after a discard, through helpers, and ignores comments")
	{
		const std::string helpers =
			"float Coverage(float d)\n{\n\treturn fwidth(d);\n}\n"
			"float Wrapped(float d)\n{\n\tif (d > 0.0)\n\t{\n\t\treturn Coverage(d) * 2.0;\n\t}\n\treturn 0.0;\n}\n";
		const std::set<std::string> functions = FindDerivativeFunctions({ StripComments(helpers) });
		CHECK(functions.contains("Coverage"));
		CHECK(functions.contains("Wrapped"));
		CHECK(functions.contains("texture"));
		CHECK_FALSE(functions.contains("textureLod"));
		CHECK_FALSE(functions.contains("if"));

		const std::string bad =
			"void main()\n"
			"{\n"
			"\tif (v_Clip <= 0.0)\n"
			"\t\tdiscard;\n"
			"\tfloat a = Wrapped(v_D);\n"
			"\tvec4 t = texture(sampler2D(u_Tex, u_Sampler), v_UV);\n"
			"\tvec4 l = textureLod(sampler2D(u_Tex, u_Sampler), v_UV, 0.0);\n"
			"}\n";
		const std::vector<std::string> problems = FindDerivativesAfterDiscard("Bad.frag", bad, functions);
		REQUIRE(problems.size() == 2);
		CHECK(problems[0] == "Bad.frag:5 takes derivatives (Wrapped) after the discard at line 4");
		CHECK(problems[1] == "Bad.frag:6 takes derivatives (texture) after the discard at line 4");

		const std::string good =
			"layout(binding = 0) uniform texture2D u_Textures[];\n"
			"void main()\n"
			"{\n"
			"\tfloat a = fwidth(v_D) * 1e5; // discard\n"
			"\t/* discard; fwidth(x) */\n"
			"\tif (a <= 0.0)\n"
			"\t\tdiscard;\n"
			"\to_Color = vec4(texelFetch(u_Image, ivec2(0), 0).rgb, a);\n"
			"}\n";
		CHECK(FindDerivativesAfterDiscard("Good.frag", good, functions).empty());
	}

	TEST_CASE("Fragment shaders take every derivative before their first discard")
	{
		const std::filesystem::path root = FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "Strata" / "shaders";
		REQUIRE(std::filesystem::is_directory(root));
		std::vector<std::filesystem::path> fragments;
		std::vector<std::string> sources;
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator it(root, error), end; it != end && !error; it.increment(error))
		{
			const std::filesystem::path& path = it->path();
			if (!it->is_regular_file())
				continue;
			const std::string extension = FileSystem::ToUTF8(path.extension());
			if (extension != ".frag" && extension != ".glsl")
				continue;
			const std::optional<std::string> source = FileSystem::ReadText(path);
			REQUIRE(source);
			sources.push_back(StripComments(*source));
			if (extension == ".frag")
				fragments.push_back(path);
		}
		REQUIRE_FALSE(error);
		REQUIRE(fragments.size() > 5);
		// Helpers in the includes that take derivatives count as derivatives (SDFCoverage).
		const std::set<std::string> functions = FindDerivativeFunctions(sources);
		CHECK(functions.contains("SDFCoverage"));
		for (const std::filesystem::path& path : fragments)
		{
			const std::optional<std::string> source = FileSystem::ReadText(path);
			REQUIRE(source);
			const std::string name = FileSystem::ToUTF8(path.lexically_relative(root));
			CAPTURE(name);
			for (const std::string& problem : FindDerivativesAfterDiscard(name, *source, functions))
				FAIL_CHECK(problem);
		}
	}
}
