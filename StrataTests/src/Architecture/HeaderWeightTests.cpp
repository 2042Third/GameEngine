#include <doctest/doctest.h>

#include "Architecture/LayeringAnalyzer.h"
#include "Strata/Core/FileSystem.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace Strata;

// Headers stay light: what a header includes, every file that includes it parses (without precompiled headers: the lint,
// and builds with a compiler cache).
namespace
{

	// The engine headers that need nlohmann::json complete, and why. Every other engine header that names JSON in
	// declarations only includes <nlohmann/json_fwd.hpp>.
	struct FullJsonHeader
	{
		std::string_view Path; // Relative to Strata/src
		std::string_view Reason;
	};

	constexpr std::array<FullJsonHeader, 9> c_FullJsonHeaders = { {
		{ "stpch.h", "the precompiled header: each library parses it once" },
		{ "Strata/Asset/AssetImporter.h", "ImportContext::Settings is a JSON member" },
		{ "Strata/Core/JsonUtils.h", "the JSON helpers: every user reads JSON values" },
		{ "Strata/Network/JsonRpc.h", "JSON default arguments and RequestValidation::Id" },
		{ "Strata/Network/RpcClient.h", "JSON default arguments" },
		{ "Strata/Network/RpcServer.h", "JSON members and default member initializers" },
		{ "Strata/Scene/EntityPrototype.h", "a vector of JSON members" },
		{ "Strata/Scene/PrefabOverrides.h", "PartChangeCache::Parts holds the prefab parts as JSON members" },
		{ "Strata/Scene/UnknownComponents.h", "a JSON member with an initializer and inline code" },
	} };

	// The headers EditorContext.h (included by about a hundred editor and test objects) declares instead of including:
	// it uses their types only by reference, pointer or owning pointer, so an edit of one rebuilds only its own users.
	constexpr std::array<std::string_view, 6> c_EditorContextDeclares = {
		"Editor/CommandJournal.h",
		"Editor/ExternalEditor.h",
		"Editor/GameExport.h",
		"Editor/ProjectThumbnail.h",
		"Editor/SceneRecovery.h",
		"Editor/ThumbnailCache.h",
	};

	bool NeedsFullJson(std::string_view path)
	{
		return std::any_of(c_FullJsonHeaders.begin(), c_FullJsonHeaders.end(), [path](const FullJsonHeader& header) { return header.Path == path; });
	}

	// The engine headers (paths relative to the source root) that include the complete nlohmann::json but are not in
	// c_FullJsonHeaders.
	std::vector<std::string> FindHeavyJsonIncludes(const std::vector<std::pair<std::string, std::string>>& headers)
	{
		std::vector<std::string> problems;
		for (const auto& [path, source] : headers)
		{
			for (const Tests::Layering::IncludeDirective& include : Tests::Layering::ParseIncludes(source))
			{
				if (include.Path == "nlohmann/json.hpp" && !NeedsFullJson(path))
				{
					problems.push_back(fmt::format("{}:{} includes <nlohmann/json.hpp>: include <nlohmann/json_fwd.hpp> when it only declares JSON "
						"parameters and returns, or list it in c_FullJsonHeaders with the reason it needs the complete type",
						path, include.Line));
				}
			}
		}
		return problems;
	}

}

TEST_SUITE("Architecture.HeaderWeight")
{
	TEST_CASE("A header including the complete JSON header must be one that needs it")
	{
		const std::vector<std::pair<std::string, std::string>> headers = {
			{ "Strata/Core/JsonUtils.h", "#include <nlohmann/json.hpp>\n" },
			{ "Strata/Scene/Light.h", "#include <nlohmann/json_fwd.hpp>\n" },
			{ "Strata/Scene/Heavy.h", "#pragma once\n\n#include <nlohmann/json.hpp>\n" },
		};
		const std::vector<std::string> problems = FindHeavyJsonIncludes(headers);
		REQUIRE(problems.size() == 1);
		CHECK(problems[0].starts_with("Strata/Scene/Heavy.h:3 includes <nlohmann/json.hpp>"));
	}

	TEST_CASE("Engine headers include the complete JSON header only when they need the complete type")
	{
		const std::filesystem::path sourceRoot = FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "Strata" / "src";
		std::vector<std::pair<std::string, std::string>> headers;
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator it(sourceRoot, error), end; !error && it != end; it.increment(error))
		{
			if (!it->is_regular_file(error) || it->path().extension() != ".h")
				continue;
			const std::optional<std::string> source = FileSystem::ReadText(it->path());
			REQUIRE(source);
			headers.emplace_back(FileSystem::ToUTF8(it->path().lexically_relative(sourceRoot)), *source);
		}
		REQUIRE_FALSE(error);
		REQUIRE(headers.size() > 100);

		for (const std::string& problem : FindHeavyJsonIncludes(headers))
			FAIL_CHECK(problem);

		// Every listed header still exists and still includes the complete header: the list does not go stale.
		for (const FullJsonHeader& listed : c_FullJsonHeaders)
		{
			CAPTURE(listed.Path);
			const auto it = std::find_if(headers.begin(), headers.end(), [&listed](const auto& header) { return header.first == listed.Path; });
			REQUIRE(it != headers.end());
			const std::vector<Tests::Layering::IncludeDirective> includes = Tests::Layering::ParseIncludes(it->second);
			CHECK(std::any_of(includes.begin(), includes.end(), [](const auto& include) { return include.Path == "nlohmann/json.hpp"; }));
		}
	}

	TEST_CASE("EditorContext.h declares the types it only refers to instead of including their headers")
	{
		const std::filesystem::path path = FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "StrataEditor" / "src" / "Editor" / "EditorContext.h";
		const std::optional<std::string> source = FileSystem::ReadText(path);
		REQUIRE(source);
		for (const Tests::Layering::IncludeDirective& include : Tests::Layering::ParseIncludes(*source))
		{
			for (std::string_view declared : c_EditorContextDeclares)
			{
				if (include.Path == declared)
					FAIL_CHECK(fmt::format("EditorContext.h:{} includes {}: declare its types instead (EditorContext.cpp includes it), and hold "
						"values of them through Scope<> with the destructor in the .cpp", include.Line, declared));
			}
		}
	}
}
