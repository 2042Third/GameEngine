#include <doctest/doctest.h>

#include "Perf/SceneGenerators.h"
#include "Scene/SceneLoaderTestUtils.h"
#include "Scene/SceneTestUtils.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneWorkBudget.h"
#include "Strata/Scene/SubScene.h"
#include "TestHelpers.h"

#include <chrono>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr float c_FrameTime = 1.0f / 60.0f;

	// A prepared scene of `count` entities nested ten to a parent (Perf::CreateNestedHierarchy).
	Ref<Scene> MakeNestedScene(size_t count, const std::string& name = "Sub")
	{
		Ref<Scene> scene = CreateRef<Scene>(name);
		Perf::CreateNestedHierarchy(*scene, count);
		scene->UpdateWorldTransforms();
		return scene;
	}

	// A frame's budget with all the time in the world: slices are bounded by the counts alone, so tests count exactly.
	SceneWorkBudget CountedBudget(const Scene& scene)
	{
		SceneWorkBudget budget = SceneWorkBudget::ForFrame(scene.GetSettings());
		budget.Deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
		budget.SubSceneMilliseconds = 3'600'000.0;
		return budget;
	}

	// Updates the scene with counted budgets until the sub-scene is loaded (REQUIREs it).
	void LoadCompletely(Scene& scene, SubSceneID id)
	{
		REQUIRE(WaitUntil([&]()
		{
			SceneWorkBudget budget = CountedBudget(scene);
			scene.OnUpdateRuntime(c_FrameTime, &budget);
			const SubSceneStatus* status = scene.GetSubSceneStatus(id);
			REQUIRE(status);
			REQUIRE(status->State != SubSceneState::Failed);
			return status->State == SubSceneState::Loaded;
		}, std::chrono::seconds(60)));
	}

}

