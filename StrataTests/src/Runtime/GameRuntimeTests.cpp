#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Runtime/PerfLogReader.h"
#include "Scene/SceneTestUtils.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/FrameProfiler.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Project/GameManifest.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Runtime/GameRuntime.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/World/WorldComponents.h"
#include "TestHelpers.h"
#include "World/WorldTestUtils.h"

#if ST_WITH_PHYSICS
	#include "Strata/Physics/PhysicsComponents.h"
	#include "Strata/Physics/PhysicsSystem.h"
#endif

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr Timestep c_Frame = Timestep(1.0f / 60.0f);

	std::vector<uint8_t> ToBytes(const std::string& text)
	{
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// A scene document named `name` with a camera and `count` more entities.
	std::vector<uint8_t> MakeScene(const std::string& name, size_t count)
	{
		Scene scene(name);
		scene.CreateEntity("Camera").AddComponent<CameraComponent>().Primary = true;
		for (size_t index = 0; index < count; index++)
			scene.CreateEntity("Entity");
		return ToBytes(JsonUtils::Dump(SceneSerializer::Serialize(scene)));
	}

#if ST_WITH_PHYSICS
	// A scene document named `name` with a camera and `count` static boxes with colliders.
	std::vector<uint8_t> MakeColliderScene(const std::string& name, size_t count)
	{
		Scene scene(name);
		scene.CreateEntity("Camera").AddComponent<CameraComponent>().Primary = true;
		for (size_t index = 0; index < count; index++)
		{
			Entity box = scene.CreateEntity("Box");
			box.GetComponent<TransformComponent>().Translation = glm::vec3(static_cast<float>(index % 100) * 2.0f, 0.0f, static_cast<float>(index / 100) * 2.0f);
			box.AddComponent<BoxColliderComponent>();
		}
		return ToBytes(JsonUtils::Dump(SceneSerializer::Serialize(scene)));
	}
#endif

	// A scene document named `name` with a camera and `count` cubes (the built-in mesh, which every asset manager has).
	std::vector<uint8_t> MakeCubeScene(const std::string& name, size_t count)
	{
		Scene scene(name);
		scene.CreateEntity("Camera").AddComponent<CameraComponent>().Primary = true;
		for (size_t index = 0; index < count; index++)
		{
			Entity cube = scene.CreateEntity("Cube");
			cube.GetComponent<TransformComponent>().Translation = glm::vec3(static_cast<float>(index % 100) * 2.0f, 0.0f, static_cast<float>(index / 100) * 2.0f);
			cube.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::CubeMesh;
		}
		return ToBytes(JsonUtils::Dump(SceneSerializer::Serialize(scene)));
	}

	// An exported game made of the given scenes (handle -> stored document), written as a pack with a manifest.
	std::filesystem::path WriteGame(const std::filesystem::path& directory, const std::map<uint64_t, std::vector<uint8_t>>& scenes, UUID startScene,
		UUID loadingScene = UUID::Null())
	{
		std::vector<AssetMetadata> metadata;
		for (const auto& [handle, document] : scenes)
		{
			AssetMetadata scene;
			scene.Handle = UUID(handle);
			scene.Type = AssetType::Scene;
			scene.Path = "Scenes/" + std::to_string(handle) + ".stscene";
			scene.Name = std::to_string(handle);
			metadata.push_back(scene);
		}
		std::string error;
		REQUIRE_MESSAGE(AssetPackWriter::Write(directory / "Game.stpak", metadata, [&scenes](const AssetMetadata& asset, std::vector<uint8_t>& outData, std::string*)
		{
			outData = scenes.at(static_cast<uint64_t>(asset.Handle));
			return true;
		}, &error), error);
		GameManifest manifest;
		manifest.Name = "Loads";
		manifest.AssetPack = "Game.stpak";
		manifest.StartScene = startScene;
		manifest.LoadingScene = loadingScene;
		REQUIRE_MESSAGE(manifest.Save(directory / "Game.stgame", &error), error);
		return directory / "Game.stgame";
	}

	// Frames until the condition holds (REQUIREd), sleeping a little between them for the loads on other threads.
	template<typename Condition>
	void UpdateUntil(GameRuntime& game, Condition&& condition)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!condition())
		{
			REQUIRE(std::chrono::steady_clock::now() < deadline);
			game.Update(c_Frame);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

}

