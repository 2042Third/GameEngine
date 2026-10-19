#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Physics/PhysicsComponents.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SceneWorkBudget.h"
#include "Strata/World/WorldStreamer.h"
#include "TestHelpers.h"
#include "World/WorldTestUtils.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	double MillisecondsSince(std::chrono::steady_clock::time_point start)
	{
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	// One frame of a running scene that streams: the asset manager, the sub-scenes' slices within the frame's budget (timed:
	// Scene.Load), the streamer (timed: World.Streaming) and the scene's update with physics (whose ChangeTime counts).
	// Returns the frame's loading work: the two timed parts plus physics' change time.
	double StreamFrame(AssetManagerBase* assets, Scene& scene, WorldStreamer* streamer, std::span<const WorldStreamingSource> sources)
	{
		if (assets)
			assets->Update();
		SceneWorkBudget budget = SceneWorkBudget::ForFrame(scene.GetSettings());
		const auto loadStart = std::chrono::steady_clock::now();
		scene.UpdateSubScenes(budget);
		double work = MillisecondsSince(loadStart);
		if (streamer)
		{
			const auto streamStart = std::chrono::steady_clock::now();
			streamer->SetSources(sources);
			streamer->Update();
			work += MillisecondsSince(streamStart);
		}
		scene.OnUpdateRuntime(Timestep(1.0f / 60.0f), &budget);
		const PhysicsSystem* physics = scene.GetSystem<PhysicsSystem>();
		return work + (physics ? physics->GetStats().ChangeTime : 0.0);
	}

}

