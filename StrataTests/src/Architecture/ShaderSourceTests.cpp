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
#include <utility>
#include <vector>

using namespace Strata;

// Fragment shaders take their derivatives (fwidth, ddx/ddy, Sample, SampleBias and SampleCmp without an explicit level,
// helpers calling those) before any discard or clip. A discard that differs per pixel ends its invocation (OpKill), which
// leaves the derivatives of the rest of its 2x2 quad undefined: some drivers (Mesa on Linux) really drop the lane, and edges
// come out with garbage widths and wrong mip levels, while others (MoltenVK) keep it, so no image on the Mac shows the
// mistake. The check is textual: after the first discard or clip in a shader's source, nothing may take derivatives.
//
// The shaders are HLSL-syntax Slang (Docs/DeveloperGuide.md, "Shaders"): Slang's own language (modules, generics' keywords,
// interfaces, target switches outside Include/Common.slang) and GLSL left over from the conversion are refused, so that the
// sources stay plain HLSL for other platforms' compilers.
namespace
{

	// The built-in functions that take screen derivatives (sampling without an explicit level or gradients; methods such as
	// u_Texture.Sample(...) read as calls of Sample).
	constexpr std::array<std::string_view, 12> c_DerivativeBuiltins = {
		"ddx", "ddy", "ddx_fine", "ddy_fine", "ddx_coarse", "ddy_coarse", "fwidth",
		"Sample", "SampleBias", "SampleCmp", "CalculateLevelOfDetail", "CalculateLevelOfDetailUnclamped",
	};

	// Slang's target switches and inline SPIR-V: only Include/Common.slang may use them, each with an HLSL form.
	constexpr std::array<std::string_view, 3> c_TargetSpecific = { "__target_switch", "spirv_asm", "__intrinsic_asm" };

	// Slang's language beyond HLSL: modules, interfaces, extensions, generics' keywords.
	constexpr std::array<std::string_view, 10> c_SlangOnly = {
		"import", "__import", "module", "implementing", "__include", "interface", "extension", "__generic", "associatedtype",
		"__exported",
	};

	// GLSL's names, which Slang accepts silently or the conversion should have replaced.
	constexpr std::array<std::string_view, 16> c_GLSLIdentifiers = {
		"layout", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4", "bvec2", "bvec3", "bvec4",
		"mat2", "mat3", "mat4",
	};
	constexpr std::array<std::string_view, 7> c_GLSLCalls = { "texture", "texelFetch", "textureLod", "mix", "fract", "dFdx", "dFdy" };

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
				// Statements look like definitions here (if (...) { ... }): only functions count. Entry points are no helpers:
				// a converted fragment shader's main calls its st_Main, whose own calls are checked where they are.
				if (!token.Call || c_Statements.end() != std::find(c_Statements.begin(), c_Statements.end(), token.Name))
					continue;
				if (token.Name == "main" || token.Name == "st_Main")
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