TEST_SUITE("Runtime.Game")
{
	TEST_CASE("A game opens at once and starts its scene once the scene is loaded")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeStart");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 100) } }, UUID(0x1010));
		const uint64_t reads = SceneSerializer::GetDeserializeCount();
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		// Nothing plays yet: the scene loads in the background, and renderers clear the frame meanwhile.
		CHECK_FALSE(game->HasStarted());
		CHECK_FALSE(game->GetScene());
		CHECK(game->IsLoadingScene());
		CHECK(game->GetLoadingSceneAsset() == UUID(0x1010));
		CHECK(game->GetSceneLoadProgress() <= 1.0f);
		UpdateUntil(*game, [&]() { return game->HasStarted(); });
		REQUIRE(game->GetScene());
		CHECK(game->GetScene()->IsRunning());
		CHECK(game->GetSceneHandle() == UUID(0x1010));
		CHECK(game->GetScene()->GetEntityCount() == 101);
		CHECK_FALSE(game->IsLoadingScene());
		CHECK_FALSE(game->GetStartError());
		CHECK(SceneSerializer::GetDeserializeCount() - reads == 1);
		// The spent scene asset is not kept.
		CHECK(game->GetAssetManager()->GetAssetState(UUID(0x1010)) == AssetState::Unloaded);
	}

	TEST_CASE("The loading scene plays while the start scene loads, then gives way to it")
	{
		ScopedJobSystem jobs(2, 2);
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeLoadingScene");
		// A start scene large enough to take many frames to read, and a small loading scene.
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 30'000) }, { 0x1011, MakeScene("Loading", 1) } }, UUID(0x1010),
			UUID(0x1011));
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		UpdateUntil(*game, [&]() { return game->GetScene() != nullptr; });
		REQUIRE(game->IsShowingLoadingScene());
		CHECK_FALSE(game->HasStarted());
		const Ref<Scene> loading = game->GetScene();
		CHECK(game->GetSceneHandle() == UUID(0x1011));
		CHECK(loading->GetName() == "Loading");
		CHECK(loading->IsRunning());
		// It plays, and its scripts could show the start scene's progress.
		game->Update(c_Frame);
		CHECK(loading->GetFrameIndex() >= 1);
		REQUIRE(loading->GetSceneLoadProgress().has_value());
		CHECK(*loading->GetSceneLoadProgress() < 1.0f);

		UpdateUntil(*game, [&]() { return game->HasStarted(); });
		CHECK_FALSE(game->IsShowingLoadingScene());
		CHECK(game->GetSceneHandle() == UUID(0x1010));
		CHECK(game->GetScene()->GetEntityCount() == 30'001);
		CHECK_FALSE(loading->IsRunning());
	}

	TEST_CASE("A game whose start scene cannot be loaded does not start")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeBrokenStart");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, ToBytes("{ \"Strata\": ") }, { 0x1011, MakeScene("Loading", 1) } }, UUID(0x1010),
			UUID(0x1011));
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		UpdateUntil(*game, [&]() { return game->GetStartError().has_value(); });
		CHECK(game->GetStartError()->find("Loading the start scene failed") != std::string::npos);
		CHECK_FALSE(game->HasStarted());
		CHECK_FALSE(game->WaitForStart(std::chrono::milliseconds(10)));

		// Scenes the manifest names must be scenes of the pack.
		const std::filesystem::path wrongLoading = WriteGame(CreateTemporaryDirectory("RuntimeWrongLoading"), { { 0x1010, MakeScene("Start", 1) } }, UUID(0x1010),
			UUID(0x1099));
		CHECK_FALSE(GameRuntime::Create(wrongLoading, &error));
		CHECK(error.find("loading scene") != std::string::npos);
		const std::filesystem::path wrongStart = WriteGame(CreateTemporaryDirectory("RuntimeWrongStart"), { { 0x1010, MakeScene("Start", 1) } }, UUID(0x1098));
		CHECK_FALSE(GameRuntime::Create(wrongStart, &error));
		CHECK(error.find("not a scene of the game") != std::string::npos);
	}

	TEST_CASE("A scene switch keeps the running scene playing until the next scene is in")
	{
		ScopedJobSystem jobs(2, 2);
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeSwitch");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 1) }, { 0x1012, MakeScene("Next", 30'000) } }, UUID(0x1010));
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		REQUIRE(game->WaitForStart());
		const Ref<Scene> start = game->GetScene();
		start->RequestSceneLoad(UUID(0x1012));
		game->Update(c_Frame);
		REQUIRE(game->IsLoadingScene());
		const uint64_t frameAtRequest = start->GetFrameIndex();
		UpdateUntil(*game, [&]() { return game->GetSceneHandle() == UUID(0x1012); });
		// The start scene kept running every frame until the switch.
		CHECK(start->GetFrameIndex() > frameAtRequest);
		CHECK_FALSE(start->IsRunning());
		CHECK(game->GetScene()->GetEntityCount() == 30'001);
		CHECK_FALSE(game->GetScene()->GetSceneLoadProgress().has_value());

		// A request replaces one under way; asking for what is not a scene keeps the scene running.
		game->GetScene()->RequestSceneLoad(UUID(0x1010));
		game->Update(c_Frame);
		CHECK(game->LoadScene(UUID(0x1012)));
		CHECK(game->GetLoadingSceneAsset() == UUID(0x1012));
		CHECK_FALSE(game->LoadScene(UUID(0x1077), &error));
		CHECK(game->GetLoadingSceneAsset() == UUID(0x1012));
		UpdateUntil(*game, [&]() { return !game->IsLoadingScene(); });
		CHECK(game->GetSceneHandle() == UUID(0x1012));
	}

	TEST_CASE("A game that renders starts and switches to scenes whose render proxies are built already")
	{
		ScopedJobSystem jobs(2, 2);
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimePreparedScenes");
		const size_t startCubes = RenderScene::c_PrepareChunk * 2 + 3;
		const size_t nextCubes = RenderScene::c_PrepareChunk + 5;
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeCubeScene("Start", startCubes) }, { 0x1012, MakeCubeScene("Next", nextCubes) } },
			UUID(0x1010));
		GameRuntimeOptions options;
		options.PrepareRenderScenes = true;
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error, options);
		REQUIRE_MESSAGE(game, error);
		REQUIRE(game->WaitForStart());
		// Nothing has drawn the scene, yet its proxies are all there: the first frame that draws it builds none.
		const RenderScene* start = RenderScene::Find(*game->GetScene());
		REQUIRE(start);
		CHECK(start->GetPendingCount() == 0);
		CHECK(start->GetStats().Proxies == startCubes);

		game->GetScene()->RequestSceneLoad(UUID(0x1012));
		UpdateUntil(*game, [&]() { return game->GetSceneHandle() == UUID(0x1012); });
		const RenderScene* next = RenderScene::Find(*game->GetScene());
		REQUIRE(next);
		CHECK(next->GetPendingCount() == 0);
		CHECK(next->GetStats().Proxies == nextCubes);

		// A headless game (the default) draws nothing, so it builds no proxies.
		game.reset();
		Scope<GameRuntime> headless = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(headless, error);
		REQUIRE(headless->WaitForStart());
		CHECK(RenderScene::Find(*headless->GetScene()) == nullptr);
	}

