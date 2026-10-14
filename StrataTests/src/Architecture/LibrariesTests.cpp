#include <doctest/doctest.h>

#include "Architecture/LayeringAnalyzer.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/PlatformDetection.h"

#include <spdlog/fmt/fmt.h>

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests::Layering;

// The engine's libraries against the layer table: CMake makes one library per layer (CMake/StrataLayers.cmake) and
// describes them in <build>/Strata/LayerLibraries.json, which this reads.
namespace
{

	std::string JoinLines(const std::vector<std::string>& lines)
	{
		std::string text;
		for (const std::string& line : lines)
			text += "\n  " + line;
		return text;
	}

	// Platform folders of the other operating systems, which this build leaves out.
	bool IsOtherPlatform(const std::string& path)
	{
#if defined(ST_PLATFORM_WINDOWS)
		const char* others[] = { "Platform/Posix/", "Platform/Linux/", "Platform/MacOS/" };
#elif defined(ST_PLATFORM_MACOS)
		const char* others[] = { "Platform/Windows/", "Platform/Linux/" };
#else
		const char* others[] = { "Platform/Windows/", "Platform/MacOS/" };
#endif
		for (const char* other : others)
		{
			if (path.starts_with(other))
				return true;
		}
		return false;
	}

	// The layers whose paths match `path` and whose exclusions do not: one for every engine file.
	std::vector<std::string> FindLayers(const LayerTable& table, const std::string& path)
	{
		std::vector<std::string> layers;
		for (const Layer& layer : table.Layers)
		{
			bool matched = false;
			for (const std::string& glob : layer.Paths)
				matched = matched || MatchesGlob(glob, path);
			for (const std::string& glob : layer.Exclude)
				matched = matched && !MatchesGlob(glob, path);
			if (matched)
				layers.push_back(layer.Name);
		}
		return layers;
	}

	std::vector<std::string> GetStrings(const nlohmann::json& object, const char* member)
	{
		std::vector<std::string> values;
		const auto it = object.find(member);
		if (it == object.end() || !it->is_array())
			return values;
		for (const nlohmann::json& value : *it)
		{
			if (value.is_string())
				values.push_back(value.get<std::string>());
		}
		return values;
	}

}

TEST_SUITE("Architecture.Libraries")
{
	TEST_CASE("Every engine source compiles into the library of its layer, and each library links the built layers it may include")
	{
		const std::filesystem::path sourceDir = FileSystem::FromUTF8(STRATA_SOURCE_DIR);
		const std::optional<std::string> tableText = FileSystem::ReadText(sourceDir / "StrataTests" / "Architecture" / "Layers.json");
		REQUIRE(tableText);
		const std::optional<nlohmann::json> tableJson = JsonUtils::Parse(*tableText);
		REQUIRE(tableJson);
		LayerTable table;
		std::vector<std::string> errors;
		const bool tableParsed = ParseLayerTable(*tableJson, table, errors);
		INFO("Layers.json:", JoinLines(errors));
		REQUIRE(tableParsed);

		const std::optional<std::string> librariesText = FileSystem::ReadText(FileSystem::FromUTF8(STRATA_LAYER_LIBRARIES_FILE));
		INFO("Written by CMake/StrataLayers.cmake at configure time: ", STRATA_LAYER_LIBRARIES_FILE);
		REQUIRE(librariesText);
		const std::optional<nlohmann::json> libraries = JsonUtils::Parse(*librariesText);
		REQUIRE(libraries);
		const auto layersIt = libraries->find("Layers");
		REQUIRE(layersIt != libraries->end());
		REQUIRE(layersIt->is_array());
		REQUIRE(layersIt->size() == table.Layers.size());

		// The library of every file, as CMake assigned it.
		std::map<std::string, std::string> libraryOfFile;
		std::set<std::string> builtLayers;
		std::vector<std::string> problems;
		for (size_t index = 0; index < table.Layers.size(); index++)
		{
			const Layer& layer = table.Layers[index];
			const nlohmann::json& library = (*layersIt)[index];
			CAPTURE(layer.Name);
			REQUIRE(library.is_object());
			CHECK(JsonUtils::GetString(library, "Name") == layer.Name);
			CHECK(JsonUtils::GetString(library, "Target") == "Strata_" + layer.Name);
			const auto builtIt = library.find("Built");
			REQUIRE(builtIt != library.end());
			REQUIRE(builtIt->is_boolean());
			const bool built = builtIt->get<bool>();
			if (built)
				builtLayers.insert(layer.Name);

			for (const std::string& source : GetStrings(library, "Sources"))
			{
				if (!libraryOfFile.emplace(source, layer.Name).second)
					problems.push_back(fmt::format("{} is in the libraries of {} and {}", source, libraryOfFile[source], layer.Name));
			}

			// A library links exactly the built layers its layer may include, in the table's order. "Links" holds what the
			// target links once every directory added its links, so a link added anywhere else (a vendor table row, another
			// CMakeLists) shows here. A layer that is not built has no library and links nothing.
			std::vector<std::string> expectedLinks;
			for (const std::string& dependency : layer.MayInclude)
			{
				if (built && builtLayers.contains(dependency))
					expectedLinks.push_back(dependency);
			}
			CHECK(GetStrings(library, "Links") == expectedLinks);
		}

		// Every engine source of this build in the library of the one layer it belongs to.
		const std::string sourceRoot = JsonUtils::GetString(*tableJson, "SourceRoot");
		const std::filesystem::path root = sourceDir / FileSystem::FromUTF8(sourceRoot);
		size_t fileCount = 0;
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error))
		{
			const std::string path = FileSystem::ToUTF8(it->path().lexically_relative(root));
			if (!it->is_regular_file() || !IsSourceFile(path) || IsOtherPlatform(path))
				continue;
			fileCount++;
			const std::vector<std::string> layers = FindLayers(table, path);
			if (layers.size() != 1)
			{
				problems.push_back(fmt::format("{} is in {} layers", path, layers.size()));
				continue;
			}
			const auto library = libraryOfFile.find(path);
			if (library == libraryOfFile.end())
				problems.push_back(fmt::format("{} (layer {}) is in no library: configure again after adding files", path, layers.front()));
			else if (library->second != layers.front())
				problems.push_back(fmt::format("{} (layer {}) compiles into the library of {}", path, layers.front(), library->second));
			if (library != libraryOfFile.end())
				libraryOfFile.erase(library);
		}
		REQUIRE_FALSE(error);
		CHECK(fileCount > 200);
		for (const auto& [path, layer] : libraryOfFile)
			problems.push_back(fmt::format("{} is in the library of {} but not in the source tree", path, layer));
		INFO("Problems:", JoinLines(problems));
		CHECK(problems.empty());
	}
}
