#include <doctest/doctest.h>

#include "Architecture/LayeringAnalyzer.h"
#include "Strata/Core/FileSystem.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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

	constexpr std::array<FullJsonHeader, 14> c_FullJsonHeaders = { {
		{ "stpch.h", "the precompiled header: each library parses it once" },
		{ "Strata/Asset/AssetImporter.h", "ImportContext::Settings is a JSON member" },
		{ "Strata/Asset/AssetRegistration.h", "AssetMeta::Settings is a JSON member with an initializer" },
		{ "Strata/Core/JsonEvents.h", "ParseJsonEvents runs nlohmann's parser template inline" },
		{ "Strata/Core/JsonUtils.h", "the JSON helpers: every user reads JSON values" },
		{ "Strata/Core/SettingsRegistry.h", "SettingsField::Default and SettingsSection::Schema are JSON members" },
		{ "Strata/Network/JsonRpc.h", "JSON default arguments and RequestValidation::Id" },
		{ "Strata/Network/RpcClient.h", "JSON default arguments" },
		{ "Strata/Network/RpcServer.h", "JSON members and default member initializers" },
		{ "Strata/Scene/CookedScene.h", "CookedSceneContents::Settings is a JSON member" },
		{ "Strata/Scene/EntityPrototype.h", "a vector of JSON members" },
		{ "Strata/Scene/EntitySink.h", "a vector of JSON members (the unknown components EntityColumns keeps)" },
		{ "Strata/Scene/PrefabOverrides.h", "PartChangeCache::Parts holds the prefab parts as JSON members" },
		{ "Strata/Scene/UnknownComponents.h", "a JSON member with an initializer and inline code" },
	} };

	// Headers of the project and its settings, which much of the engine, the editor and the runtime include: they reach the
	// settings' JSON through ProjectSettingsValues, whose JSON lives in its implementation, so they include none of the
	// headers that need the complete type (SettingsRegistry.h among them).
	constexpr std::array<std::string_view, 3> c_SettingsDeclaringHeaders = {
		"Strata/Core/ProjectSettings.h",
		"Strata/Project/GameManifest.h",
		"Strata/Project/Project.h",
	};

	// The headers EditorContext.h (included by about a hundred editor and test objects) declares instead of including:
	// it uses their types only by reference, pointer or owning pointer, so an edit of one rebuilds only its own users.
	constexpr std::array<std::string_view, 7> c_EditorContextDeclares = {
		"Editor/CommandJournal.h",
		"Editor/EditorPreferences.h",
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

	bool IncludesFullJson(std::string_view source)
	{
		const std::vector<Tests::Layering::IncludeDirective> includes = Tests::Layering::ParseIncludes(source);
		return std::any_of(includes.begin(), includes.end(), [](const auto& include) { return include.Path == "nlohmann/json.hpp"; });
	}

	// The lines (outside line comments) where a header that does not include the complete JSON header names a std::span of
	// nlohmann's JSON. Apple Clang 15's libc++ instantiates such a span's iterator functions, which need the complete element
	// type, in files that never call them: every file that includes the header without the complete type fails CI's macOS
	// build (RendererQuality::OptionModel did, in files that include SceneRenderer.h).
	std::vector<std::string> FindForwardDeclaredJsonSpans(const std::vector<std::pair<std::string, std::string>>& headers)
	{
		std::vector<std::string> problems;
		for (const auto& [path, source] : headers)
		{
			if (IncludesFullJson(source))
				continue;
			const std::string_view text = source;
			uint32_t lineNumber = 0;
			for (size_t lineStart = 0; lineStart < text.size();)
			{
				size_t lineEnd = text.find('\n', lineStart);
				if (lineEnd == std::string_view::npos)
					lineEnd = text.size();
				lineNumber++;
				std::string_view line = text.substr(lineStart, lineEnd - lineStart);
				line = line.substr(0, line.find("//"));
				for (size_t at = line.find("std::span<"); at != std::string_view::npos; at = line.find("std::span<", at + 1))
				{
					const std::string_view arguments = line.substr(at, line.find('>', at) - at);
					if (arguments.find("nlohmann::") != std::string_view::npos)
					{
						problems.push_back(fmt::format("{}:{} names a std::span of JSON but includes only <nlohmann/json_fwd.hpp>: hold a pointer or a "
							"reference to the container instead (Apple Clang 15's libc++ needs the complete type for the span in every file "
							"that includes the header)", path, lineNumber));
					}
				}
				lineStart = lineEnd + 1;
			}
		}
		return problems;
	}

	// The headers (.h) under a folder, with their paths relative to it.
	std::vector<std::pair<std::string, std::string>> ReadHeaders(const std::filesystem::path& folder)
	{
		std::vector<std::pair<std::string, std::string>> headers;
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
		{
			if (!it->is_regular_file(error) || it->path().extension() != ".h")
				continue;
			const std::optional<std::string> source = FileSystem::ReadText(it->path());
			REQUIRE(source);
			headers.emplace_back(FileSystem::ToUTF8(it->path().lexically_relative(folder)), *source);
		}
		REQUIRE_FALSE(error);
		return headers;
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
		const std::vector<std::pair<std::string, std::string>> headers = ReadHeaders(FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "Strata" / "src");
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

	TEST_CASE("A header that only declares JSON names no span of it")
	{
		const std::vector<std::pair<std::string, std::string>> headers = {
			{ "Strata/Renderer/Member.h", "#pragma once\n\n#include <nlohmann/json_fwd.hpp>\n\n#include <span>\n\nstruct Model\n{\n\tstd::span<const nlohmann::json> Values;\n};\n" },
			{ "Strata/Renderer/Return.h", "#include <nlohmann/json_fwd.hpp>\n\nstd::span<const nlohmann::json> GetValues(); // std::span<const nlohmann::json>\n" },
			{ "Strata/Core/Complete.h", "#include <nlohmann/json.hpp>\n\nstd::span<const nlohmann::json> GetValues();\n" },
			{ "Strata/Renderer/Rows.h", "#include <nlohmann/json_fwd.hpp>\n\nstd::span<const Row> GetRows(const nlohmann::json& value); // std::span<nlohmann::json>\n" },
		};
		const std::vector<std::string> problems = FindForwardDeclaredJsonSpans(headers);
		REQUIRE(problems.size() == 2);
		CHECK(problems[0].starts_with("Strata/Renderer/Member.h:9 names a std::span of JSON"));
		CHECK(problems[1].starts_with("Strata/Renderer/Return.h:3 names a std::span of JSON"));
	}

	TEST_CASE("Engine and editor headers that only declare JSON name no span of it")
	{
		const std::filesystem::path root = FileSystem::FromUTF8(STRATA_SOURCE_DIR);
		for (const std::filesystem::path& folder : { root / "Strata" / "src", root / "StrataEditor" / "src" })
		{
			CAPTURE(FileSystem::ToUTF8(folder));
			const std::vector<std::pair<std::string, std::string>> headers = ReadHeaders(folder);
			REQUIRE(headers.size() > 100);
			for (const std::string& problem : FindForwardDeclaredJsonSpans(headers))
				FAIL_CHECK(problem);
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

	TEST_CASE("The project and settings headers include no header that needs the complete JSON type")
	{
		const std::filesystem::path sourceRoot = FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "Strata" / "src";
		for (std::string_view header : c_SettingsDeclaringHeaders)
		{
			CAPTURE(header);
			const std::optional<std::string> source = FileSystem::ReadText(sourceRoot / FileSystem::FromUTF8(header));
			REQUIRE(source);
			for (const Tests::Layering::IncludeDirective& include : Tests::Layering::ParseIncludes(*source))
			{
				if (include.Path == "nlohmann/json.hpp" || NeedsFullJson(include.Path))
				{
					FAIL_CHECK(fmt::format("{}:{} includes {}: declare what it needs and include it in the .cpp", header, include.Line,
						include.Path));
				}
			}
		}
	}
}
