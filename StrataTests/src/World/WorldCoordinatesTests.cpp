#include <doctest/doctest.h>

#include "Scene/SceneTestUtils.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/SceneServices.h"
#include "Strata/Scene/SceneWorkBudget.h"
#include "Strata/World/WorldComponents.h"
#include "Strata/World/WorldGrid.h"
#include "Strata/World/WorldStreamingSystem.h"
#include "TestHelpers.h"
#include "World/WorldTestUtils.h"

#if ST_WITH_PHYSICS
	#include "Strata/Physics/PhysicsComponents.h"
#endif

#include <chrono>
#include <cmath>
#include <set>
#include <thread>
#include <utility>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A 4 x 4 world of 32 m Ground cells (floors at y = 0) around (20,000, 0): 20 km from the origin, the edge of what
	// worlds support (WorldData::c_SupportedExtent).
	constexpr double c_FarX = 20'000.0;
	constexpr float c_SourceRadius = 40.0f;

	GeneratedWorldSpecification FarWorld()
	{
		GeneratedWorldSpecification specification;
		specification.Name = "Far";
		specification.GridOrigin = glm::dvec2(c_FarX - 64.0, -64.0);
		specification.EntitiesPerCell = 4;
		specification.BodiesPerCell = 0;
		specification.Floors = true;
		return specification;
	}

	void RunFrame(MountedWorld& mounted, Scene& scene)
	{
		mounted.Manager->Update();
		SceneWorkBudget budget = SceneWorkBudget::ForFrame(scene.GetSettings());
		scene.OnUpdateRuntime(Timestep(1.0f / 60.0f), &budget);
	}

	// The Ground cells whose squares lie within `radius` of `point`, worked out without the streamer.
	std::set<std::pair<int32_t, int32_t>> CellsWithin(const GeneratedWorld& world, const glm::dvec3& point, double radius)
	{
		std::set<std::pair<int32_t, int32_t>> cells;
		const WorldLayer& layer = world.Data.Layers[0];
		for (const WorldCell& cell : world.Data.Cells)
		{
			if (cell.Layer == layer.ID && WorldGrid::Distance(WorldGrid::CellBounds(layer, world.Data.GridOrigin, cell.X, cell.Z), point) <= radius)
				cells.insert({ cell.X, cell.Z });
		}
		return cells;
	}

	std::set<std::pair<int32_t, int32_t>> LoadedCells(const WorldStreamer& streamer)
	{
		std::vector<WorldActiveCell> active;
		streamer.GetActiveCells(active);
		std::set<std::pair<int32_t, int32_t>> cells;
		for (const WorldActiveCell& cell : active)
		{
			if (cell.Status == WorldCellStatus::Loaded)
				cells.insert({ cell.Key.X, cell.Key.Z });
		}
		return cells;
	}

}

TEST_SUITE("World.Coordinates")
{
	TEST_CASE("A world 20 km out streams the cells around its source, and bodies rest and are hit there")
	{
		ScopedJobSystem jobs(2, 1);
		MountedWorld mounted("WorldCoordinates", FarWorld());
		Scene scene("Persistent");
		scene.CreateEntity("World").AddComponent<WorldComponent>().World = mounted.World.World;
		Entity source = scene.CreateEntity("Source");
		source.AddComponent<StreamingSourceComponent>().Radius = c_SourceRadius;
		scene.OnRuntimeStart();
		const WorldStreamingSystem* world = scene.GetSystem<WorldStreamingSystem>();
		REQUIRE(world);

		// At the origin the source wants nothing: the world is 20 km away. Once the world is in and the source evaluated, the
		// streamer's decisions show it whatever the speed of the loads: no cell is wanted and no load started.
		REQUIRE(WaitUntil([&]()
		{
			RunFrame(mounted, scene);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			return world->GetStreamer() && world->GetStreamer()->HasEvaluated();
		}, std::chrono::seconds(30)));
		for (int frame = 0; frame < 10; frame++)
			RunFrame(mounted, scene);
		const WorldStreamingStats idle = world->GetStreamer()->GetStats();
		CHECK(idle.CellsWanted == 0);
		CHECK(idle.LoadsStarted == 0);
		CHECK(idle.CellsLoading == 0);
		CHECK(idle.CellsLoaded == 0);

		// Driven there, it streams exactly the cells within its radius.
		const glm::vec3 there(static_cast<float>(c_FarX) + 16.0f, 1.0f, 16.0f);
		source.GetComponent<TransformComponent>().Translation = there;
		source.MarkModified<TransformComponent>();
		const std::set<std::pair<int32_t, int32_t>> expected = CellsWithin(mounted.World, glm::dvec3(there), c_SourceRadius);
		REQUIRE(expected.size() == 9);
		REQUIRE(WaitUntil([&]()
		{
			RunFrame(mounted, scene);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			const WorldStreamingStats stats = world->GetStreamer()->GetStats();
			return stats.CellsLoading == 0 && stats.CellsLoaded == expected.size();
		}, std::chrono::seconds(60)));
		CHECK(LoadedCells(*world->GetStreamer()) == expected);
		CheckSceneCaches(scene);

#if ST_WITH_PHYSICS
		// A box dropped on the floor of cell (2, 2) there comes to rest on it and stays put: float32 resolves 2 mm there, and
		// box3d's contact solver lets a box landing flat slide a few millimeters (5.3 mm for a cube dropped 3.5 m at the
		// origin in a world of box3d alone).
		Entity box = scene.CreateEntity("Dropped");
		box.GetComponent<TransformComponent>().Translation = there + glm::vec3(0.0f, 1.0f, 0.0f);
		box.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Dynamic;
		box.AddComponent<BoxColliderComponent>();
		for (int frame = 0; frame < 120; frame++)
			RunFrame(mounted, scene);
		const glm::vec3 rested = glm::vec3(scene.GetWorldTransform(box)[3]);
		CHECK(rested.y == doctest::Approx(0.5f).epsilon(0.05));
		CHECK(std::abs(rested.x - there.x) < 0.02f);
		CHECK(std::abs(rested.z - there.z) < 0.02f);
		for (int frame = 0; frame < 600; frame++)
			RunFrame(mounted, scene);
		const glm::vec3 later = glm::vec3(scene.GetWorldTransform(box)[3]);
		CHECK(glm::length(later - rested) < 0.01f);

		// Physics' rays from there hit the box, and beside it the cell's floor.
		IPhysicsQueries* physics = scene.GetService<IPhysicsQueries>();
		REQUIRE(physics);
		const std::optional<RaycastHit> onBox = physics->Raycast(there + glm::vec3(0.0f, 9.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f, ~0u, Entity(), false);
		REQUIRE(onBox);
		CHECK(onBox->HitEntity == box);
		CHECK(onBox->Point.y == doctest::Approx(later.y + 0.5f).epsilon(0.01));
		const std::optional<RaycastHit> onFloor = physics->Raycast(there + glm::vec3(10.0f, 9.0f, 10.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f, ~0u, Entity(), false);
		REQUIRE(onFloor);
		CHECK(onFloor->HitEntity.GetName() == "Floor 2_2");
		CHECK(std::abs(onFloor->Point.y) < 0.01f);
#endif
		scene.OnRuntimeStop();
	}
}