TEST_SUITE("Scene.SubScenes")
{
	TEST_CASE("Unloading leaves the game at once and destroys the entities over the following updates, within the budget")
	{
		ScopedJobSystem jobs(2, 1);
		Scene scene("Level");
		Entity resident = scene.CreateEntity("Resident");
		scene.GetSettings().MaxEntitiesDestroyedPerFrame = 2000;
		const SubSceneID id = scene.BeginAdditiveLoad(MakeNestedScene(10'001));
		LoadCompletely(scene, id);
		const size_t loaded = scene.GetEntityCount();
		REQUIRE(loaded == 10'002);
		const std::vector<Entity> roots = scene.GetSubSceneRoots(id);
		REQUIRE_FALSE(roots.empty());
		Entity deep = roots.front();
		while (const Entity child = scene.GetEntityByUUID(deep.GetComponent<RelationshipComponent>().Children.empty() ? UUID::Null()
			: deep.GetComponent<RelationshipComponent>().Children.front()))
			deep = child;

		// The request destroys nothing: the sub-scene's entities are inactive at once (out of rendering, scripts, physics).
		CHECK(scene.UnloadSubScene(id));
		CHECK(scene.GetEntityCount() == loaded);
		CHECK_FALSE(scene.IsActiveInHierarchy(deep));
		CHECK(scene.IsActiveInHierarchy(resident));
		const SubSceneStatus* status = scene.GetSubSceneStatus(id);
		REQUIRE(status);
		CHECK(status->State == SubSceneState::Unloading);
		CHECK_FALSE(scene.IsSubSceneLoaded(id));
		CHECK(scene.HasPendingSubScenes());
		CHECK(scene.UnloadSubScene(id)); // Again: nothing changes

		// Each update destroys at most the budget, children before parents, the caches exact after each.
		uint32_t updates = 0;
		size_t previous = scene.GetEntityCount();
		while (scene.GetSubSceneStatus(id) && updates < 20)
		{
			SceneWorkBudget budget = CountedBudget(scene);
			scene.OnUpdateRuntime(c_FrameTime, &budget);
			updates++;
			const size_t now = scene.GetEntityCount();
			CHECK(previous - now <= 2000);
			CHECK(budget.EntitiesToDestroy == 2000 - (previous - now));
			if (const SubSceneStatus* during = scene.GetSubSceneStatus(id))
				CHECK(during->DestroyedEntities == loaded - now);
			previous = now;
			CheckSceneCaches(scene);
		}
		CHECK(updates == 6); // ceil(10,001 / 2,000)
		CHECK(scene.GetSubSceneStatus(id) == nullptr);
		CHECK_FALSE(scene.UnloadSubScene(id));
		CHECK(scene.GetEntityCount() == 1);
		CHECK(scene.GetEntityByUUID(resident.GetUUID()));
		CHECK_FALSE(scene.HasPendingSubScenes());
	}

	TEST_CASE("Creating and destroying share one update's budget")
	{
		ScopedJobSystem jobs(2, 1);
		Scene scene("Level");
		const SubSceneID leaving = scene.BeginAdditiveLoad(MakeNestedScene(3000, "Leaving"));
		LoadCompletely(scene, leaving);
		const SubSceneID arriving = scene.BeginAdditiveLoad(MakeNestedScene(3000, "Arriving"));
		REQUIRE(scene.UnloadSubScene(leaving));

		size_t created = 0;
		size_t destroyed = 0;
		REQUIRE(WaitUntil([&]()
		{
			const size_t createdBefore = scene.GetSubSceneStatus(arriving)->CreatedEntities;
			const SubSceneStatus* leavingStatus = scene.GetSubSceneStatus(leaving);
			const size_t destroyedBefore = leavingStatus ? leavingStatus->DestroyedEntities : 3000;
			SceneWorkBudget budget = CountedBudget(scene);
			budget.EntitiesToCreate = 700;
			budget.EntitiesToDestroy = 400;
			scene.OnUpdateRuntime(c_FrameTime, &budget);
			const size_t createdNow = scene.GetSubSceneStatus(arriving)->CreatedEntities - createdBefore;
			leavingStatus = scene.GetSubSceneStatus(leaving);
			const size_t destroyedNow = (leavingStatus ? leavingStatus->DestroyedEntities : 3000) - destroyedBefore;
			CHECK(createdNow <= 700);
			CHECK(destroyedNow <= 400);
			CHECK(budget.EntitiesToCreate == 700 - createdNow);
			CHECK(budget.EntitiesToDestroy == 400 - destroyedNow);
			created += createdNow;
			destroyed += destroyedNow;
			return scene.IsSubSceneLoaded(arriving) && !leavingStatus;
		}, std::chrono::seconds(60)));
		CHECK(created == 3000);
		CHECK(destroyed == 3000);
		CHECK(scene.GetEntityCount() == 3000);
		CheckSceneCaches(scene);
	}

	TEST_CASE("A scene loaded again while its sub-scene is still being unloaded waits for it, then loads without an ID collision")
	{
		SceneLoaderFixture fixture(MakeSceneDocument(30, "Room"));
		Scene scene("Level");
		scene.GetSettings().MaxEntitiesDestroyedPerFrame = 4;
		const auto frame = [&]()
		{
			fixture.Manager->Update();
			SceneWorkBudget budget = CountedBudget(scene);
			scene.OnUpdateRuntime(c_FrameTime, &budget);
		};
		const SubSceneID first = scene.LoadSubScene(fixture.SceneHandle);
		REQUIRE(first != 0);
		REQUIRE(WaitUntil([&]() { frame(); return scene.IsSubSceneLoaded(first); }, std::chrono::seconds(30)));
		REQUIRE(scene.GetEntityCount() == 30);

		REQUIRE(scene.UnloadSubScene(first));
		const SubSceneID second = scene.LoadSubScene(fixture.SceneHandle);
		REQUIRE(second != 0);
		bool waited = false;
		REQUIRE(WaitUntil([&]()
		{
			frame();
			const SubSceneStatus* status = scene.GetSubSceneStatus(second);
			REQUIRE(status);
			REQUIRE(status->State != SubSceneState::Failed);
			// While the first still has entities, the second creates none.
			if (scene.GetSubSceneStatus(first))
			{
				waited |= status->State == SubSceneState::Loading;
				CHECK(status->CreatedEntities == 0);
			}
			return status->State == SubSceneState::Loaded;
		}, std::chrono::seconds(30)));
		CHECK(waited);
		CHECK(scene.GetSubSceneStatus(first) == nullptr);
		CHECK(scene.GetEntityCount() == 30);
		CheckSceneCaches(scene);
	}

	TEST_CASE("An entity moved out of a sub-scene being unloaded survives it")
	{
		ScopedJobSystem jobs(2, 1);
		Scene scene("Level");
		Entity resident = scene.CreateEntity("Resident");
		const SubSceneID id = scene.BeginAdditiveLoad(MakeNestedScene(111));
		LoadCompletely(scene, id);
		const std::vector<Entity> roots = scene.GetSubSceneRoots(id);
		REQUIRE(roots.size() == 1);
		const UUID kept = roots.front().GetComponent<RelationshipComponent>().Children.front();
		const size_t keptSubtree = 11; // A child of the root and its ten children

		REQUIRE(scene.UnloadSubScene(id));
		Entity moved = scene.GetEntityByUUID(kept);
		REQUIRE(moved);
		CHECK_FALSE(scene.IsActiveInHierarchy(moved));
		scene.SetParent(moved, resident);
		CHECK(scene.IsActiveInHierarchy(moved)); // Under an active parent now

		scene.FlushSubSceneUnloads();
		CHECK(scene.GetSubSceneStatus(id) == nullptr);
		CHECK(scene.GetEntityCount() == 1 + keptSubtree);
		CHECK(scene.GetEntityByUUID(kept));
		CheckSceneCaches(scene);
	}
}