#if ST_WITH_PHYSICS
	TEST_CASE("A game starts and switches to scenes whose physics bodies are built already")
	{
		ScopedJobSystem jobs(2, 2);
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimePreparedPhysics");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeColliderScene("Start", 300) }, { 0x1012, MakeColliderScene("Next", 20) } },
			UUID(0x1010));
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		REQUIRE(game->WaitForStart());
		// The bodies were built while the scene loaded: the frame it started in built none.
		const PhysicsSystem* start = game->GetScene()->GetSystem<PhysicsSystem>();
		REQUIRE(start);
		CHECK(start->GetStats().BodyCount == 300);
		CHECK(start->GetStats().PreparedSlices >= 1);

		game->GetScene()->RequestSceneLoad(UUID(0x1012));
		UpdateUntil(*game, [&]() { return game->GetSceneHandle() == UUID(0x1012); });
		const PhysicsSystem* next = game->GetScene()->GetSystem<PhysicsSystem>();
		REQUIRE(next);
		CHECK(next->GetStats().BodyCount == 20);
		CHECK(next->GetStats().PreparedSlices >= 1);
	}
#endif

	TEST_CASE("A world game starts with its start cells in, paces the frames that show nothing and logs its streaming")
	{
		ScopedJobSystem jobs(2, 1);
		GeneratedWorldSpecification specification;
		specification.GridX = 3;
		specification.GridZ = 3;
		specification.EntitiesPerCell = 10;
		specification.BodiesPerCell = 3;
		specification.Floors = true;
		specification.BuildPersistent = [](Scene& persistent, AssetHandle world) { persistent.CreateEntity("World").AddComponent<WorldComponent>().World = world; };
		GeneratedWorld world;
		const std::filesystem::path manifest = WriteGeneratedWorldGame(CreateTemporaryDirectory("RuntimeWorldGame"), specification, &world);
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		// Nothing plays yet: a renderer would clear the frame, at most 60 times a second.
		CHECK(game->GetFrameRateCap(0) == GameRuntime::c_LoadingFrameRate);
		REQUIRE(game->WaitForStart());
		CHECK(game->GetFrameRateCap(0) == 0);
		CHECK(game->GetFrameRateCap(144) == 144);

		// The Ground's cells around the camera at the origin came with the start (prepared, then adopted), bodies and all.
		const IWorldStreaming* streaming = game->GetScene()->GetService<IWorldStreaming>();
		REQUIRE(streaming);
		CHECK(streaming->GetStats().CellsLoaded == 4);
		CHECK(streaming->GetStats().LoadsStarted == 0);
		for (uint32_t frame = 0; frame < 3; frame++)
			game->Update(c_Frame);

		// The perf log's columns read the running scene's streaming and physics.
		const FramePerfColumnGroup columns = game->MakePerfLogColumns();
		REQUIRE(columns.Names.size() == 7);
		CHECK(columns.Names[2] == "WorldCellsLoaded");
		CHECK(columns.Names[5] == "PhysicsBodiesWaiting");
		std::string line;
		FramePerfRow row(line);
		columns.Write(row);
		REQUIRE(row.GetCount() == 7);
		const std::optional<PerfLogTable> table = ParsePerfLog("A,B,C,D,E,F,G\n" + line.substr(1) + "\n", error);
		REQUIRE_MESSAGE(table, error);
		const std::vector<double>& values = table->Rows[0];
		CHECK(values[0] >= 4.0); // Wanted
		CHECK(values[2] >= 4.0); // Loaded
		CHECK(values[4] >= static_cast<double>(4 * world.BodiesPerGroundCell)); // Bodies
		CHECK(values[5] == 0.0); // Waiting
		CHECK(values[6] >= values[4]); // Capacity
	}

	TEST_CASE("A scene the running scene asks for in every update loads once and replaces it")
	{
		ScopedJobSystem jobs(2, 2);
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeRepeatedSwitch");
		// A next scene large enough to take many frames to read.
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 1) }, { 0x1012, MakeScene("Next", 30'000) } }, UUID(0x1010));
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		REQUIRE(game->WaitForStart());

		// As a script does that calls Game::LoadScene in every update while its condition holds: the load goes on (the
		// progress the running scene reads grows, and never falls back), and the scene's document is read once.
		const uint64_t reads = SceneSerializer::GetDeserializeCount();
		float progress = 0.0f;
		UpdateUntil(*game, [&]()
		{
			if (game->GetSceneHandle() == UUID(0x1012))
				return true;
			game->GetScene()->RequestSceneLoad(UUID(0x1012));
			if (const std::optional<float> reported = game->GetScene()->GetSceneLoadProgress())
			{
				CHECK(*reported >= progress);
				progress = *reported;
			}
			return false;
		});
		CHECK(progress > 0.0f);
		CHECK(game->GetScene()->GetEntityCount() == 30'001);
		CHECK(SceneSerializer::GetDeserializeCount() - reads == 1);

		// Starting the running scene over in every update (Game::ReloadScene) likewise loads it once.
		const Ref<Scene> previous = game->GetScene();
		const uint64_t reloadReads = SceneSerializer::GetDeserializeCount();
		UpdateUntil(*game, [&]()
		{
			if (game->GetScene() != previous)
				return true;
			previous->RequestSceneLoad(UUID::Null());
			return false;
		});
		CHECK(game->GetSceneHandle() == UUID(0x1012));
		CHECK(SceneSerializer::GetDeserializeCount() - reloadReads == 1);
	}

	TEST_CASE("A switch is timed in the frame's Scene.Load stage, and the scene it replaces is destroyed on a background thread")
	{
		ScopedJobSystem jobs(2, 2);
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeSwitchRelease");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 10'000) }, { 0x1012, MakeScene("Next", 1) } }, UUID(0x1010));
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		REQUIRE(game->WaitForStart());
		const SceneDestructionWatch start(*game->GetScene());
		game->GetScene()->RequestSceneLoad(UUID(0x1012));

		// Frames as the application runs them, between the profiler's frame markers.
		FrameProfiler& profiler = FrameProfiler::Get();
		profiler.Reset();
		const FrameStageID load = profiler.FindStage("Scene.Load");
		REQUIRE(load != c_InvalidFrameStage);
		float loadMs = 0.0f;
		UpdateUntil(*game, [&]()
		{
			if (profiler.IsInFrame())
			{
				profiler.EndFrame(0.0);
				loadMs += profiler.GetFrame(0).StageMs[load];
			}
			if (game->GetSceneHandle() == UUID(0x1012))
				return true;
			profiler.BeginFrame();
			return false;
		});
		profiler.Reset();
		CHECK(game->GetScene()->IsRunning());
		CHECK(loadMs > 0.0f);
		CHECK(start.WaitForDestruction() == SceneDestructionWatch::Place::BackgroundThread);
	}

	TEST_CASE("The loading scene starting itself over does not stop the start scene's load")
	{
		ScopedJobSystem jobs(2, 2);
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeLoadingRestart");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 30'000) }, { 0x1011, MakeScene("Loading", 1) } }, UUID(0x1010),
			UUID(0x1011));
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		UpdateUntil(*game, [&]() { return game->GetScene() != nullptr; });
		REQUIRE(game->IsShowingLoadingScene());

		// The loading scene asks to start over in every update (Game::ReloadScene) until the game started: it starts over
		// (loaded again on its own) while the start scene's load goes on.
		UpdateUntil(*game, [&]()
		{
			if (game->HasStarted())
				return true;
			game->GetScene()->RequestSceneLoad(UUID::Null());
			return false;
		});
		CHECK(game->GetSceneHandle() == UUID(0x1010));
		CHECK(game->GetScene()->GetEntityCount() == 30'001);
		CHECK_FALSE(game->IsShowingLoadingScene());
		CHECK_FALSE(game->IsLoadingScene());
	}
}
