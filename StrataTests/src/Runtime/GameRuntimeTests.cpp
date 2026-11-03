#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Core/SettingsTestUtils.h"
#include "Runtime/PerfLogReader.h"
#include "Scene/SceneTestUtils.h"
#include "Strata/Asset/AssetPackWriter.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/FrameProfiler.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Input/Input.h"
#include "Strata/Input/InputActions.h"
#include "Strata/Project/GameManifest.h"
#include "Strata/Renderer/RendererQuality.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Core/SaveFiles.h"
#include "Strata/Runtime/GameGraphicsService.h"
#include "Strata/Runtime/GamePresentation.h"
#include "Strata/Runtime/GameRuntime.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/ProjectCollisionLayers.h"
#include "Strata/Scene/ProjectSceneTiming.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/World/WorldComponents.h"
#include "TestHelpers.h"
#include "World/WorldTestUtils.h"

#if ST_WITH_PHYSICS
	#include "Strata/Physics/PhysicsComponents.h"
	#include "Strata/Physics/PhysicsSimulationSettings.h"
	#include "Strata/Physics/PhysicsSystem.h"
	#include "Strata/Physics/PhysicsWorld.h"
#endif

#include <chrono>
#include <cmath>
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

	// Rewrites a manifest written by WriteGame: its format version and its settings (none when null).
	void RewriteManifest(const std::filesystem::path& manifest, uint32_t version, const nlohmann::json& settings)
	{
		const std::optional<std::string> text = FileSystem::ReadText(manifest);
		REQUIRE(text);
		std::optional<nlohmann::json> json = JsonUtils::Parse(*text);
		REQUIRE(json);
		(*json)["Strata"]["Version"] = version;
		if (settings.is_null())
			json->erase("Settings");
		else
			(*json)["Settings"] = settings;
		REQUIRE(FileSystem::WriteText(manifest, JsonUtils::Dump(*json, 1, '\t')));
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

	TEST_CASE("A game's played time counts its scenes' fixed steps from its start, not the loading scene's")
	{
		ScopedJobSystem jobs(2, 2);
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimePlayedTime");
		// The start scene takes many frames to read; the next one steps at 20 Hz, the others at the default 60 Hz.
		Scene next("Next");
		next.CreateEntity("Camera").AddComponent<CameraComponent>().Primary = true;
		next.GetSettings().FixedTimestep = 0.05f;
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 30'000) }, { 0x1011, MakeScene("Loading", 1) },
			{ 0x1012, ToBytes(JsonUtils::Dump(SceneSerializer::Serialize(next))) } }, UUID(0x1010), UUID(0x1011));
		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		CHECK(game->GetPlayedSeconds() == 0.0);

		// The loading scene steps (its first frame runs a step), but the game has not started playing.
		UpdateUntil(*game, [&]() { return game->GetScene() != nullptr; });
		REQUIRE(game->IsShowingLoadingScene());
		CHECK(game->GetScene()->GetFixedStepCount() >= 1);
		CHECK(game->GetPlayedSeconds() == 0.0);

		UpdateUntil(*game, [&]() { return game->HasStarted(); });
		const Ref<Scene> start = game->GetScene();
		// The frame that started the scene ran its first step; each frame of a 60th of a second runs one more.
		CHECK(start->GetFixedStepCount() == 1);
		for (int frame = 0; frame < 59; frame++)
			game->Update(c_Frame);
		CHECK(start->GetFixedStepCount() == 60);
		CHECK(game->GetPlayedSeconds() == doctest::Approx(1.0));
		// Time the game stops is not played.
		start->SetTimeScale(0.0f);
		for (int frame = 0; frame < 30; frame++)
			game->Update(c_Frame);
		CHECK(start->GetFixedStepCount() == 60);
		CHECK(game->GetPlayedSeconds() == doctest::Approx(1.0));
		start->SetTimeScale(1.0f);

		// The scenes after a switch add their steps at their own timestep (the start scene steps while the next one loads).
		start->RequestSceneLoad(UUID(0x1012));
		UpdateUntil(*game, [&]() { return game->GetSceneHandle() == UUID(0x1012) && game->GetScene()->GetFixedStepCount() >= 3; });
		const double startSeconds = static_cast<double>(start->GetFixedStepCount()) * static_cast<double>(1.0f / 60.0f);
		const double nextSeconds = static_cast<double>(game->GetScene()->GetFixedStepCount()) * static_cast<double>(0.05f);
		CHECK(start->GetFixedStepCount() > 60);
		CHECK(game->GetPlayedSeconds() == doctest::Approx(startSeconds + nextSeconds));
	}

	TEST_CASE("A run that keeps every fixed step plays its scene's clock on frames too long for its steps")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeKeepSteps");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 10) } }, UUID(0x1010));
		// A frame of a quarter second (Application's longest timestep) is longer than the 8 steps of a 60th an update runs
		// by default.
		const Timestep longFrame(0.25f);
		const double step = static_cast<double>(1.0f / 60.0f);
		struct LongFrame
		{
			double Played = 0.0;
			double SceneTime = 0.0;
			uint64_t Steps = 0;
			uint64_t Dropped = 0;
			float UnscaledDelta = 0.0f;
		};
		// The game time, the scene's clock and the steps one long frame adds, at a time scale.
		auto runLongFrame = [&](bool keepEveryFixedStep, float timeScale)
		{
			GameRuntimeOptions options;
			options.KeepEveryFixedStep = keepEveryFixedStep;
			std::string error;
			Scope<GameRuntime> game = GameRuntime::Create(manifest, &error, options);
			REQUIRE_MESSAGE(game, error);
			UpdateUntil(*game, [&]() { return game->HasStarted(); });
			Scene& scene = *game->GetScene();
			scene.SetTimeScale(timeScale);
			const double played = game->GetPlayedSeconds();
			const double time = scene.GetTime();
			const uint64_t steps = scene.GetFixedStepCount();
			const uint64_t dropped = scene.GetDroppedFixedStepCount();
			game->Update(longFrame);
			return LongFrame { game->GetPlayedSeconds() - played, scene.GetTime() - time, scene.GetFixedStepCount() - steps,
				scene.GetDroppedFixedStepCount() - dropped, scene.GetUnscaledDeltaTime() };
		};

		SUBCASE("Without it, the scene's clock runs ahead of the steps it dropped")
		{
			const LongFrame update = runLongFrame(false, 1.0f);
			CHECK(update.Steps == 8);
			CHECK(update.Dropped > 0);
			CHECK(update.Played == doctest::Approx(8.0 * step));
			CHECK(update.SceneTime == doctest::Approx(0.25));
		}
		SUBCASE("With it, the frame plays what its steps simulate, and the clocks agree")
		{
			const LongFrame update = runLongFrame(true, 1.0f);
			CHECK(update.Steps == 8);
			CHECK(update.Dropped == 0);
			CHECK(update.Played == doctest::Approx(8.0 * step));
			// The scene's clock and its scripts' frame time (scaled and unscaled) see the same limited frame.
			CHECK(update.SceneTime == doctest::Approx(8.0 * step));
			CHECK(static_cast<double>(update.UnscaledDelta) == doctest::Approx(8.0 * step));
		}
		SUBCASE("With it, a faster time scale limits the frame's wall time instead of its steps")
		{
			const LongFrame update = runLongFrame(true, 2.0f);
			CHECK(update.Steps == 8);
			CHECK(update.Dropped == 0);
			CHECK(update.Played == doctest::Approx(8.0 * step));
			CHECK(update.SceneTime == doctest::Approx(8.0 * step));
			CHECK(static_cast<double>(update.UnscaledDelta) == doctest::Approx(4.0 * step));
		}
		SUBCASE("With it, a frame short enough for its steps is not changed")
		{
			GameRuntimeOptions options;
			options.KeepEveryFixedStep = true;
			std::string error;
			Scope<GameRuntime> game = GameRuntime::Create(manifest, &error, options);
			REQUIRE_MESSAGE(game, error);
			UpdateUntil(*game, [&]() { return game->HasStarted(); });
			const double time = game->GetScene()->GetTime();
			game->Update(Timestep(0.1f));
			CHECK(game->GetScene()->GetTime() - time == doctest::Approx(0.1));
			CHECK(game->GetScene()->GetDroppedFixedStepCount() == 0);
		}
	}

	TEST_CASE("A game at 240 steps per second keeps every step of long frames")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeFixedRate");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 10) } }, UUID(0x1010));
		RewriteManifest(manifest, 5, { { ProjectSceneTiming::c_SectionId, { { "Version", 1 }, { "Value", { { "FixedRate", "240" } } } } } });
		const double step = static_cast<double>(1.0f / 240.0f);
		auto start = [&](bool keepEveryFixedStep)
		{
			GameRuntimeOptions options;
			options.KeepEveryFixedStep = keepEveryFixedStep;
			std::string error;
			Scope<GameRuntime> game = GameRuntime::Create(manifest, &error, options);
			REQUIRE_MESSAGE(game, error);
			UpdateUntil(*game, [&]() { return game->HasStarted(); });
			// The manifest's rate, not the scene's 1/60 s, and the steps of 2/15 s at it.
			REQUIRE(game->GetScene()->GetFixedTimestep() == 1.0f / 240.0f);
			REQUIRE(game->GetScene()->GetMaxFixedSteps() == 32);
			return game;
		};

		SUBCASE("A tenth of a second runs all its steps")
		{
			Scope<GameRuntime> game = start(false);
			Scene& scene = *game->GetScene();
			const uint64_t steps = scene.GetFixedStepCount();
			const uint64_t dropped = scene.GetDroppedFixedStepCount();
			game->Update(Timestep(0.1f));
			// 24 steps of a 240th; float rounding may leave the last one due at the next update. At the scene's own 8 steps an
			// update, 33 ms, the frame would drop most of them.
			CHECK(scene.GetFixedStepCount() - steps >= 23);
			CHECK(scene.GetDroppedFixedStepCount() == dropped);
		}
		SUBCASE("With KeepEveryFixedStep, a quarter second plays the 32 steps of 2/15 s and drops none")
		{
			Scope<GameRuntime> game = start(true);
			Scene& scene = *game->GetScene();
			const uint64_t steps = scene.GetFixedStepCount();
			const uint64_t dropped = scene.GetDroppedFixedStepCount();
			const double played = game->GetPlayedSeconds();
			game->Update(Timestep(0.25f));
			// The frame is limited to exactly 32 steps of time, but the accumulator is a float that loses a few ulps over 32
			// subtractions, so the 32nd step may fall due at the next update.
			const uint64_t first = scene.GetFixedStepCount() - steps;
			CHECK(first >= 31);
			CHECK(first <= 32);
			CHECK(scene.GetDroppedFixedStepCount() == dropped);
			CHECK(game->GetPlayedSeconds() - played == doctest::Approx(static_cast<double>(first) * step));
			game->Update(Timestep(0.25f));
			CHECK(scene.GetFixedStepCount() - steps - first >= 32);
			CHECK(scene.GetFixedStepCount() - steps >= 63);
			CHECK(scene.GetDroppedFixedStepCount() == dropped);
		}
		SUBCASE("Without it, a quarter second runs the 32 steps and drops the rest")
		{
			Scope<GameRuntime> game = start(false);
			Scene& scene = *game->GetScene();
			const uint64_t steps = scene.GetFixedStepCount();
			const uint64_t dropped = scene.GetDroppedFixedStepCount();
			game->Update(Timestep(0.25f));
			CHECK(scene.GetFixedStepCount() - steps == 32);
			CHECK(scene.GetDroppedFixedStepCount() > dropped);
		}
	}

	TEST_CASE("GameRuntimeOptions::Interpolation draws between fixed steps where the manifest's Scene.Timing allows it")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeInterpolation");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 10) } }, UUID(0x1010));
		auto start = [&](std::optional<bool> interpolation)
		{
			GameRuntimeOptions options;
			if (interpolation)
				options.Interpolation = *interpolation;
			std::string error;
			Scope<GameRuntime> game = GameRuntime::Create(manifest, &error, options);
			REQUIRE_MESSAGE(game, error);
			UpdateUntil(*game, [&]() { return game->HasStarted(); });
			game->Update(Timestep(1.0f / 60.0f));
			return game;
		};

		SUBCASE("Off by default: tools and tests draw what they simulate")
		{
			Scope<GameRuntime> game = start(std::nullopt);
			CHECK_FALSE(game->GetScene()->IsInterpolationEnabled());
			CHECK_FALSE(game->GetScene()->IsInterpolating());
		}
		SUBCASE("On where the program asks")
		{
			Scope<GameRuntime> game = start(true);
			CHECK(game->GetScene()->IsInterpolating());
		}
		SUBCASE("Off where the manifest's Scene.Timing turns it off")
		{
			RewriteManifest(manifest, 5, { { ProjectSceneTiming::c_SectionId, { { "Version", 1 }, { "Value", { { "Interpolation", false } } } } } });
			Scope<GameRuntime> game = start(true);
			CHECK(game->GetScene()->IsInterpolationEnabled());
			CHECK_FALSE(game->GetScene()->IsInterpolating());
		}
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

	TEST_CASE("An exported game's second scene collides by the project's table")
	{
		ScopedJobSystem jobs(2, 2);
		// The second scene's document stores its own table (0.5.0's form) where every layer collides; the project's keeps
		// layers 0 and 4 apart.
		Scene next("Next");
		next.CreateEntity("Camera").AddComponent<CameraComponent>().Primary = true;
		Entity ground = next.CreateEntity("Ground");
		ground.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -0.5f, 0.0f);
		ground.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(20.0f, 0.5f, 20.0f);
		Entity ghost = next.CreateEntity("Ghost");
		ghost.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 1.0f, 0.0f);
		ghost.AddComponent<RigidBodyComponent>().Layer = 4;
		ghost.AddComponent<BoxColliderComponent>();
		nlohmann::json document = SceneSerializer::Serialize(next);
		document["Scene"]["Settings"]["PhysicsLayers"] = { { "Names", { { "4", "Solid" } } }, { "NoCollisions", nlohmann::json::array() } };
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeProjectLayers");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 1) }, { 0x1012, ToBytes(JsonUtils::Dump(document)) } },
			UUID(0x1010));
		PhysicsLayerSettings project;
		project.Names[4] = "Ghosts";
		project.SetCollide(0, 4, false);
		RewriteManifest(manifest, 5, { { ProjectCollisionLayers::c_SectionId, { { "Version", 1 }, { "Value", project.ToJson() } } } });

		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		REQUIRE(game->WaitForStart());
		CHECK(game->GetScene()->GetPhysicsLayers() == project);
		game->GetScene()->RequestSceneLoad(UUID(0x1012));
		UpdateUntil(*game, [&]() { return game->GetSceneHandle() == UUID(0x1012); });
		Scene& running = *game->GetScene();
		CHECK(running.GetPhysicsLayers() == project);
		CHECK(running.DoStoredLayersDiffer());
		const PhysicsSystem* physics = running.GetSystem<PhysicsSystem>();
		REQUIRE(physics);
		CHECK_FALSE(physics->DoLayersCollide(0, 4));
		const Entity falling = running.FindEntityByName("Ghost");
		UpdateUntil(*game, [&]() { return glm::vec3(running.GetWorldTransform(falling)[3]).y < -2.0f; });
	}

	TEST_CASE("An exported game's physics runs with its manifest's Physics.Simulation section")
	{
		ScopedJobSystem jobs(2, 2);
		Scene start("Start");
		start.CreateEntity("Camera").AddComponent<CameraComponent>().Primary = true;
		Entity ground = start.CreateEntity("Ground");
		ground.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -0.5f, 0.0f);
		ground.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(20.0f, 0.5f, 20.0f);
		Entity box = start.CreateEntity("Box");
		box.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.5f, 0.0f);
		box.AddComponent<RigidBodyComponent>();
		box.AddComponent<BoxColliderComponent>();
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeProjectSimulation");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, ToBytes(JsonUtils::Dump(SceneSerializer::Serialize(start))) } }, UUID(0x1010));
		const nlohmann::json tuning = { { "ContactRecycleDistance", 0.02 }, { "SleepThreshold", 0.15 }, { "ContactHertz", 25.0 },
			{ "ContactDampingRatio", 8.0 }, { "ContactPushSpeed", 2.0 } };
		RewriteManifest(manifest, 5, { { PhysicsSimulationSettings::c_SectionId, { { "Version", 1 }, { "Value", tuning } } } });

		std::string error;
		Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(game, error);
		REQUIRE(game->WaitForStart());
		game->Update(c_Frame);
		const PhysicsSystem* physics = game->GetScene()->GetSystem<PhysicsSystem>();
		REQUIRE(physics);
		REQUIRE(physics->GetWorld());
		CHECK(physics->GetSimulationSource() == PhysicsSimulationSettings::Source::Project);
		const PhysicsWorld& world = *physics->GetWorld();
		CHECK(world.GetContactRecycleDistance() == doctest::Approx(0.02f));
		const std::optional<float> threshold = world.GetSleepThreshold(game->GetScene()->FindEntityByName("Box"));
		REQUIRE(threshold);
		CHECK(*threshold == doctest::Approx(0.15f));
		CHECK(world.GetSettings().ContactHertz == doctest::Approx(25.0f));
		CHECK(world.GetSettings().ContactDampingRatio == doctest::Approx(8.0f));
		CHECK(world.GetSettings().ContactPushSpeed == doctest::Approx(2.0f));
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

		// The perf log's columns read the running scene's streaming and physics: its capacity and its last step. Writing a row
		// turns box3d's counters on for the scene, so the contact columns are filled from its next step on.
		const FramePerfColumnGroup columns = game->MakePerfLogColumns();
		REQUIRE(columns.Names.size() == 15);
		CHECK(columns.Names[2] == "WorldCellsLoaded");
		CHECK(columns.Names[5] == "PhysicsBodiesWaiting");
		CHECK(columns.Names[6] == "PhysicsBodiesPending");
		CHECK(columns.Names[8] == "PhysicsStepMs");
		CHECK(columns.Names[12] == "PhysicsContacts");
		CHECK(columns.Names[14] == "PhysicsMaxBodySpeed");
		const auto writeRow = [&]()
		{
			std::string line;
			FramePerfRow row(line);
			columns.Write(row);
			REQUIRE(row.GetCount() == 15);
			const std::optional<PerfLogTable> table = ParsePerfLog("A,B,C,D,E,F,G,H,I,J,K,L,M,N,O\n" + line.substr(1) + "\n", error);
			REQUIRE_MESSAGE(table, error);
			return table->Rows[0];
		};
		// The first row: the scene's steps so far ran without the counters, so the contact columns stay empty, not 0.
		const std::vector<double> first = writeRow();
		CHECK(std::isnan(first[12]));
		CHECK(std::isnan(first[13]));
		for (uint32_t frame = 0; frame < 2; frame++)
			game->Update(c_Frame);
		const std::vector<double> values = writeRow();
		CHECK(values[0] >= 4.0); // Wanted
		CHECK(values[2] >= 4.0); // Loaded
