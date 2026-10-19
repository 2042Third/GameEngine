#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/EditorAssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scripting/ScriptingComponents.h"
#include "TestHelpers.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A project with a scene asset to stream ("Levels/Room.stscene": RoomFloor with a child), a prefab ("Prefabs/Crate.stprefab")
	// that uses a material of the project, and that material; its asset manager is the active one while it lives.
	struct StreamingProject
	{
		Ref<EditorAssetManager> Manager;
		AssetHandle Room = UUID::Null();
		AssetHandle Crate = UUID::Null();
		AssetHandle Material = UUID::Null();

		StreamingProject()
		{
			const std::filesystem::path root = CreateTemporaryDirectory("ScriptStreamingProject");
			const std::filesystem::path assets = root / "Assets";
			for (const char* folder : { "Levels", "Prefabs", "Materials" })
				REQUIRE(FileSystem::CreateDirectories(assets / folder));
			MaterialProperties properties;
			properties.Roughness = 0.25f;
			REQUIRE(FileSystem::WriteText(assets / "Materials" / "Crate.stmat", Material::Create(properties)->Serialize().dump(1, '\t')));

			EditorAssetManagerSpecification specification;
			specification.AssetDirectory = assets;
			specification.CacheDirectory = root / ".strata" / "Cache";
			specification.WatchFiles = false;
			Manager = CreateRef<EditorAssetManager>(specification);
			Manager->Scan();
			Material = Manager->FindAssetByPath("Materials/Crate.stmat");
			REQUIRE(Material.IsValid());

			Scene room("Room");
			Entity floor = room.CreateEntity("RoomFloor");
			room.CreateChildEntity(floor, "RoomLamp");
			REQUIRE(FileSystem::WriteText(assets / "Levels" / "Room.stscene", SceneSerializer::Serialize(room).dump(1, '\t')));
			Scene authoring("Authoring");
			Entity crate = authoring.CreateEntity("Crate");
			MeshRendererComponent& renderer = crate.AddComponent<MeshRendererComponent>();
			renderer.Mesh = BuiltinAssets::CubeMesh;
			renderer.Material = Material;
			REQUIRE(FileSystem::WriteText(assets / "Prefabs" / "Crate.stprefab", Prefab::CreateFromEntities(authoring, { crate })->Serialize().dump(1, '\t')));
			Manager->Scan();
			Room = Manager->FindAssetByPath("Levels/Room.stscene");
			Crate = Manager->FindAssetByPath("Prefabs/Crate.stprefab");
			REQUIRE(Manager->GetAssetType(Room) == AssetType::Scene);
			REQUIRE(Manager->GetAssetType(Crate) == AssetType::Prefab);
			const std::vector<AssetHandle> dependencies = Manager->GetDependencies(Crate);
			REQUIRE(std::find(dependencies.begin(), dependencies.end(), Material) != dependencies.end());
			AssetManager::SetActive(Manager);
		}

		~StreamingProject()
		{
			AssetManager::SetActive(nullptr);
		}

		StreamingProject(const StreamingProject&) = delete;
		StreamingProject& operator=(const StreamingProject&) = delete;

		// One frame as applications run it: the asset manager's update, then the scene's.
		void Frame(Scene& scene)
		{
			Manager->Update();
			scene.OnUpdateRuntime(1.0f / 60.0f);
		}
	};

}

TEST_SUITE("Scripting.Streaming")
{
	TEST_CASE("Scripts load and unload sub-scenes and spawn many instances once what they use is in")
	{
		ScopedJobSystem jobs(2, 1);
		StreamingProject project;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene("Level");
		Entity streamer = scene.CreateEntity("Streamer");
		ScriptEntry& entry = AddScriptEntry(streamer, "SubSceneStreamer");
		AddFieldOverride(entry, "Room", PropertyType::Asset, PropertyValue(UUID(project.Room)));
		AddFieldOverride(entry, "RoomPath", PropertyType::String, PropertyValue(std::string("Levels/Room.stscene")));
		AddFieldOverride(entry, "Crate", PropertyType::Asset, PropertyValue(UUID(project.Crate)));
		scene.OnRuntimeStart();
		// A scene switch the owner reports while the level goes on (games and the editor set it every frame of a switch).
		scene.SetSceneLoadProgress(0.25f);

		const ScriptSystem& system = GetScriptSystem(scene);
		REQUIRE(WaitUntil([&]()
		{
			project.Frame(scene);
			return GetField<bool>(system, streamer, "SubSceneStreamer", "Done");
		}, std::chrono::seconds(30)));
		CheckScriptChecks(system, streamer, "SubSceneStreamer", 16);
		CHECK(GetField<float>(system, streamer, "SubSceneStreamer", "SwitchProgress") == 0.25f);
		CHECK(GetField<float>(system, streamer, "SubSceneStreamer", "CrateProgress") == 1.0f);
		CHECK(GetField<int32_t>(system, streamer, "SubSceneStreamer", "CratesSpawned") == 3);

		// The room came and went; the twin load was unloaded before its entities existed.
		CHECK(scene.GetSubScenes().empty());
		CHECK_FALSE(scene.FindEntityByName("RoomFloor"));
		CHECK_FALSE(scene.FindEntityByName("RoomLamp"));
		CHECK(streamer.GetChildren().size() == 3);
		const Entity first = scene.GetEntityByUUID(GetField<UUID>(system, streamer, "SubSceneStreamer", "FirstCrate"));
		REQUIRE(first);
		CHECK(first.GetName() == "Crate");
		CHECK(first.GetComponent<PrefabInstanceComponent>().Prefab == project.Crate);
		CHECK(first.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 1.0f, 0.0f));
		size_t crates = 0;
		for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
			crates += entity.GetName() == "Crate" ? 1 : 0;
		CHECK(crates == 4);
		// Released: nothing the closure request pinned stays pinned.
		CHECK(project.Manager->GetPinCount(project.Crate) == 0);
		CHECK(project.Manager->GetPinCount(project.Material) == 0);

		// Without a switch under way, the progress is negative.
		scene.SetSceneLoadProgress(std::nullopt);
		project.Frame(scene);
		CHECK(GetField<float>(system, streamer, "SubSceneStreamer", "SwitchProgress") < 0.0f);
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}

	TEST_CASE("The streaming functions refuse what they cannot do, and engines without them do nothing")
	{
		StreamingProject project;
		REQUIRE(project.Manager->LoadAssetSync(project.Crate));
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene("Level");
		Entity runner = scene.CreateEntity("Runner");
		ScriptEntry& entry = AddScriptEntry(runner, "StreamingMisuse");
		AddFieldOverride(entry, "NotAScene", PropertyType::Asset, PropertyValue(UUID(project.Material)));
		AddFieldOverride(entry, "Crate", PropertyType::Asset, PropertyValue(UUID(project.Crate)));
		scene.OnRuntimeStart();
		const size_t entities = scene.GetEntityCount();
		RunFrames(scene, 1);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, runner, "StreamingMisuse", "Done"));
		CheckScriptChecks(system, runner, "StreamingMisuse", 10);
		CHECK(scene.GetSubScenes().empty());
		CHECK(scene.GetEntityCount() == entities);
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}
}
