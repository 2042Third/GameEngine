#include <doctest/doctest.h>

#include "Architecture/LayeringAnalyzer.h"
#include "Strata/Core/FileSystem.h"

#include <spdlog/fmt/fmt.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

// The precompiled headers hold only the standard library, vendor headers and the Core basics: every object of their
// targets rebuilds when one of them changes, so a header that changes often must never be in one.
namespace
{

	// The engine headers a precompiled header may hold: the Core basics, which change rarely, and the engine's own
	// precompiled header, which the editor's and the tests' start with.
	constexpr std::array<std::string_view, 6> c_AllowedEngineHeaders = {
		"stpch.h",
		"Strata/Core/PlatformDetection.h",
		"Strata/Core/Base.h",
		"Strata/Core/Log.h",
		"Strata/Core/Assert.h",
		"Strata/Core/Profiling.h",
	};

	bool IsAllowedEngineHeader(std::string_view path)
	{
		for (std::string_view allowed : c_AllowedEngineHeaders)
		{
			if (path == allowed)
				return true;
		}
		return false;
	}

	// The includes of a precompiled header that it may not hold: quoted includes other than the allowed engine headers,
	// and engine headers included with angle brackets (Strata/src is a public include directory).
	std::vector<std::string> FindForbiddenIncludes(std::string_view name, std::string_view source)
	{
		std::vector<std::string> problems;
		for (const Tests::Layering::IncludeDirective& include : Tests::Layering::ParseIncludes(source))
		{
			const bool engineHeader = !include.Angled || include.Path.starts_with("Strata/") || include.Path.starts_with("Platform/");
			if (engineHeader && !IsAllowedEngineHeader(include.Path))
				problems.push_back(fmt::format("{}:{} includes {}", name, include.Line, include.Path));
		}
		return problems;
	}

}

TEST_SUITE("Architecture.PrecompiledHeaders")
{
	TEST_CASE("A precompiled header holding an engine header that is not a Core basic is refused")
	{
		CHECK(FindForbiddenIncludes("Pch.h", "#include \"stpch.h\"\n#include <doctest/doctest.h>\n#include <vector>\n").empty());
		CHECK(FindForbiddenIncludes("Pch.h", "#include \"Strata/Core/Log.h\"\n#include <entt/entt.hpp>\n").empty());

		const std::vector<std::string> problems = FindForbiddenIncludes("Pch.h",
			"#include \"stpch.h\"\n"
			"#include \"Strata/Scene/Scene.h\"\n"
			"#include <Strata/Renderer/Renderer.h>\n"
			"#include \"Editor/EditorContext.h\"\n"
			"// #include \"Strata/Scene/Entity.h\"\n");
		REQUIRE(problems.size() == 3);
		CHECK(problems[0] == "Pch.h:2 includes Strata/Scene/Scene.h");
		CHECK(problems[1] == "Pch.h:3 includes Strata/Renderer/Renderer.h");
		CHECK(problems[2] == "Pch.h:4 includes Editor/EditorContext.h");
	}

	TEST_CASE("The precompiled headers hold only the standard library, vendor headers and the Core basics")
	{
		const std::filesystem::path root = FileSystem::FromUTF8(STRATA_SOURCE_DIR);
		const std::array<std::string, 3> headers = { "Strata/src/stpch.h", "StrataEditor/src/EditorPch.h", "StrataTests/src/TestsPch.h" };
		for (const std::string& header : headers)
		{
			CAPTURE(header);
			const std::optional<std::string> source = FileSystem::ReadText(root / FileSystem::FromUTF8(header));
			REQUIRE(source);
			CHECK_FALSE(Tests::Layering::ParseIncludes(*source).empty());
			const std::vector<std::string> problems = FindForbiddenIncludes(header, *source);
			for (const std::string& problem : problems)
				FAIL_CHECK(problem);
		}
	}
}