	// "<name>:<line> takes derivatives (<call>) after the discard at line <n>" for each such call (a clip counts as a discard).
	std::vector<std::string> FindDerivativesAfterDiscard(std::string_view name, std::string_view source, const std::set<std::string>& derivativeFunctions)
	{
		std::vector<std::string> problems;
		const std::string stripped = StripComments(source);
		std::optional<size_t> discardLine;
		for (const Token& token : Tokenize(stripped))
		{
			if (token.Name == "discard" || (token.Name == "clip" && token.Call))
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

	template<size_t Count>
	bool Contains(const std::array<std::string_view, Count>& names, std::string_view name)
	{
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	// "<name>:<line> <what>" for each token of the source (comments stripped) that is not plain HLSL.
	std::vector<std::string> FindSyntaxProblems(std::string_view name, std::string_view source)
	{
		std::vector<std::string> problems;
		const std::string stripped = StripComments(source);
		for (const Token& token : Tokenize(stripped))
		{
			const size_t line = LineOf(stripped, token.Offset);
			if (Contains(c_TargetSpecific, token.Name) && name != "Include/Common.slang")
				problems.push_back(fmt::format("{}:{} uses {}, which only Include/Common.slang may (with an HLSL form)", name, line, token.Name));
			else if (Contains(c_SlangOnly, token.Name))
				problems.push_back(fmt::format("{}:{} uses {}: Slang's language, not HLSL", name, line, token.Name));
			else if (Contains(c_GLSLIdentifiers, token.Name) || token.Name.starts_with("gl_"))
				problems.push_back(fmt::format("{}:{} uses GLSL's {}", name, line, token.Name));
			else if (token.Call && Contains(c_GLSLCalls, token.Name))
				problems.push_back(fmt::format("{}:{} calls GLSL's {}", name, line, token.Name));
		}
		return problems;
	}

	// The names (relative, forward slashes) and texts of every file under Strata/shaders, sorted by name.
	std::vector<std::pair<std::string, std::string>> ReadShaderSources()
	{
		const std::filesystem::path root = FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "Strata" / "shaders";
		REQUIRE(std::filesystem::is_directory(root));
		std::vector<std::pair<std::string, std::string>> files;
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator it(root, error), end; it != end && !error; it.increment(error))
		{
			if (!it->is_regular_file())
				continue;
			const std::optional<std::string> source = FileSystem::ReadText(it->path());
			REQUIRE(source);
			files.emplace_back(FileSystem::ToUTF8(it->path().lexically_relative(root).generic_string()), *source);
		}
		REQUIRE_FALSE(error);
		std::sort(files.begin(), files.end());
		return files;
	}

	bool EndsWith(std::string_view text, std::string_view suffix)
	{
		return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
	}

}

TEST_SUITE("Architecture.Shaders")
{
	TEST_CASE("The derivative check finds derivatives after a discard, through helpers, and ignores comments")
	{
		const std::string helpers =
			"float Coverage(float d)\n{\n\treturn abs(ddx(d)) + abs(ddy(d));\n}\n"
			"float Wrapped(float d)\n{\n\tif (d > 0.0)\n\t{\n\t\treturn Coverage(d) * 2.0;\n\t}\n\treturn 0.0;\n}\n";
		const std::set<std::string> functions = FindDerivativeFunctions({ StripComments(helpers) });
		CHECK(functions.contains("Coverage"));
		CHECK(functions.contains("Wrapped"));
		CHECK(functions.contains("Sample"));
		CHECK_FALSE(functions.contains("SampleLevel"));
		CHECK_FALSE(functions.contains("if"));

		const std::string bad =
			"void st_Main()\n"
			"{\n"
			"\tif (v_Clip <= 0.0)\n"
			"\t\tdiscard;\n"
			"\tfloat a = Wrapped(v_D);\n"
			"\tfloat4 t = u_Tex.Sample(u_Sampler, v_UV);\n"
			"\tfloat4 l = u_Tex.SampleLevel(u_Sampler, v_UV, 0.0);\n"
			"}\n";
		const std::vector<std::string> problems = FindDerivativesAfterDiscard("Bad.frag", bad, functions);
		REQUIRE(problems.size() == 2);
		CHECK(problems[0] == "Bad.frag:5 takes derivatives (Wrapped) after the discard at line 4");
		CHECK(problems[1] == "Bad.frag:6 takes derivatives (Sample) after the discard at line 4");

		// HLSL's conditional discard.
		const std::string clipped =
			"void st_Main()\n"
			"{\n"
			"\tclip(v_Alpha - 0.5);\n"
			"\tfloat4 t = u_Tex.Sample(u_Sampler, v_UV);\n"
			"}\n";
		const std::vector<std::string> clipProblems = FindDerivativesAfterDiscard("Clip.frag", clipped, functions);
		REQUIRE(clipProblems.size() == 1);
		CHECK(clipProblems[0] == "Clip.frag:4 takes derivatives (Sample) after the discard at line 3");

		const std::string good =
			"Texture2D<float4> u_Textures[] : register(t0, space1);\n"
			"void st_Main()\n"
			"{\n"
			"\tfloat a = fwidth(v_D) * 1e5; // discard\n"
			"\t/* discard; clip(a); fwidth(x) */\n"
			"\tif (a <= 0.0)\n"
			"\t\tdiscard;\n"
			"\to_Color = float4(u_Image.Load(int3(0, 0, 0)).rgb, a);\n"
			"}\n";
		CHECK(FindDerivativesAfterDiscard("Good.frag", good, functions).empty());
	}

	TEST_CASE("Fragment shaders take every derivative before their first discard")
	{
		// The functions come from the includes and the fragment shaders only (as they did in GLSL): a compute or vertex
		// shader's st_Main is no helper a fragment shader calls.
		std::vector<std::pair<std::string, std::string>> fragments;
		std::vector<std::string> sources;
		for (const std::pair<std::string, std::string>& file : ReadShaderSources())
		{
			const bool fragment = EndsWith(file.first, ".frag.slang");
			const bool stage = fragment || EndsWith(file.first, ".vert.slang") || EndsWith(file.first, ".comp.slang") || EndsWith(file.first, ".geom.slang");
			if (stage && !fragment)
				continue;
			sources.push_back(StripComments(file.second));
			if (fragment)
				fragments.push_back(file);
		}
		REQUIRE(fragments.size() > 5);
		// Helpers in the includes that take derivatives count as derivatives (SDFCoverage).
		const std::set<std::string> functions = FindDerivativeFunctions(sources);
		CHECK(functions.contains("SDFCoverage"));
		for (const std::pair<std::string, std::string>& file : fragments)
		{
			CAPTURE(file.first);
			for (const std::string& problem : FindDerivativesAfterDiscard(file.first, file.second, functions))
				FAIL_CHECK(problem);
		}
	}

	TEST_CASE("Strata/shaders holds Slang sources only")
	{
		const std::vector<std::pair<std::string, std::string>> files = ReadShaderSources();
		REQUIRE_FALSE(files.empty());
		for (const std::pair<std::string, std::string>& file : files)
		{
			CAPTURE(file.first);
			CHECK(EndsWith(file.first, ".slang"));
		}
	}

	TEST_CASE("The syntax check finds Slang's own language and GLSL, outside comments")
	{
		const std::string crafted =
			"import Lighting;\n"
			"interface ILight { float3 Evaluate(); };\n"
			"__generic<T> T Twice(T value) { return value; }\n"
			"uint Pack(float2 v) { __target_switch { case spirv: return spirv_asm { OpNop }; default: return 0u; } }\n"
			"layout(location = 0) in vec3 a_Position;\n"
			"float4 Read() { return mix(texture(u_Tex, gl_FragCoord.xy), float4(0.0), 0.5); }\n"
			"// import, vec3, texture(x), mix(a, b, c) and gl_Position in a comment\n"
			"float4 Fine(Texture2D<float4> texture) { return texture.Load(int3(0, 0, 0)) + lerp(1.0, 2.0, 0.5); }\n";
		const std::vector<std::string> expected = {
			"Crafted.frag.slang:1 uses import: Slang's language, not HLSL",
			"Crafted.frag.slang:2 uses interface: Slang's language, not HLSL",
			"Crafted.frag.slang:3 uses __generic: Slang's language, not HLSL",
			"Crafted.frag.slang:4 uses __target_switch, which only Include/Common.slang may (with an HLSL form)",
			"Crafted.frag.slang:4 uses spirv_asm, which only Include/Common.slang may (with an HLSL form)",
			"Crafted.frag.slang:5 uses GLSL's layout",
			"Crafted.frag.slang:5 uses GLSL's vec3",
			"Crafted.frag.slang:6 calls GLSL's mix",
			"Crafted.frag.slang:6 calls GLSL's texture",
			"Crafted.frag.slang:6 uses GLSL's gl_FragCoord",
		};
		CHECK(FindSyntaxProblems("Crafted.frag.slang", crafted) == expected);
		CHECK(FindSyntaxProblems("Include/Common.slang", "uint Pack(float2 v) { __target_switch { default: return 0u; } }\n").empty());
	}

	TEST_CASE("Shaders use HLSL syntax")
	{
		const std::vector<std::pair<std::string, std::string>> files = ReadShaderSources();
		REQUIRE(files.size() > 50);
		for (const std::pair<std::string, std::string>& file : files)
		{
			CAPTURE(file.first);
			for (const std::string& problem : FindSyntaxProblems(file.first, file.second))
				FAIL_CHECK(problem);
		}
	}
}
