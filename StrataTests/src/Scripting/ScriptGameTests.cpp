#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/EditorAssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scripting/ScriptingComponents.h"
#include "TestHelpers.h"

#include <filesystem>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A project with a scene asset ("Levels/Next.stscene") and a material (an asset that is no scene), whose asset manager is
	// the active one while it lives.
	struct GameProject
	{
		Ref<EditorAssetManager> Manager;
		AssetHandle Level = UUID::Null();
		AssetHandle Paint = UUID::Null(); // A material

		GameProject()
		{
			const std::filesystem::path root = CreateTemporaryDirectory("ScriptGameProject");
			REQUIRE(FileSystem::CreateDirectories(root / "Assets" / "Levels"));
			Scene level("Next");
			level.CreateEntity("Goal");
			REQUIRE(FileSystem::WriteText(root / "Assets" / "Levels" / "Next.stscene", SceneSerializer::Serialize(level).dump(1, '\t')));

			EditorAssetManagerSpecification specification;
			specification.AssetDirectory = root / "Assets";
			specification.CacheDirectory = root / ".strata" / "Cache";
			specification.WatchFiles = false;
			Manager = CreateRef<EditorAssetManager>(specification);
			Manager->Scan();
			Level = Manager->FindAssetByPath("Levels/Next.stscene");
			REQUIRE(Manager->GetAssetType(Level) == AssetType::Scene);
			AssetMetadata metadata;
			metadata.Name = "Paint";
			Paint = Manager->AddMemoryAsset(Material::Create(), metadata);
			AssetManager::SetActive(Manager);
		}

		~GameProject()
		{
			AssetManager::SetActive(nullptr);
		}

		GameProject(const GameProject&) = delete;
		GameProject& operator=(const GameProject&) = delete;
	};

}

TEST_SUITE("Scripting.Game")
{
	TEST_CASE("Scripts ask to quit the game and to load scenes")
	{
		GameProject project;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity runner = scene.CreateEntity("Runner");
		ScriptEntry& entry = AddScriptEntry(runner, "GameFlow");
		AddFieldOverride(entry, "Level", PropertyType::Asset, PropertyValue(UUID(project.Level)));
		AddFieldOverride(entry, "LevelPath", PropertyType::String, PropertyValue(std::string("Levels/Next.stscene")));
		AddFieldOverride(entry, "ExitCode", PropertyType::Int, PropertyValue(int32_t(42)));
		scene.OnRuntimeStart();
		RunFrames(scene, 1);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, runner, "GameFlow", "Done"));
		CheckScriptChecks(system, runner, "GameFlow", 4);
		// The scene holds the requests for its owner (here: nobody, so the scene keeps running).
		CHECK(scene.GetQuitRequest() == 42);
		CHECK(scene.GetSceneLoadRequest() == UUID::Null()); // The restart replaced the loads
		CHECK(scene.IsRunning());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Refused scene loads and engines without the game functions request nothing")
	{
		GameProject project;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity runner = scene.CreateEntity("Runner");
		AddFieldOverride(AddScriptEntry(runner, "GameFlowMisuse"), "NotAScene", PropertyType::Asset, PropertyValue(UUID(project.Paint)));
		scene.OnRuntimeStart();
		RunFrames(scene, 1);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, runner, "GameFlowMisuse", "Done"));
		CheckScriptChecks(system, runner, "GameFlowMisuse", 5);
		CHECK_FALSE(scene.GetSceneLoadRequest().has_value());
		CHECK_FALSE(scene.GetQuitRequest().has_value());
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}
}
