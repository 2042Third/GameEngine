#include <doctest/doctest.h>

#include "Scene/SceneTestUtils.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Scene/SceneWorkBudget.h"
#include "Strata/World/WorldStreamer.h"
#include "World/WorldTestUtils.h"

#if ST_WITH_PHYSICS
	#include "Strata/Physics/PhysicsComponents.h"
	#include "Strata/Physics/PhysicsSystem.h"
#endif

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Drives a streamer the way the World system does, one frame at a time: the asset manager's update, the sub-scenes'
	// slices within the frame's entity counts (their time limit lifted, so the counts alone decide), then the streamer.
	struct StreamerDrive
	{
		MountedWorld& Mounted;
		Scene& Target;
		WorldStreamer Streamer;
		std::vector<std::vector<WorldStreamingDecision>> Decisions; // Per frame
		uint32_t MostLoading = 0;

		StreamerDrive(MountedWorld& mounted, Scene& scene, float distanceScale = 1.0f)
			: Mounted(mounted), Target(scene), Streamer(scene)
		{
			Streamer.SetWorld(mounted.World.World, distanceScale);
			// The World asset first, so that the first frame decides.
			REQUIRE(WaitUntil([&]()
			{
				Mounted.Manager->Update();
				return Mounted.Manager->GetAsset<WorldAsset>(mounted.World.World) != nullptr;
			}, std::chrono::seconds(30)));
		}

		static SceneWorkBudget MakeBudget(const SceneSettings& settings, bool create)
		{
			SceneWorkBudget budget = SceneWorkBudget::ForFrame(settings, 1.0e6);
			budget.SubSceneMilliseconds = 1.0e6;
			if (!create)
			{
				budget.EntitiesToCreate = 0;
				budget.EntitiesToDestroy = 0;
			}
			return budget;
		}

		void Frame(std::span<const WorldStreamingSource> sources)
		{
			Mounted.Manager->Update();
			SceneWorkBudget budget = MakeBudget(Target.GetSettings(), true);
			if (Target.IsRunning())
				Target.OnUpdateRuntime(Timestep(1.0f / 60.0f), &budget);
			else
				Target.UpdateSubScenes(budget);
			Streamer.SetSources(sources);
			Streamer.Update();
			Decisions.push_back(Streamer.GetLastDecisions());
			MostLoading = std::max(MostLoading, Streamer.GetStats().CellsLoading);
		}

		void Frame(const WorldStreamingSource& source)
		{
			Frame(std::span<const WorldStreamingSource>(&source, 1));
		}

		// Lets every started load read and compile its scene without creating entities, so that what the next frames do
		// depends on the decisions only, not on how fast the loads ran.
		void SettleLoads()
		{
			REQUIRE(WaitUntil([&]()
			{
				Mounted.Manager->Update();
				SceneWorkBudget budget = MakeBudget(Target.GetSettings(), false);
				Target.UpdateSubScenes(budget);
				for (const SubSceneStatus& status : Target.GetSubScenes())
				{
					if (status.State == SubSceneState::Loading)
						return false;
				}
				return true;
			}, std::chrono::seconds(30)));
		}

		// Frames (each settled) until the condition holds.
		void FramesUntil(const WorldStreamingSource& source, const std::function<bool()>& condition, uint32_t maxFrames = 2000)
		{
			for (uint32_t frame = 0; frame < maxFrames && !condition(); frame++)
			{
				Frame(source);
				SettleLoads();
			}
			REQUIRE(condition());
		}

		size_t CountDecisions() const
		{
			size_t count = 0;
			for (const std::vector<WorldStreamingDecision>& frame : Decisions)
				count += frame.size();
			return count;
		}
	};

	WorldStreamingSource SourceAt(float x, float z, float radius = 0.0f)
	{
		WorldStreamingSource source;
		source.Position = glm::vec3(x, 1.0f, z);
		source.Radius = radius;
		return source;
	}

	// The cells of a layer whose squares lie within `radius` of a point, found independently of the streamer.
	std::set<WorldCellKey> CellsWithin(const WorldData& data, uint8_t layerID, const glm::vec3& point, double radius)
	{
		std::set<WorldCellKey> cells;
		const WorldLayer* layer = data.FindLayer(layerID);
		for (const WorldCell& cell : data.Cells)
		{
			if (cell.Layer == layerID && WorldGrid::Distance(WorldGrid::CellBounds(*layer, data.GridOrigin, cell.X, cell.Z), glm::dvec3(point)) <= radius)
				cells.insert(cell.GetKey());
		}
		return cells;
	}

	std::set<WorldCellKey> LoadedCells(const WorldStreamer& streamer)
	{
		std::vector<WorldCellState> states;
		streamer.GetCellStates(states);
		std::set<WorldCellKey> cells;
		for (const WorldCellState& state : states)
		{
			if (state.Status == WorldCellStatus::Loaded)
				cells.insert(state.Key);
		}
		return cells;
	}

	GeneratedWorldSpecification SmallWorld(uint32_t side = 6)
	{
		GeneratedWorldSpecification specification;
		specification.GridX = side;
		specification.GridZ = side;
		specification.EntitiesPerCell = 20;
		specification.BodiesPerCell = 0;
		return specification;
	}

}

