#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Perf/WorldFixture.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetPack.h>
#include <Strata/Asset/RuntimeAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Project/GameManifest.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

TEST_SUITE("Editor.Export")
{
	TEST_CASE("World projects export as games that start with the world asked for")
	{
		ScopedJobSystem jobs(4, 2);
		const std::filesystem::path directory = CreateTemporaryDirectory("WorldGame");
		WorldProjectSpec spec;
		spec.Scenes = { { "Town", 3000, true } };
		std::string error;
		const std::optional<WorldProject> project = WriteWorldProject(directory / "Project", spec, error);
		INFO(error);
		REQUIRE(project.has_value());

		WorldGameOptions options;
		options.StartScene = project->Scenes[0].Handle;
		const std::optional<WorldGame> game = WriteWorldGame(*project, directory / "Game", error, options);
		INFO(error);
		REQUIRE(game.has_value());
		CHECK(game->Executable.empty());
		const std::optional<GameManifest> manifest = GameManifest::Load(game->Manifest, &error);
		REQUIRE(manifest.has_value());
		CHECK(manifest->StartScene == project->Scenes[0].Handle);

		// The pack holds the world, which reads back with all its entities.
		const Ref<RuntimeAssetManager> runtime = RuntimeAssetManager::Create(game->AssetPack, &error);
		REQUIRE_MESSAGE(runtime, error);
		const std::vector<AssetPackEntry>& entries = runtime->GetPack().GetEntries();
		const auto world = std::find_if(entries.begin(), entries.end(), [&](const AssetPackEntry& entry) { return entry.Metadata.Handle == project->Scenes[0].Handle; });
		REQUIRE(world != entries.end());
		std::vector<uint8_t> bytes;
		REQUIRE(runtime->GetPack().ReadData(*world, bytes, &error));
		const std::optional<nlohmann::json> document = JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
		REQUIRE(document.has_value());
		Scene scene;
		REQUIRE(SceneSerializer::Deserialize(scene, *document, &error));
		CHECK(scene.GetEntityCount() == GetWorldEntityCount(spec.Scenes[0]));
	}
}
