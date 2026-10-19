#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"
#include "Perf/SceneGenerators.h"
#include "Renderer/GPUTestUtils.h"
#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/FrameProfiler.h"
#include "Strata/Physics/PhysicsComponents.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Math/Math.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/SceneRenderer.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SceneWorkBudget.h"
#include "Strata/World/WorldStreamer.h"
#include "TestHelpers.h"
#include "World/WorldTestUtils.h"

#include <algorithm>
#include <chrono>
#include <functional>
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

	// One frame of a running scene that streams, in a game frame's order: the asset manager, the sub-scenes' slices within the
	// frame's budget, the streamer and `streaming`, work done in its place (timed: World.Streaming; the World system runs
	// after the slices and before physics), and the scene's update with physics (whose ChangeTime counts). A game frame makes
	// one pass over the sub-scenes, at the start of the scene's update: here the update's own pass gets a spent budget, so
	// that it slices nothing a second time, and the frame profiler's Scene.Load stage times both passes. Returns the frame's
	// loading work: Scene.Load, World.Streaming and physics' change time.
	double StreamFrame(AssetManagerBase* assets, Scene& scene, WorldStreamer* streamer, std::span<const WorldStreamingSource> sources,
		const std::function<void()>& streaming = {})
	{
		FrameProfiler& profiler = FrameProfiler::Get();
		const FrameStageID sceneLoad = profiler.FindStage("Scene.Load");
		REQUIRE(sceneLoad != c_InvalidFrameStage);
		profiler.BeginFrame();
		if (assets)
			assets->Update();
		SceneWorkBudget budget = SceneWorkBudget::ForFrame(scene.GetSettings());
		scene.UpdateSubScenes(budget);
		double work = 0.0;
		if (streamer || streaming)
		{
			const auto streamStart = std::chrono::steady_clock::now();
			if (streamer)
			{
				streamer->SetSources(sources);
				streamer->Update();
			}
			if (streaming)
				streaming();
			work += MillisecondsSince(streamStart);
		}
		SceneWorkBudget spent = budget;
		spent.EntitiesToCreate = 0;
		spent.EntitiesToDestroy = 0;
		spent.Deadline = std::chrono::steady_clock::now();
		scene.OnUpdateRuntime(Timestep(1.0f / 60.0f), &spent);
		profiler.EndFrame(0.0);
		work += profiler.GetFrame(0).StageMs[sceneLoad];
		const PhysicsSystem* physics = scene.GetSystem<PhysicsSystem>();
		return work + (physics ? physics->GetStats().ChangeTime : 0.0);
	}

}

