#include <doctest/doctest.h>

#include "Scene/SceneTestUtils.h"
#include "Strata/Scene/SceneLoader.h"
#include "Strata/Scene/SceneWorkBudget.h"
#include "Strata/World/WorldComponents.h"
#include "Strata/World/WorldStreamingSystem.h"
#include "World/WorldTestUtils.h"

#if ST_WITH_PHYSICS
	#include "Strata/Physics/PhysicsComponents.h"
	#include "Strata/Physics/PhysicsSystem.h"
#endif

#include <chrono>
#include <cmath>
#include <thread>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Runs a frame of a running scene the way GameRuntime does: the asset manager, then the scene with the frame's budget.
	void RunFrame(MountedWorld& mounted, Scene& scene)
	{
		mounted.Manager->Update();
		SceneWorkBudget budget = SceneWorkBudget::ForFrame(scene.GetSettings());
		scene.OnUpdateRuntime(Timestep(1.0f / 60.0f), &budget);
	}

	// Frames until the condition holds (by the clock, sleeping a little for the loads on other threads).
	void RunUntil(MountedWorld& mounted, Scene& scene, const std::function<bool()>& condition)
	{
		REQUIRE(WaitUntil([&]()
		{
			RunFrame(mounted, scene);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return condition();
		}, std::chrono::seconds(60)));
	}

	// A persistent scene of the mounted world, made in code: its WorldComponent, and what `build` adds.
	Ref<Scene> MakePersistent(const MountedWorld& mounted, const std::function<void(Scene&)>& build = {})
	{
		Ref<Scene> scene = CreateRef<Scene>("Persistent");
		scene->CreateEntity("World").AddComponent<WorldComponent>().World = mounted.World.World;
		if (build)
			build(*scene);
		return scene;
	}

	Entity AddCamera(Scene& scene, const glm::vec3& position)
	{
		Entity camera = scene.CreateEntity("Camera");
		camera.AddComponent<CameraComponent>().Primary = true;
		camera.GetComponent<TransformComponent>().Translation = position;
		return camera;
	}

	uint32_t CountLoaded(const WorldStreamer& streamer, uint8_t layer)
	{
		std::vector<WorldCellState> states;
		streamer.GetCellStates(states);
		uint32_t count = 0;
		for (const WorldCellState& state : states)
			count += state.Key.Layer == layer && state.Status == WorldCellStatus::Loaded ? 1 : 0;
		return count;
	}

	GeneratedWorldSpecification TwoLayers()
	{
		GeneratedWorldSpecification specification;
		specification.GridX = 4;
		specification.GridZ = 4;
		specification.EntitiesPerCell = 10;
		specification.BodiesPerCell = 2;
		specification.PropsPerCell = 2;
		return specification;
	}

}