TEST_SUITE("Perf.World")
{
	TEST_CASE("Unloading a cell of 25,000 entities with bodies spreads its work over frames")
	{
		// The scale audit's cell: 25,000 static boxes. Before the sliced unload its frame took 51-56 ms; the frame's work is
		// now bounded by the frame's budget (Scene::UpdateSubScenes) and physics' batched removals (ChangeTime).
		Perf::ScopedApplicationJobSystem jobs;
		Perf::CheckBudgetConfirmed("Perf.World.CellUnloadMaxFrame", [&]
		{
			Scene scene("Unload");
			scene.CreateEntity("Floor").AddComponent<BoxColliderComponent>();
			std::vector<Entity> boxes;
			boxes.reserve(25'000);
			for (uint32_t index = 0; index < 25'000; index++)
			{
				Entity box = scene.CreateEntity("Box");
				box.GetComponent<TransformComponent>().Translation = glm::vec3(static_cast<float>(index % 160) * 2.0f, 1.0f, static_cast<float>(index / 160) * 2.0f);
				box.AddComponent<BoxColliderComponent>();
				boxes.push_back(box);
			}
			const SubSceneID cell = scene.AdoptSubScene(UUID(0xCE11), boxes);
			REQUIRE(cell != 0);
			scene.OnRuntimeStart();
			REQUIRE(scene.GetSystem<PhysicsSystem>()->GetStats().BodyCount == 25'001);
			// A frame first: building the bodies is the start's work, not the unload's (the counters roll per update).
			scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));

			REQUIRE(scene.UnloadSubScene(cell));
			double longest = 0.0;
			uint32_t frames = 0;
			while (scene.GetSubSceneStatus(cell) != nullptr)
			{
				REQUIRE(frames++ < 1000);
				longest = std::max(longest, StreamFrame(AssetManager::GetActive().get(), scene, nullptr, {}));
			}
			// The bodies left with the entities.
			scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
			CHECK(scene.GetSystem<PhysicsSystem>()->GetStats().BodyCount == 1);
			MESSAGE("Unloading 25,000 entities with bodies: ", frames, " frames, the longest's work ", longest, " ms");
			scene.OnRuntimeStop();
			return longest;
		});
	}

	TEST_CASE("Four cells of 25,000 bodies stream into a world created for 65,536 without bodies waiting")
	{
		Perf::ScopedApplicationJobSystem jobs;
		GeneratedWorldSpecification specification;
		specification.GridX = 2;
		specification.GridZ = 2;
		specification.CellSize = 400.0f;
		specification.EntitiesPerCell = 25'000;
		specification.BodiesPerCell = 25'000;
		MountedWorld mounted("PerfWorldBodiesWaiting", specification);
		const PhysicsSettings previous = PhysicsSystem::GetDefaultSettings();
		PhysicsSettings settings = previous;
		settings.MaxBodies = 65'536;
		PhysicsSystem::SetDefaultSettings(settings);
		struct Restore
		{
			PhysicsSettings Settings;
			~Restore() { PhysicsSystem::SetDefaultSettings(Settings); }
		} restore { previous };

		Perf::CheckBudgetConfirmed("Perf.World.PhysicsBodiesWaiting", [&]
		{
			Scene scene("Streamed");
			scene.CreateEntity("Floor").AddComponent<BoxColliderComponent>();
			scene.OnRuntimeStart();
			const IPhysicsCapacity* capacity = scene.GetService<IPhysicsCapacity>();
			REQUIRE(capacity);
			REQUIRE(capacity->GetCapacityInfo().MaxBodies == 65'536);
			WorldStreamer streamer(scene);
			streamer.SetWorld(mounted.World.World);
			WorldStreamingSource source;
			source.Position = glm::vec3(400.0f, 0.0f, 400.0f);
			source.Radius = 100.0f;
			uint32_t mostWaiting = 0;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
			while (streamer.GetStats().CellsLoaded < 4)
			{
				REQUIRE(std::chrono::steady_clock::now() < deadline);
				StreamFrame(mounted.Manager.get(), scene, &streamer, std::span<const WorldStreamingSource>(&source, 1));
				mostWaiting = std::max(mostWaiting, capacity->GetCapacityInfo().WaitingForSlot);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
			CHECK(capacity->GetCapacityInfo().Bodies == 100'001);
			MESSAGE("Streaming 100,000 bodies into a world of 65,536: at most ", mostWaiting, " bodies waited, ", capacity->GetCapacityInfo().GrowthCount, " growths");
			streamer.UnloadAll();
			scene.FlushSubSceneUnloads();
			scene.OnRuntimeStop();
			return static_cast<double>(mostWaiting);
		});
	}

	TEST_CASE("Driving across a 16x16-cell world at 40 m/s keeps each frame's loading work small")
	{
		// The streaming audit's grid: cells of 2,502 entities with 501 static bodies, a source driven along a row.
		Perf::ScopedApplicationJobSystem jobs;
		GeneratedWorldSpecification specification;
		specification.GridX = 16;
		specification.GridZ = 16;
		specification.CellSize = 64.0f;
		specification.StreamingDistance = 128.0f;
		specification.EntitiesPerCell = 2'502;
		specification.BodiesPerCell = 501;
		MountedWorld mounted("PerfWorldDrive", specification);

		Perf::CheckBudgetConfirmed("Perf.World.CellDriveMaxFrame", [&]
		{
			Scene scene("Drive");
			scene.CreateEntity("Floor").AddComponent<BoxColliderComponent>();
			scene.OnRuntimeStart();
			WorldStreamer streamer(scene);
			streamer.SetWorld(mounted.World.World);
			WorldStreamingSource source;
			source.Position = glm::vec3(32.0f, 1.0f, 512.0f);
			source.Forward = glm::vec3(1.0f, 0.0f, 0.0f);
			// The start: everything around the source loaded, as a prepared start leaves it.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
			while (!streamer.IsLoadedAround(source.Position, 128.0f) || streamer.GetStats().CellsLoading > 0)
			{
				REQUIRE(std::chrono::steady_clock::now() < deadline);
				StreamFrame(mounted.Manager.get(), scene, &streamer, std::span<const WorldStreamingSource>(&source, 1));
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			// 900 frames at 40 m/s: 600 m along the row, with the frame rate's real time between frames.
			double longest = 0.0;
			for (uint32_t frame = 0; frame < 900; frame++)
			{
				const auto frameStart = std::chrono::steady_clock::now();
				source.Position.x += 40.0f / 60.0f;
				longest = std::max(longest, StreamFrame(mounted.Manager.get(), scene, &streamer, std::span<const WorldStreamingSource>(&source, 1)));
				std::this_thread::sleep_until(frameStart + std::chrono::microseconds(16'667));
			}
			MESSAGE("Driving 600 m across the grid: ", streamer.GetStats().LoadsStarted, " loads, ", streamer.GetStats().UnloadsStarted,
				" unloads, the longest frame's loading work ", longest, " ms");
			streamer.UnloadAll();
			scene.FlushSubSceneUnloads();
			scene.OnRuntimeStop();
			return longest;
		});
	}
}