TEST_SUITE("Perf.World")
{
	TEST_CASE("Unloading a cell of 25,000 entities with bodies spreads its work over frames")
	{
		// The scale audit's cell: 25,000 static boxes. Before the sliced unload its frame took 51-56 ms; now the request's frame
		// takes the cell out of the game (its roots inactive, its bodies out of the simulation in one batch) and the later
		// frames destroy it within the frame's budget (Scene::UpdateSubScenes) with physics' batched removals (ChangeTime).
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

			// The request's frame, as the World system makes it: the sub-scenes' slices (none of this cell's yet), the request in
			// the streamer's place, then the scene's update, where physics takes the cell's bodies out.
			double longest = StreamFrame(AssetManager::GetActive().get(), scene, nullptr, {}, [&]() { REQUIRE(scene.UnloadSubScene(cell)); });
			const double requestFrame = longest;
			uint32_t frames = 1;
			while (scene.GetSubSceneStatus(cell) != nullptr)
			{
				REQUIRE(frames++ < 1000);
				longest = std::max(longest, StreamFrame(AssetManager::GetActive().get(), scene, nullptr, {}));
			}
			// The bodies left with the entities.
			scene.OnUpdateRuntime(Timestep(1.0f / 60.0f));
			CHECK(scene.GetSystem<PhysicsSystem>()->GetStats().BodyCount == 1);
			MESSAGE("Unloading 25,000 entities with bodies: ", frames, " frames, the request's work ", requestFrame, " ms, the longest's ", longest, " ms");
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

TEST_SUITE("PerfGPU.World")
{
	TEST_CASE("Driving across a 16x16-cell world at 40 m/s while a GPU-driven renderer draws keeps each frame's work small")
	{
		// Perf.World.CellDriveMaxFrame's drive with a renderer (GPUDrivenMode::Auto) drawing every frame from above the source:
		// a frame's work is its loading work and the renderer's (its new proxies upload within NewInstanceBytesPerFrame, and
		// what waits drains). Maximum by nature: the hitch of a frame that a cell's arrival or departure lands in.
		Tests::GPUContext gpu;
		REQUIRE(gpu.IsValid());
		Perf::ScopedApplicationJobSystem jobs;
		GeneratedWorldSpecification specification;
		specification.GridX = 16;
		specification.GridZ = 16;
		specification.CellSize = 64.0f;
		specification.StreamingDistance = 128.0f;
		specification.EntitiesPerCell = 2'502;
		specification.BodiesPerCell = 501;
		MountedWorld mounted("PerfGPUWorldDrive", specification);

		Perf::CheckBudgetConfirmed("PerfGPU.World.CellDriveMaxFrame", [&]
		{
			Scene scene("Drive");
			Tests::AddNeutralPostProcess(scene);
			scene.CreateEntity("Floor").AddComponent<BoxColliderComponent>();
			scene.OnRuntimeStart();
			WorldStreamer streamer(scene);
			streamer.SetWorld(mounted.World.World);
			WorldStreamingSource source;
			source.Position = glm::vec3(32.0f, 1.0f, 512.0f);
			source.Forward = glm::vec3(1.0f, 0.0f, 0.0f);
			SceneRendererSpecification rendererSpecification;
			rendererSpecification.GPUDriven = GPUDrivenMode::Auto;
			SceneRenderer renderer(rendererSpecification);
			renderer.SetViewportSize(640, 360);
			const auto frame = [&]()
			{
				REQUIRE(gpu.GetDevice().BeginFrame());
				Renderer::BeginFrame();
				const double loading = StreamFrame(mounted.Manager.get(), scene, &streamer, std::span<const WorldStreamingSource>(&source, 1));
				SceneCamera camera = Tests::LookAt(source.Position + glm::vec3(-40.0f, 60.0f, 0.0f), source.Position + glm::vec3(60.0f, 0.0f, 0.0f));
				camera.Projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 640.0f / 360.0f, 0.1f, 1000.0f);
				camera.Far = 1000.0f;
				const auto renderStart = std::chrono::steady_clock::now();
				REQUIRE(renderer.Render(scene, camera));
				const double rendering = MillisecondsSince(renderStart);
				gpu.GetDevice().EndFrame();
				return loading + rendering;
			};
			// The start: everything around the source loaded, as a prepared start leaves it, and drawn.
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(5);
			while (!streamer.IsLoadedAround(source.Position, 128.0f) || streamer.GetStats().CellsLoading > 0 || renderer.GetStats().DeferredInstanceBytes > 0)
			{
				REQUIRE(std::chrono::steady_clock::now() < deadline);
				frame();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			double longest = 0.0;
			uint64_t mostDeferred = 0;
			for (uint32_t index = 0; index < 900; index++)
			{
				const auto frameStart = std::chrono::steady_clock::now();
				source.Position.x += 40.0f / 60.0f;
				longest = std::max(longest, frame());
				mostDeferred = std::max(mostDeferred, renderer.GetStats().DeferredInstanceBytes);
				std::this_thread::sleep_until(frameStart + std::chrono::microseconds(16'667));
			}
			// What waited to upload drains once the source stops.
			while (renderer.GetStats().DeferredInstanceBytes > 0 || streamer.GetStats().CellsLoading > 0)
			{
				REQUIRE(std::chrono::steady_clock::now() < deadline + std::chrono::minutes(1));
				frame();
			}
			MESSAGE("Driving 600 m across the grid while drawing: ", streamer.GetStats().LoadsStarted, " loads, the longest frame's work ", longest,
				" ms, at most ", mostDeferred, " bytes of new proxies waiting (GPU-driven: ", renderer.GetStats().GPUDriven, ")");
			CHECK(renderer.GetStats().DeferredInstanceBytes == 0);
			streamer.UnloadAll();
			scene.FlushSubSceneUnloads();
			scene.OnRuntimeStop();
			CHECK(gpu.GetNewErrorCount() == 0);
			return longest;
		});
	}
}