TEST_SUITE("World.Streaming")
{
	TEST_CASE("The World system streams around the primary camera without sources, and around active sources' layers")
	{
		ScopedJobSystem jobs(2, 1);
		MountedWorld mounted("WorldSystemSources", TwoLayers());
		Ref<Scene> scene = MakePersistent(mounted, [](Scene& persistent) { AddCamera(persistent, glm::vec3(16.0f, 2.0f, 16.0f)); });
		scene->OnRuntimeStart();
		WorldStreamingSystem* system = scene->GetSystem<WorldStreamingSystem>();
		REQUIRE(system);
		REQUIRE(system->GetStreamer());
		IWorldStreaming* streaming = scene->GetService<IWorldStreaming>();
		REQUIRE(streaming == system);
		REQUIRE(system->WaitForStartCells(std::chrono::seconds(30)));
		RunUntil(mounted, *scene, [&]() { return streaming->GetStats().CellsLoading == 0 && streaming->GetStats().CellsLoaded > 0; });
		// The camera at (16, 16): the Ground's cells within 48 m and the Props' within 24 m.
		CHECK(CountLoaded(*system->GetStreamer(), 1) == 6);
		CHECK(CountLoaded(*system->GetStreamer(), 2) == 9);
		CHECK(streaming->IsLoadedAround(glm::vec3(16.0f, 0.0f, 16.0f), 20.0f));

		// An active source takes over from the camera; its layer list limits it to the Props, around a point far away. A
		// disabled source and an inactive one count for nothing.
		Entity source = scene->CreateEntity("Source");
		source.GetComponent<TransformComponent>().Translation = glm::vec3(112.0f, 0.0f, 112.0f);
		StreamingSourceComponent& component = source.AddComponent<StreamingSourceComponent>();
		component.Layers = " Props ,";
		component.Radius = 10.0f;
		Entity disabled = scene->CreateEntity("Disabled");
		disabled.AddComponent<StreamingSourceComponent>().Enabled = false;
		Entity inactive = scene->CreateEntity("Inactive");
		inactive.AddComponent<StreamingSourceComponent>();
		inactive.AddComponent<InactiveComponent>();
		RunUntil(mounted, *scene, [&]()
		{
			return CountLoaded(*system->GetStreamer(), 1) == 0 && streaming->GetStats().CellsUnloading == 0 && streaming->GetStats().CellsLoading == 0;
		});
		CHECK(CountLoaded(*system->GetStreamer(), 2) == 4); // The four 16 m Props cells around (112, 112)
		CHECK(streaming->IsLoadedAround(glm::vec3(112.0f, 0.0f, 112.0f), 10.0f) == false); // The Ground counts too
		scene->OnRuntimeStop();
		CHECK(scene->GetService<IWorldStreaming>() == nullptr);
	}

	TEST_CASE("Requests load far regions first, layers toggle through the service, and unknown layer names warn once")
	{
		ScopedJobSystem jobs(2, 1);
		MountedWorld mounted("WorldSystemRequests", TwoLayers());
		Ref<Scene> scene = MakePersistent(mounted, [](Scene& persistent)
		{
			Entity source = persistent.CreateEntity("Source");
			StreamingSourceComponent& component = source.AddComponent<StreamingSourceComponent>();
			component.Radius = 8.0f;
			component.Layers = "Ground, Rivers";
		});
		scene->OnRuntimeStart();
		IWorldStreaming* streaming = scene->GetService<IWorldStreaming>();
		REQUIRE(streaming);
		const uint64_t before = Log::GetBuffer().GetLatestSequence();
		RunUntil(mounted, *scene, [&]() { return streaming->GetStats().CellsLoaded == 1 && streaming->GetStats().CellsLoading == 0; });
		for (uint32_t frame = 0; frame < 5; frame++)
			RunFrame(mounted, *scene);
		size_t warnings = 0;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(before))
			warnings += entry.Message.find("names the layer 'Rivers'") != std::string::npos ? 1 : 0;
		CHECK(warnings == 1);

		// A request far away loads before the source's next cells, until released.
		CHECK(streaming->RequestCellsAround(glm::vec3(std::nanf("")), 1.0f) == 0);
		CHECK(streaming->RequestCellsAround(glm::vec3(0.0f), -1.0f) == 0);
		const uint64_t request = streaming->RequestCellsAround(glm::vec3(100.0f, 0.0f, 100.0f), 20.0f);
		REQUIRE(request != 0);
		CHECK_FALSE(streaming->IsLoadedAround(glm::vec3(100.0f, 0.0f, 100.0f), 20.0f));
		RunUntil(mounted, *scene, [&]() { return streaming->IsLoadedAround(glm::vec3(100.0f, 0.0f, 100.0f), 20.0f); });
		CHECK(streaming->ReleaseCells(request));
		CHECK_FALSE(streaming->ReleaseCells(request));
		RunUntil(mounted, *scene, [&]() { return streaming->GetStats().CellsLoaded == 1 && streaming->GetStats().CellsUnloading == 0; });

		// Layers by name, through the service.
		CHECK(streaming->IsLayerEnabled("Props") == true);
		CHECK(streaming->SetLayerEnabled("Props", false));
		CHECK(streaming->IsLayerEnabled("Props") == false);
		CHECK_FALSE(streaming->SetLayerEnabled("Rivers", true));
		CHECK_FALSE(streaming->IsLayerEnabled("Rivers").has_value());
		const uint64_t props = streaming->RequestCellsAround(glm::vec3(8.0f, 0.0f, 8.0f), 1.0f);
		for (uint32_t frame = 0; frame < 10; frame++)
			RunFrame(mounted, *scene);
		CHECK(streaming->IsLoadedAround(glm::vec3(8.0f, 0.0f, 8.0f), 1.0f)); // Only the Ground's cell: Props are off
		streaming->ReleaseCells(props);
		scene->OnRuntimeStop();
	}

	TEST_CASE("Scenes without a world cost the system a storage check and stream nothing")
	{
		Scene scene("Plain");
		AddCamera(scene, glm::vec3(0.0f));
		scene.OnRuntimeStart(SceneRuntimeMode::Simulate);
		WorldStreamingSystem* system = scene.GetSystem<WorldStreamingSystem>();
		REQUIRE(system); // Simulate runs it too
		for (uint32_t frame = 0; frame < 3; frame++)
			scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
		CHECK(system->GetStreamer() == nullptr);
		CHECK(system->GetStats().LoadsStarted == 0);
		CHECK_FALSE(system->IsLoadedAround(glm::vec3(0.0f), 1.0f));
		CHECK_FALSE(system->SetLayerEnabled("Ground", false));
		CHECK_FALSE(system->WaitForStartCells(std::chrono::milliseconds(10)));
		scene.OnRuntimeStop();
	}