TEST_SUITE("World.Streaming")
{
	TEST_CASE("A source loads the cells within its radius nearest first, four at a time, and keeps them within the hysteresis")
	{
		ScopedJobSystem jobs(2, 1);
		MountedWorld mounted("WorldStreamerRadius", SmallWorld());
		Scene scene("Streamed");
		StreamerDrive drive(mounted, scene);
		const WorldData& data = mounted.World.Data;

		// 48 m around the middle of cell (0, 0), whose cells are 32 m: H = max(16, 4.8) = 16.
		const WorldStreamingSource source = SourceAt(16.0f, 16.0f);
		const std::set<WorldCellKey> wanted = CellsWithin(data, 1, source.Position, 48.0);
		REQUIRE(wanted.size() == 6);
		drive.FramesUntil(source, [&]() { return LoadedCells(drive.Streamer) == wanted; });
		CHECK(drive.MostLoading <= WorldStreamer::c_MaxConcurrentCellLoads);
		CHECK(drive.Streamer.IsLoadedAround(source.Position, 48.0f));
		CHECK(drive.Streamer.GetStats().CellsWanted == 6);
		CHECK(drive.Streamer.GetStats().LoadsStarted == 6);
		// The first frame starts the four nearest cells, nearest first (ties by key).
		REQUIRE(drive.Decisions[0].size() == 4);
		CHECK(drive.Decisions[0][0].Key == WorldCellKey { 1, 0, 0 });
		CHECK(drive.Decisions[0][1].Key == WorldCellKey { 1, 0, 1 });
		CHECK(drive.Decisions[0][2].Key == WorldCellKey { 1, 1, 0 });
		CHECK(drive.Decisions[0][3].Key == WorldCellKey { 1, 1, 1 });
		const SubSceneID first = drive.Streamer.GetCellSubScene(WorldCellKey { 1, 0, 0 });
		REQUIRE(first != 0);
		CHECK(scene.GetSubSceneStatus(first)->CreatedEntities == 20);

		// Swinging 10 m to and fro takes the column x = 2 out of the radius and back, but never beyond the hysteresis.
		const size_t before = drive.CountDecisions();
		for (uint32_t frame = 0; frame < 60; frame++)
		{
			drive.Frame(SourceAt(frame % 2 == 0 ? 6.0f : 16.0f, 16.0f));
			drive.SettleLoads();
		}
		CHECK(drive.CountDecisions() == before);
		CHECK(LoadedCells(drive.Streamer) == wanted);

		// Beyond it (column x = 2 lies 84 m away from x = -20, more than 48 + 16), those cells go.
		const WorldStreamingSource away = SourceAt(-20.0f, 16.0f);
		const std::set<WorldCellKey> kept = CellsWithin(data, 1, away.Position, 48.0 + 16.0);
		drive.FramesUntil(away, [&]() { return drive.Streamer.GetStats().CellsUnloading == 0 && LoadedCells(drive.Streamer) == kept; });
		CHECK(drive.Streamer.GetStats().UnloadsStarted == wanted.size() - kept.size());
		CHECK(scene.GetSubSceneStatus(first) != nullptr);
		CheckSceneCaches(scene);
	}

	TEST_CASE("A still source costs no evaluation; one that moved more than a centimeter does")
	{
		ScopedJobSystem jobs(2, 1);
		MountedWorld mounted("WorldStreamerIdle", SmallWorld(3));
		Scene scene("Streamed");
		StreamerDrive drive(mounted, scene);
		const WorldStreamingSource source = SourceAt(48.0f, 48.0f);
		drive.FramesUntil(source, [&]() { return drive.Streamer.GetStats().CellsLoaded == 9; });

		drive.Frame(source);
		drive.Frame(source);
		CHECK(drive.Streamer.GetStats().EvaluatedCells == 0);
		CHECK(drive.Streamer.GetLastDecisions().empty());
		drive.Frame(SourceAt(48.005f, 48.0f)); // Half a centimeter
		CHECK(drive.Streamer.GetStats().EvaluatedCells == 0);
		drive.Frame(SourceAt(48.02f, 48.0f));
		CHECK(drive.Streamer.GetStats().EvaluatedCells > 0);
		drive.Frame(SourceAt(48.02f, 48.0f));
		CHECK(drive.Streamer.GetStats().EvaluatedCells == 0);
	}

	TEST_CASE("Layers, layer masks, pins and priorities decide what streams")
	{
		ScopedJobSystem jobs(2, 1);
		GeneratedWorldSpecification specification = SmallWorld(4);
		specification.PropsPerCell = 2;
		MountedWorld mounted("WorldStreamerLayers", specification);
		const WorldData& data = mounted.World.Data;
		Scene scene("Streamed");
		StreamerDrive drive(mounted, scene);

		// A source of the Ground only.
		WorldStreamingSource groundOnly = SourceAt(16.0f, 16.0f, 40.0f);
		groundOnly.LayerMask = 1u << 0;
		const std::set<WorldCellKey> ground = CellsWithin(data, 1, groundOnly.Position, 40.0);
		drive.FramesUntil(groundOnly, [&]() { return LoadedCells(drive.Streamer) == ground; });
		CHECK(drive.Streamer.IsLayerEnabled("Ground") == true);
		CHECK(drive.Streamer.IsLayerEnabled("Props") == true);
		CHECK_FALSE(drive.Streamer.IsLayerEnabled("Nope").has_value());
		CHECK_FALSE(drive.Streamer.SetLayerEnabled("Nope", false));

		// Both layers, then the Ground disabled: its cells go, the Props' stay.
		WorldStreamingSource both = groundOnly;
		both.LayerMask = ~0u;
		std::set<WorldCellKey> all = ground;
		const std::set<WorldCellKey> props = CellsWithin(data, 2, both.Position, 40.0);
		all.insert(props.begin(), props.end());
		drive.FramesUntil(both, [&]() { return LoadedCells(drive.Streamer) == all; });
		CHECK(drive.Streamer.SetLayerEnabled("Ground", false));
		CHECK(drive.Streamer.IsLayerEnabled("Ground") == false);
		drive.FramesUntil(both, [&]() { return LoadedCells(drive.Streamer) == props && drive.Streamer.GetStats().CellsUnloading == 0; });
		CHECK(drive.Streamer.IsLoadedAround(both.Position, 40.0f)); // Only enabled layers count
		CHECK(drive.Streamer.SetLayerEnabled("Ground", true));

		// A pinned cell far away loads, and goes once unpinned.
		const WorldCellKey farCell { 1, 3, 3 };
		drive.Streamer.PinCell(farCell);
		drive.Streamer.PinCell(farCell);
		CHECK(drive.Streamer.IsPinned(farCell));
		drive.FramesUntil(both, [&]() { return LoadedCells(drive.Streamer).contains(farCell); });
		CHECK(drive.Streamer.GetStats().CellsPinned == 1);
		drive.Streamer.UnpinCell(farCell);
		drive.Frame(both);
		drive.SettleLoads();
		CHECK(LoadedCells(drive.Streamer).contains(farCell)); // Pinned twice
		drive.Streamer.UnpinCell(farCell);
		drive.FramesUntil(both, [&]() { return !LoadedCells(drive.Streamer).contains(farCell); });

		// Of two sources far apart, the one of higher priority has its cells start first.
		Scene other("Priorities");
		StreamerDrive priorities(mounted, other);
		WorldStreamingSource low = SourceAt(8.0f, 8.0f, 8.0f);
		low.LayerMask = 1u << 0;
		WorldStreamingSource high = SourceAt(120.0f, 120.0f, 8.0f);
		high.LayerMask = 1u << 0;
		high.Priority = 5;
		const WorldStreamingSource sources[] = { low, high };
		priorities.Frame(sources);
		REQUIRE_FALSE(priorities.Decisions[0].empty());
		CHECK(priorities.Decisions[0][0].Key == WorldCellKey { 1, 3, 3 });
		CHECK(priorities.Decisions[0].back().Key == WorldCellKey { 1, 0, 0 });
	}

	TEST_CASE("Over a drive, loads in flight and the entities created and destroyed per frame stay within their budgets")
	{
		ScopedJobSystem jobs(2, 1);
		GeneratedWorldSpecification specification = SmallWorld(6);
		specification.EntitiesPerCell = 60;
		MountedWorld mounted("WorldStreamerBudgets", specification);
		Scene scene("Streamed");
		scene.GetSettings().MaxEntitiesInstantiatedPerFrame = 50;
		scene.GetSettings().MaxEntitiesDestroyedPerFrame = 40;
		StreamerDrive drive(mounted, scene);

		uint64_t created = 0;
		uint64_t destroyed = 0;
		for (uint32_t frame = 0; frame < 300; frame++)
		{
			// Across the world and back at 1.2 m per frame.
			const float x = 16.0f + 160.0f * std::abs(std::fmod(static_cast<float>(frame) / 150.0f, 2.0f) - 1.0f);
			const size_t entitiesBefore = scene.GetEntityCount();
			drive.Frame(SourceAt(x, 80.0f, 40.0f));
			drive.SettleLoads();
			const WorldStreamingStats stats = drive.Streamer.GetStats();
			CHECK(stats.CellsLoading <= WorldStreamer::c_MaxConcurrentCellLoads);
			CHECK(stats.EntitiesCreatedLastUpdate <= 50);
			CHECK(stats.EntitiesDestroyedLastUpdate <= 40);
			created += stats.EntitiesCreatedLastUpdate;
			destroyed += stats.EntitiesDestroyedLastUpdate;
			CHECK(scene.GetEntityCount() + stats.EntitiesDestroyedLastUpdate == entitiesBefore + stats.EntitiesCreatedLastUpdate);
		}
		CHECK(created > 1000);
		CHECK(destroyed > 500);
		CheckSceneCaches(scene);
	}

	TEST_CASE("The decisions of a drive are the same with one worker and with many")
	{
		auto driveWith = [](uint32_t workers)
		{
			ScopedJobSystem jobs(workers, std::max(1u, workers / 2));
			GeneratedWorldSpecification specification = SmallWorld(8);
			specification.PropsPerCell = 2;
			MountedWorld mounted(fmt::format("WorldStreamerDeterminism{}", workers), specification);
			Scene scene("Streamed");
			scene.GetSettings().MaxEntitiesInstantiatedPerFrame = 30;
			scene.GetSettings().MaxEntitiesDestroyedPerFrame = 25;
			StreamerDrive drive(mounted, scene);
			std::vector<std::tuple<uint32_t, WorldStreamingDecision::Action, uint64_t>> decisions;
			for (uint32_t frame = 0; frame < 600; frame++)
			{
				// A loop around the world with a turn of the facing, so that the view bias orders loads too.
				const float angle = static_cast<float>(frame) * 0.0105f;
				WorldStreamingSource source = SourceAt(128.0f + 90.0f * std::cos(angle), 128.0f + 90.0f * std::sin(angle));
				source.Forward = glm::vec3(-std::sin(angle), 0.0f, std::cos(angle));
				source.ViewBias = 0.5f;
				drive.Frame(source);
				for (const WorldStreamingDecision& decision : drive.Streamer.GetLastDecisions())
					decisions.emplace_back(frame, decision.Kind, decision.Key.Pack());
				drive.SettleLoads();
			}
			return decisions;
		};
		const auto one = driveWith(1);
		const auto many = driveWith(std::max(4u, std::thread::hardware_concurrency()));
		CHECK(one.size() > 40);
		CHECK(one == many);
	}

#if ST_WITH_PHYSICS
	TEST_CASE("With a cooked index, physics grows for the cells before their bodies arrive")
	{
		ScopedJobSystem jobs(2, 1);
		// Four cells of 2,500 bodies against a world created for 4,096 (the full size, 65,536 against four of 25,000, is the
		// Release perf metric Perf.World.PhysicsBodiesWaiting).
		GeneratedWorldSpecification specification;
		specification.GridX = 2;
		specification.GridZ = 2;
		specification.EntitiesPerCell = 2500;
		specification.BodiesPerCell = 2500;
		specification.CellSize = 64.0f;
		MountedWorld mounted("WorldStreamerReserve", specification);
		const PhysicsSettings previous = PhysicsSystem::GetDefaultSettings();
		PhysicsSettings settings = previous;
		settings.MaxBodies = 4096;
		PhysicsSystem::SetDefaultSettings(settings);
		struct Restore
		{
			PhysicsSettings Settings;
			~Restore() { PhysicsSystem::SetDefaultSettings(Settings); }
		} restore { previous };

		Scene scene("Streamed");
		scene.CreateEntity("Floor").AddComponent<BoxColliderComponent>(); // A physics world from the start
		scene.OnRuntimeStart();
		const IPhysicsCapacity* capacity = scene.GetService<IPhysicsCapacity>();
		REQUIRE(capacity);
		REQUIRE(capacity->GetCapacityInfo().MaxBodies == 4096);
		StreamerDrive drive(mounted, scene);
		const WorldStreamingSource source = SourceAt(64.0f, 64.0f, 40.0f);
		uint32_t firstGrowthTo = 0;
		drive.FramesUntil(source, [&]()
		{
			const PhysicsCapacityInfo info = capacity->GetCapacityInfo();
			CHECK(info.WaitingForSlot == 0);
			if (info.GrowthCount > 0 && firstGrowthTo == 0)
				firstGrowthTo = scene.GetSystem<PhysicsSystem>()->GetStats().LastGrowthTo;
			return drive.Streamer.GetStats().CellsLoaded == 4;
		});
		// One growth made room for every cell at once: the reservation, not the first slice of bodies, sized it.
		CHECK(firstGrowthTo >= 4 * 2500 + 1);
		CHECK(capacity->GetCapacityInfo().GrowthCount == 1);
		CHECK(capacity->GetCapacityInfo().Bodies == 4 * 2500 + 1);
		scene.OnRuntimeStop();
	}
#endif

	TEST_CASE("A cell that fails to load is not retried until its scene changes")
	{
		ScopedJobSystem jobs(2, 1);
		GeneratedWorldSpecification specification = SmallWorld(2);
		specification.BrokenCells = { 0 };
		MountedWorld mounted("WorldStreamerFailure", specification);
		Scene scene("Streamed");
		StreamerDrive drive(mounted, scene);
		const WorldStreamingSource source = SourceAt(32.0f, 32.0f);
		drive.FramesUntil(source, [&]() { return drive.Streamer.GetStats().CellsLoaded == 3 && drive.Streamer.GetStats().CellsFailed == 1; });
		const uint64_t loads = drive.Streamer.GetStats().LoadsStarted;
		CHECK(loads == 4);
		std::vector<WorldCellState> states;
		drive.Streamer.GetCellStates(states);
		REQUIRE(states.size() == 4);
		CHECK(states[0].Status == WorldCellStatus::Failed);
		CHECK_FALSE(states[0].Error.empty());
		CHECK(states[0].SubScene == 0);
		CHECK(states[0].Wanted);
		std::string error;
		CHECK(drive.Streamer.GetCellStatus(states[0].Key, &error) == WorldCellStatus::Failed);
		CHECK(error == states[0].Error);
		CHECK(drive.Streamer.GetCellStatus(states[1].Key) == WorldCellStatus::Loaded);
		CHECK(drive.Streamer.GetCellStatus(WorldCellKey { 1, 40, 40 }) == WorldCellStatus::Unloaded);
		// One cell's state, found by key, is what the whole list holds for it.
		for (const WorldCellState& listed : states)
		{
			WorldCellState state;
			REQUIRE(drive.Streamer.GetCellState(listed.Key, state));
			CHECK(state.Key == listed.Key);
			CHECK(state.Scene == listed.Scene);
			CHECK(state.Status == listed.Status);
			CHECK(state.SubScene == listed.SubScene);
			CHECK(state.Wanted == listed.Wanted);
			CHECK(state.Pinned == listed.Pinned);
			CHECK(state.Entities == listed.Entities);
			CHECK(state.Error == listed.Error);
		}
		CHECK(states[1].Entities > 0);
		WorldCellState missing;
		missing.Entities = 7;
		CHECK_FALSE(drive.Streamer.GetCellState(WorldCellKey { 1, 40, 40 }, missing));
		CHECK(missing.Entities == 7);
		CHECK(drive.Streamer.IsLoadedAround(source.Position, 1.0f) == false);
		for (uint32_t frame = 0; frame < 30; frame++)
			drive.Frame(SourceAt(32.0f + static_cast<float>(frame), 32.0f));
		CHECK(drive.Streamer.GetStats().LoadsStarted == loads);

		// Its scene changed (a reload): it is tried again, and fails again.
		mounted.Manager->ReloadAsset(mounted.World.GetCellScene(1, 0, 0));
		drive.FramesUntil(source, [&]() { return drive.Streamer.GetStats().LoadsStarted == loads + 1 && drive.Streamer.GetStats().CellsFailed == 1; });
	}

	TEST_CASE("Cells loaded before the streamer started are adopted, not loaded again")
	{
		ScopedJobSystem jobs(2, 1);
		MountedWorld mounted("WorldStreamerAdopt", SmallWorld(3));
		Scene scene("Streamed");
		const SubSceneID loaded = scene.LoadSubScene(mounted.World.GetCellScene(1, 1, 1));
		REQUIRE(loaded != 0);
		REQUIRE(WaitUntil([&]()
		{
			mounted.Manager->Update();
			scene.UpdateSubScenes();
			return scene.IsSubSceneLoaded(loaded);
		}, std::chrono::seconds(30)));

		StreamerDrive drive(mounted, scene);
		drive.Streamer.AdoptLoadedCells();
		const WorldStreamingSource source = SourceAt(48.0f, 48.0f, 10.0f);
		drive.FramesUntil(source, [&]() { return drive.Streamer.GetStats().CellsLoaded == 1; });
		CHECK(drive.Streamer.GetCellSubScene(WorldCellKey { 1, 1, 1 }) == loaded);
		CHECK(drive.Streamer.GetStats().LoadsStarted == 0);

		// It goes like the cells the streamer loads.
		drive.FramesUntil(SourceAt(-200.0f, -200.0f, 10.0f), [&]() { return scene.GetSubSceneStatus(loaded) == nullptr; });
		CHECK(drive.Streamer.GetStats().UnloadsStarted == 1);
		CHECK(scene.GetEntityCount() == 0);
	}
}