#if ST_WITH_PHYSICS
		CHECK(values[4] >= static_cast<double>(4 * world.BodiesPerGroundCell)); // Bodies
		CHECK(values[5] == 0.0); // Waiting
		CHECK(values[6] == 0.0); // Pending: the cells' boxes need no mesh
		CHECK(values[7] >= values[4]); // Capacity
		CHECK(values[8] > 0.0); // Step ms
		CHECK(values[9] >= 0.0); // box3d's ms (it may round to 0 with nothing awake)
		for (size_t column = 10; column < 15; column++)
			CHECK_FALSE(std::isnan(values[column])); // Awake, sleeping, contacts, recycled, the fastest body
#else
		// An engine without physics leaves the physics columns empty.
		for (size_t column = 4; column < 15; column++)
			CHECK(std::isnan(values[column]));
#endif
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

	TEST_CASE("An exported game provides its manifest's settings while it runs, and a version-4 manifest none")
	{
		SettingsRegistry registry;
		Tests::RegisterTestSections(registry);
		GameRuntimeOptions options;
		options.Settings = &registry;
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeSettings");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 1) } }, UUID(0x1010));
		RewriteManifest(manifest, 5, {
			{ "Test.Simple", { { "Version", 1 }, { "Value", { { "Count", 2 } } } } },
			{ "Test.Complex", { { "Version", 1 }, { "Value", { { "Name", "Upgraded" } } } } },
			{ "Game.Unknown", { { "Version", 9 }, { "Value", { { "Anything", true } } } } } });
		REQUIRE_FALSE(EngineServices::Get<IProjectSettings>());

		std::string error;
		{
			Scope<GameRuntime> game = GameRuntime::Create(manifest, &error, options);
			REQUIRE_MESSAGE(game, error);
			UpdateUntil(*game, [&]() { return game->HasStarted(); });
			const IProjectSettings* settings = EngineServices::Get<IProjectSettings>();
			REQUIRE(settings);
			REQUIRE(settings->FindSection("Test.Simple"));
			CHECK((*settings->FindSection("Test.Simple"))["Count"] == 2);
			CHECK((*settings->FindSection("Test.Simple"))["Mode"] == "Fast");
			// Validated as the registry reads it: upgraded to the section's version.
			REQUIRE(settings->FindSection("Test.Complex"));
			CHECK((*settings->FindSection("Test.Complex"))["Names"] == nlohmann::json::array({ "Upgraded" }));
			// A section no registered module claims is not provided, and does not stop the game.
			CHECK_FALSE(settings->FindSection("Game.Unknown"));
		}
		CHECK_FALSE(EngineServices::Get<IProjectSettings>());

		// A version-4 manifest has no settings to provide: modules keep their defaults (and scenes their stored settings).
		RewriteManifest(manifest, 4, nullptr);
		Scope<GameRuntime> old = GameRuntime::Create(manifest, &error, options);
		REQUIRE_MESSAGE(old, error);
		CHECK_FALSE(old->GetManifest().Settings.has_value());
		CHECK_FALSE(EngineServices::Get<IProjectSettings>());
		UpdateUntil(*old, [&]() { return old->HasStarted(); });
		CHECK_FALSE(EngineServices::Get<IProjectSettings>());
	}

	TEST_CASE("An exported game loads the players' input bindings with its manifest's actions, and they end with it")
	{
		SettingsRegistry registry;
		REQUIRE(registry.Register(InputActionSet::MakeSection()));
		GameRuntimeOptions options;
		options.Settings = &registry;
		options.SaveFilesDirectory = CreateTemporaryDirectory("RuntimeBindingsSaves");
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeBindings");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 1) } }, UUID(0x1010));
		const nlohmann::json actions = { { "Maps", nlohmann::json::array({ { { "Name", "Gameplay" }, { "Actions", nlohmann::json::array({
			{ { "Name", "Jump" }, { "Bindings", { "Key/Space" } } } }) } } }) } };
		RewriteManifest(manifest, 5, { { InputActionSet::c_SectionId, { { "Version", 1 }, { "Value", actions } } } });
		InputPlayerBindings players;
		players.Actions["Gameplay/Jump"] = { *InputBinding::Parse("Key/J") };
		REQUIRE(FileSystem::WriteText(options.SaveFilesDirectory / InputPlayerBindings::c_FileName, players.Write()));

		std::string error;
		{
			Scope<GameRuntime> game = GameRuntime::Create(manifest, &error, options);
			REQUIRE_MESSAGE(game, error);
			CHECK(InputActions::GetPlayerBindings() == players);
			CHECK(InputActions::GetBindings("Jump") == std::vector<InputBinding> { *InputBinding::Parse("Key/J") });
		}
		CHECK(InputActions::GetPlayerBindings().Actions.empty());
		Input::Reset();
	}

	TEST_CASE("An exported game loads the players' graphics options before its first script call, and they end with it")
	{
		SettingsRegistry registry;
		REQUIRE(registry.Register(RendererQuality::MakeSection()));
		const SettingsSection& section = *registry.Find(RendererQuality::c_SectionId);
		GameRuntimeOptions options;
		options.Settings = &registry;
		options.SaveFilesDirectory = CreateTemporaryDirectory("RuntimeGraphicsSaves");
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeGraphics");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 1) } }, UUID(0x1010));
		RewriteManifest(manifest, 5, { { std::string(RendererQuality::c_SectionId), { { "Version", RendererQuality::c_SectionVersion },
			{ "Value", { { "Preset", "High" } } } } } });
		REQUIRE(FileSystem::WriteText(options.SaveFilesDirectory / GraphicsOptionsFile::c_FileName, GraphicsOptionsFile::Write({ { "Preset", "Low" } }, section)));

		IGameGraphics* before = EngineServices::Get<IGameGraphics>();
		std::string error;
		{
			// Create provides the options before the scene starts (its OnCreate is the first script call).
			Scope<GameRuntime> game = GameRuntime::Create(manifest, &error, options);
			REQUIRE_MESSAGE(game, error);
			IGameGraphics* graphics = EngineServices::Get<IGameGraphics>();
			REQUIRE(graphics);
			CHECK(graphics == &game->GetGraphics());
			CHECK(graphics->GetSection() == &section);
			CHECK(graphics->GetEffectiveValue()["Preset"] == "Low");
			CHECK(graphics->GetSource("Preset") == GameGraphicsSource::Player);
			CHECK(game->GetGraphics().GetQuality().HasSameKnobs(RendererQuality::GetPreset(RendererQualityPreset::Low)));
			// The manifest's Graphics Quality under them.
			REQUIRE(graphics->ResetOptions(""));
			CHECK(graphics->GetEffectiveValue()["Preset"] == "High");
		}
		CHECK(EngineServices::Get<IGameGraphics>() == before);

		// A version-4 manifest has no settings: the section's default, with the players' choices over it.
		RewriteManifest(manifest, 4, nullptr);
		{
			Scope<GameRuntime> game = GameRuntime::Create(manifest, &error, options);
			REQUIRE_MESSAGE(game, error);
			CHECK(game->GetGraphics().GetEffectiveValue()["Preset"] == "Low");
			REQUIRE(game->GetGraphics().ResetOptions("Preset"));
			CHECK(game->GetGraphics().GetEffectiveValue() == section.GetDefault());
		}
	}

	TEST_CASE("GameRuntimeOptions::Quality fixes the preset for the session without writing the file")
	{
		SettingsRegistry registry;
		REQUIRE(registry.Register(RendererQuality::MakeSection()));
		const SettingsSection& section = *registry.Find(RendererQuality::c_SectionId);
		GameRuntimeOptions options;
		options.Settings = &registry;
		options.SaveFilesDirectory = CreateTemporaryDirectory("RuntimeQualitySaves");
		options.Quality = RendererQualityPreset::Ultra;
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeQuality");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 1) } }, UUID(0x1010));
		RewriteManifest(manifest, 5, { { std::string(RendererQuality::c_SectionId), { { "Version", RendererQuality::c_SectionVersion },
			{ "Value", { { "Preset", "Medium" } } } } } });
		const std::filesystem::path file = options.SaveFilesDirectory / GraphicsOptionsFile::c_FileName;
		const std::string saved = GraphicsOptionsFile::Write({ { "Preset", "Low" } }, section);
		REQUIRE(FileSystem::WriteText(file, saved));

		std::string error;
		{
			Scope<GameRuntime> game = GameRuntime::Create(manifest, &error, options);
			REQUIRE_MESSAGE(game, error);
			GameGraphicsService& graphics = game->GetGraphics();
			CHECK(graphics.GetCommandLinePreset() == RendererQualityPreset::Ultra);
			CHECK(graphics.GetEffectiveValue() == RendererQuality::GetPresetValues()[static_cast<size_t>(RendererQualityPreset::Ultra)]);
			CHECK(graphics.GetSource("Preset") == GameGraphicsSource::CommandLine);
			CHECK(graphics.GetPlayerOptions() == nlohmann::json { { "Preset", "Low" } });
		}
		// The players' file is as it was.
		CHECK(FileSystem::ReadText(file) == saved);
	}

	TEST_CASE("The window title is the game's title")
	{
		GameManifest manifest;
		manifest.Name = "Adventure";
		manifest.Title = "Star Adventure";
		manifest.Version = "1.2.0";
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeTitle");
		const GamePresentation presentation = GamePresentation::Make(manifest, directory / "Adventure.stgame", directory / "Adventure Game.exe");
		CHECK(presentation.Title == "Star Adventure");
		CHECK(presentation.Banner.rfind("Star Adventure 1.2.0 (Strata ", 0) == 0);
		CHECK(presentation.AppId == "Adventure Game");
		WindowSpecification window;
		presentation.ApplyTo(window);
		CHECK(window.Title == "Star Adventure");
		CHECK(window.AppId == "Adventure Game");

		// A game without a title shows its name; one without a version (before version 5) names none.
		manifest.Title.clear();
		manifest.Version.clear();
		const GamePresentation untitled = GamePresentation::Make(manifest, directory / "Adventure.stgame", directory / "Adventure");
		CHECK(untitled.Title == "Adventure");
		CHECK(untitled.Banner.rfind("Adventure (Strata ", 0) == 0);
	}

	TEST_CASE("Changing the title or the company keeps the save and log directories")
	{
		// A name of its own: the save directory is resolved in the player's data, and removed again below.
		const std::string name = "Strata Test Identity Saves";
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeIdentitySaves");
		const std::filesystem::path manifestPath = WriteGame(directory, { { 0x1010, MakeScene("Start", 1) } }, UUID(0x1010));
		std::string error;
		std::optional<GameManifest> manifest = GameManifest::Load(manifestPath, &error);
		REQUIRE_MESSAGE(manifest, error);
		manifest->Name = name;

		std::vector<std::filesystem::path> saveDirectories;
		std::vector<std::string> dataDirectories;
		for (const auto& [title, company] : { std::pair<const char*, const char*>("First Title", "First Studio"), { "Second Title", "Second Studio" } })
		{
			manifest->Title = title;
			manifest->Company = company;
			REQUIRE_MESSAGE(manifest->Save(manifestPath, &error), error);
			dataDirectories.push_back(GamePresentation::Make(*manifest, manifestPath, directory / "Game").DataDirectoryName);
			Scope<GameRuntime> game = GameRuntime::Create(manifestPath, &error);
			REQUIRE_MESSAGE(game, error);
			const std::optional<std::filesystem::path> saves = game->GetSaveDirectory();
			REQUIRE(saves);
			saveDirectories.push_back(*saves);
		}
		CHECK(dataDirectories[0] == SaveFiles::MakeGameDirectoryName(name));
		CHECK(dataDirectories[1] == dataDirectories[0]);
		CHECK(saveDirectories[1] == saveDirectories[0]);
		CHECK(FileSystem::ToUTF8(saveDirectories[0].parent_path().filename()) == dataDirectories[0]);
		// Resolving the save directory created the game's data directory (not its Saves, which the first save creates).
		CHECK_FALSE(FileSystem::Exists(saveDirectories[0]));
		std::error_code ignored;
		std::filesystem::remove(saveDirectories[0].parent_path(), ignored);
	}

	TEST_CASE("The window icon comes from the manifest's icon file")
	{
		GameManifest manifest;
		manifest.Name = "Adventure";
		manifest.Icon = "Adventure.png";
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeIcon");
		const GamePresentation presentation = GamePresentation::Make(manifest, directory / "Adventure.stgame", directory / "Adventure");
		CHECK(presentation.IconFile == directory / "Adventure.png");
		WindowSpecification window;
		presentation.ApplyTo(window);
		CHECK(window.IconPath == directory / "Adventure.png");

		// Without an icon the window keeps none (on Windows, the executable's).
		manifest.Icon.clear();
		WindowSpecification plain;
		GamePresentation::Make(manifest, directory / "Adventure.stgame", directory / "Adventure").ApplyTo(plain);
		CHECK(plain.IconPath.empty());
	}

	TEST_CASE("A manifest section that fails Read stops the game naming it")
	{
		SettingsRegistry registry;
		Tests::RegisterTestSections(registry);
		GameRuntimeOptions options;
		options.Settings = &registry;
		const std::filesystem::path directory = CreateTemporaryDirectory("RuntimeSettingsInvalid");
		const std::filesystem::path manifest = WriteGame(directory, { { 0x1010, MakeScene("Start", 1) } }, UUID(0x1010));
		RewriteManifest(manifest, 5, {
			{ "Test.Complex", { { "Version", 2 }, { "Value", { { "Names", { "A" } } } } } },
			{ "Test.Simple", { { "Version", 1 }, { "Value", { { "Count", 50 } } } } } });

		std::string error;
		CHECK_FALSE(GameRuntime::Create(manifest, &error, options));
		CHECK(error == "The game's settings section 'Test.Simple' is invalid: 'Count' must be a whole number from 1 to 8");
		CHECK_FALSE(EngineServices::Get<IProjectSettings>());

		// Read refuses a newer version of a section too.
		RewriteManifest(manifest, 5, { { "Test.Complex", { { "Version", 3 }, { "Value", nlohmann::json::object() } } } });
		CHECK_FALSE(GameRuntime::Create(manifest, &error, options));
		CHECK(error.find("'Test.Complex' is invalid") != std::string::npos);
		// Once valid, the same game starts.
		RewriteManifest(manifest, 5, { { "Test.Simple", { { "Version", 1 }, { "Value", { { "Count", 5 } } } } } });
		CHECK(GameRuntime::Create(manifest, &error, options));
	}
}