#if ST_WITH_PHYSICS
	TEST_CASE("A world loaded to run starts with its LoadBeforeStart cells in place and their bodies built")
	{
		ScopedJobSystem jobs(2, 1);
		GeneratedWorldSpecification specification = TwoLayers();
		specification.Floors = true;
		specification.BuildPersistent = [](Scene& persistent, AssetHandle world)
		{
			persistent.CreateEntity("World").AddComponent<WorldComponent>().World = world;
			Entity crate = persistent.CreateEntity("Crate");
			crate.GetComponent<TransformComponent>().Translation = glm::vec3(16.0f, 0.5f, 16.0f);
			crate.AddComponent<RigidBodyComponent>();
			crate.AddComponent<BoxColliderComponent>();
		};
		MountedWorld mounted("WorldPreparedStart", specification);

		SceneLoadOptions options;
		options.PrepareSystems = true;
		SceneLoadRequest request = SceneLoader::LoadAsync(mounted.World.Persistent, options);
		REQUIRE(WaitUntil([&]()
		{
			mounted.Manager->Update();
			request.Update();
			return request.IsFinished();
		}, std::chrono::seconds(60)));
		REQUIRE(request.GetState() == SceneLoadState::Ready);
		const Ref<Scene> scene = request.TakeScene();
		REQUIRE(scene);
		// The Ground's cells around the camera at the origin are in, the Props' (not LoadBeforeStart) are not.
		uint32_t groundCells = 0;
		for (const SubSceneStatus& status : scene->GetSubScenes())
		{
			CHECK(status.State == SubSceneState::Loaded);
			for (const WorldCell& cell : mounted.World.Data.Cells)
			{
				if (cell.Scene == status.Scene)
				{
					CHECK(cell.Layer == 1);
					groundCells++;
				}
			}
		}
		CHECK(groundCells == 4); // Within 48 m of the origin: (0, 0), (1, 0), (0, 1) and (1, 1)

		// The physics room the cooked index asks for was given before physics' preparation created its world.
		const uint32_t recommended = mounted.World.Data.Index->GetRecommendedMaxBodies();
		REQUIRE(recommended > 0);
		CHECK(scene->GetSettings().PhysicsCapacity.MaxBodies == recommended);

		scene->OnRuntimeStart();
		PhysicsSystem* physics = scene->GetSystem<PhysicsSystem>();
		REQUIRE(physics);
		CHECK(physics->GetStats().PreparedSlices >= 1);
		CHECK(physics->GetStats().MaxBodies >= recommended);
		const uint64_t bodies = physics->GetStats().BodyCount;
		CHECK(bodies == 1 + 4 * mounted.World.BodiesPerGroundCell);
		WorldStreamingSystem* system = scene->GetSystem<WorldStreamingSystem>();
		REQUIRE(system);
		for (uint32_t frame = 0; frame < 60; frame++)
			RunFrame(mounted, *scene);
		// The adopted cells were not loaded again, and the crate rests on its cell's floor.
		CHECK(CountLoaded(*system->GetStreamer(), 1) == 4);
		CHECK(scene->FindEntityByName("Crate").GetComponent<TransformComponent>().Translation.y == doctest::Approx(0.5f).epsilon(0.02));
		CheckSceneCaches(*scene);
		scene->OnRuntimeStop();
	}
#endif
}
