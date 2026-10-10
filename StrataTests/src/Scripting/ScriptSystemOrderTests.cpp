#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"

#include <random>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	std::string Events(std::initializer_list<const char*> events)
	{
		std::string text;
		for (const char* event : events)
		{
			text += event;
			text += ';';
		}
		return text;
	}

	Entity CreateQuietLifecycle(Scene& scene, const std::string& name)
	{
		Entity entity = scene.CreateEntity(name);
		AddFieldOverride(AddScriptEntry(entity, "Lifecycle"), "RecordUpdates", PropertyType::Bool, false);
		return entity;
	}

	// The OnUpdate events of a log, in order.
	std::vector<std::string> GetUpdates(Scene& scene)
	{
		std::vector<std::string> updates;
		const std::string log = GetLog(scene);
		size_t start = 0;
		for (size_t end = log.find(';'); end != std::string::npos; start = end + 1, end = log.find(';', start))
		{
			const std::string event = log.substr(start, end - start);
			if (event.ends_with(".Update"))
				updates.push_back(event);
		}
		return updates;
	}

	// The OnUpdate events the update order must produce: the scripts of active entities in hierarchy order, each entity's in
	// entry order. Lifecycle and LifecycleSecond record themselves (as their class name).
	std::vector<std::string> GetExpectedUpdates(const Scene& scene)
	{
		std::vector<std::string> updates;
		for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
		{
			const ScriptComponent* component = entity.TryGetComponent<ScriptComponent>();
			if (!component || !scene.IsActiveInHierarchy(entity))
				continue;
			for (const ScriptEntry& entry : component->Scripts)
				updates.push_back(entity.GetName() + "." + entry.ClassName + ".Update");
		}
		return updates;
	}

}

TEST_SUITE("Scripting.Order")
{
	TEST_CASE("The update order changes only after scripted entities move or instances change")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		std::vector<Entity> entities;
		for (int index = 0; index < 20; index++)
			entities.push_back(CreateQuietLifecycle(scene, "Scripted" + std::to_string(index)));
		Entity plain = scene.CreateEntity("Plain");

		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		RunFrames(scene, 1);
		uint64_t rebuilds = system.GetUpdateOrderRebuildCount();
		const uint64_t orderBuilds = scene.GetHierarchyOrderBuildCount();

		// Frames, moves, activity and component edits leave the order alone.
		RunFrames(scene, 5);
		entities[3].GetTransform().Translation.x = 1.0f;
		entities[3].MarkModified<TransformComponent>();
		entities[4].SetActive(false);
		plain.AddComponent<TagComponent>("Tag");
		RunFrames(scene, 5);
		CHECK(system.GetUpdateOrderRebuildCount() == rebuilds);
		CHECK(scene.GetHierarchyOrderBuildCount() == orderBuilds);
		CHECK(GetField<int32_t>(system, entities[0], "Lifecycle", "Updates") == 11);
		CHECK(GetField<int32_t>(system, entities[4], "Lifecycle", "Updates") == 6);

		// A scripted entity moved: placed again once, at the next frame.
		REQUIRE(scene.SetParent(entities[7], plain));
		RunFrames(scene, 3);
		CHECK(system.GetUpdateOrderRebuildCount() == rebuilds + 1);
		rebuilds = system.GetUpdateOrderRebuildCount();

		// A new instance: placed once. Destroyed ones stay listed, skipped, until they could make up an eighth of the order;
		// then the order is compacted once.
		AddScriptEntry(plain, "HiddenCallbacks");
		plain.MarkModified<ScriptComponent>();
		RunFrames(scene, 3);
		CHECK(system.GetUpdateOrderRebuildCount() == rebuilds + 1);
		CHECK(GetField<int32_t>(system, plain, "HiddenCallbacks", "Updates") == 3);
		plain.RemoveComponent<ScriptComponent>();
		RunFrames(scene, 3);
		CHECK(system.GetUpdateOrderRebuildCount() == rebuilds + 1);
		CHECK(system.GetInstanceCount() == 20);
		const int32_t updates = GetField<int32_t>(system, entities[0], "Lifecycle", "Updates");
		entities[10].RemoveComponent<ScriptComponent>();
		entities[11].RemoveComponent<ScriptComponent>();
		RunFrames(scene, 3);
		CHECK(system.GetUpdateOrderRebuildCount() == rebuilds + 2);
		CHECK(system.GetInstanceCount() == 18);
		CHECK(GetField<int32_t>(system, entities[0], "Lifecycle", "Updates") == updates + 3);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Entities without scripts never make the update order walk the scene")
	{
		// A scene of plain entities with a few scripts: spawning, destroying and moving plain entities leaves the order
		// alone; new and moved scripts are placed without walking the scene's hierarchy order.
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		CreateLogEntity(scene);
		Entity container = scene.CreateEntity("Container");
		for (int index = 0; index < 2000; index++)
			scene.CreateChildEntity(container, "Plain");
		std::vector<Entity> scripted;
		for (int index = 0; index < 4; index++)
		{
			scripted.push_back(scene.CreateEntity("Scripted" + std::to_string(index)));
			AddScriptEntry(scripted.back(), "Lifecycle");
		}
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		RunFrames(scene, 1, 0.0f);
		const uint64_t rebuilds = system.GetUpdateOrderRebuildCount();
		const uint64_t fullBuilds = system.GetFullUpdateOrderBuildCount();
		const uint64_t orderBuilds = scene.GetHierarchyOrderBuildCount();

		for (int frame = 0; frame < 10; frame++)
		{
			scene.CreateEntity("Spawned");
			scene.CreateChildEntity(container, "SpawnedChild");
			scene.DestroyEntity(container.GetChildren().front());
			REQUIRE(scene.SetSiblingIndex(container, static_cast<size_t>(frame % 3)));
			RunFrames(scene, 1, 0.0f);
		}
		CHECK(system.GetUpdateOrderRebuildCount() == rebuilds);
		CHECK(scene.GetHierarchyOrderBuildCount() == orderBuilds);
		CHECK(GetField<int32_t>(system, scripted[0], "Lifecycle", "Updates") == 11);

		// A scripted entity spawned under the container, a moved one, a second script on another and a destroyed one: each
		// frame places them at their positions without computing the hierarchy order (the checks compute it afterwards).
		const auto runFrame = [&]()
		{
			const uint64_t builds = scene.GetHierarchyOrderBuildCount();
			ClearLog(scene);
			RunFrames(scene, 1, 0.0f);
			CHECK(scene.GetHierarchyOrderBuildCount() == builds);
			CHECK(GetUpdates(scene) == GetExpectedUpdates(scene));
		};
		Entity spawned = scene.CreateChildEntity(container, "SpawnedScript");
		AddScriptEntry(spawned, "LifecycleSecond");
		spawned.MarkModified<ScriptComponent>();
		runFrame();
		REQUIRE(scene.SetParent(scripted[3], scripted[0]));
		runFrame();
		AddScriptEntry(scripted[1], "LifecycleSecond");
		scripted[1].MarkModified<ScriptComponent>();
		scene.DestroyEntity(scripted[2]);
		REQUIRE(scene.SetSiblingIndex(scripted[1], 0));
		runFrame();
		CHECK(GetUpdates(scene) == std::vector<std::string> { "Scripted1.Lifecycle.Update", "Scripted1.LifecycleSecond.Update",
			"SpawnedScript.LifecycleSecond.Update", "Scripted0.Lifecycle.Update", "Scripted3.Lifecycle.Update" });
		CHECK(system.GetUpdateOrderRebuildCount() == rebuilds + 3);
		CHECK(system.GetFullUpdateOrderBuildCount() == fullBuilds);
		scene.OnRuntimeStop();
	}

	TEST_CASE("The update order follows random spawns, moves, activity changes and destructions")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		std::mt19937 random(31);
		Scene scene;
		CreateLogEntity(scene);
		std::vector<Entity> entities;
		int created = 0;
		const auto pick = [&]() -> Entity
		{
			std::vector<Entity> alive;
			for (const Entity entity : entities)
			{
				if (entity.IsValid())
					alive.push_back(entity);
			}
			return alive.empty() ? Entity() : alive[std::uniform_int_distribution<size_t>(0, alive.size() - 1)(random)];
		};
		const auto spawn = [&]()
		{
			const Entity parent = std::uniform_int_distribution<int>(0, 2)(random) == 0 ? Entity() : pick();
			const std::string name = "E" + std::to_string(created++);
			Entity entity = parent ? scene.CreateChildEntity(parent, name) : scene.CreateEntity(name);
			const int scripts = std::uniform_int_distribution<int>(0, 3)(random);
			if (scripts > 0)
			{
				AddScriptEntry(entity, scripts == 3 ? "LifecycleSecond" : "Lifecycle");
				if (scripts == 2)
					AddScriptEntry(entity, "LifecycleSecond");
				entity.MarkModified<ScriptComponent>();
			}
			entities.push_back(entity);
		};
		const auto operate = [&]()
		{
			const int kind = std::uniform_int_distribution<int>(0, 9)(random);
			Entity entity = pick();
			if (kind < 3 || !entity)
			{
				spawn();
			}
			else if (kind < 5)
			{
				scene.SetParent(entity, std::uniform_int_distribution<int>(0, 3)(random) == 0 ? Entity() : pick(), false);
			}
			else if (kind < 7)
			{
				scene.SetSiblingIndex(entity, std::uniform_int_distribution<size_t>(0, 6)(random));
			}
			else if (kind < 8)
			{
				entity.SetActive(!entity.IsActive());
			}
			else if (kind < 9)
			{
				scene.DestroyEntity(entity);
			}
			else if (ScriptComponent* component = entity.TryGetComponent<ScriptComponent>(); component && !component->Scripts.empty())
			{
				// A script removed, or the other one added.
				if (component->Scripts.size() == 2)
					component->Scripts.erase(component->Scripts.begin());
				else
					AddScriptEntry(entity, component->Scripts[0].ClassName == "Lifecycle" ? "LifecycleSecond" : "Lifecycle");
				entity.MarkModified<ScriptComponent>();
			}
		};

		for (int index = 0; index < 40; index++)
			spawn();
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		for (int frame = 0; frame < 300; frame++)
		{
			// Mostly a few changes per frame, sometimes a burst (more placements than are worth placing one by one).
			const int operations = frame % 25 == 24 ? 30 : std::uniform_int_distribution<int>(0, 4)(random);
			for (int operation = 0; operation < operations; operation++)
				operate();
			ClearLog(scene);
			scene.OnUpdateRuntime(0.0f);
			const std::vector<std::string> updates = GetUpdates(scene);
			const std::vector<std::string> expected = GetExpectedUpdates(scene);
			if (updates != expected)
			{
				FAIL_CHECK("Frame ", frame, ": the scripts updated in another order than the hierarchy's");
				break;
			}
		}
		// Both ways of bringing the order up to date ran.
		CHECK(system.GetFullUpdateOrderBuildCount() >= 3);
		CHECK(system.GetUpdateOrderRebuildCount() - system.GetFullUpdateOrderBuildCount() >= 100);
		CHECK(system.GetInstanceCount() > 20);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Reordering siblings while playing changes the update order at the next frame")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		CreateLogEntity(scene);
		Entity first = scene.CreateEntity("First");
		Entity second = scene.CreateEntity("Second");
		AddScriptEntry(first, "Lifecycle");
		AddScriptEntry(second, "Lifecycle");
		scene.OnRuntimeStart();
		RunFrames(scene, 1, 0.0f);

		ClearLog(scene);
		REQUIRE(scene.SetSiblingIndex(second, 0));
		scene.OnUpdateRuntime(0.0f);
		CHECK(GetLog(scene) == Events({ "Second.Lifecycle.Update", "First.Lifecycle.Update", "Second.Lifecycle.Late", "First.Lifecycle.Late" }));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Update callbacks reach only the scripts whose class implements them")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		scene.GetSettings().FixedTimestep = 0.01f;
		CreateLogEntity(scene);
		Entity entity = scene.CreateEntity("Mixed");
		AddScriptEntry(entity, "Idle");
		AddScriptEntry(entity, "HiddenCallbacks");
		AddScriptEntry(entity, "Lifecycle");
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);

		ClearLog(scene);
		scene.OnUpdateRuntime(0.01f);
		CHECK(GetLog(scene) == Events({ "Mixed.Lifecycle.Update", "Mixed.Lifecycle.Fixed", "Mixed.Lifecycle.Late" }));
		CHECK(GetField<int32_t>(system, entity, "HiddenCallbacks", "Updates") == 1);
		CHECK(system.HasInstance(entity, "Idle"));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts keep updating their own entities when destroyed entities' handles are reused")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		std::vector<Entity> fillers;
		for (int index = 0; index < 50; index++)
			fillers.push_back(scene.CreateEntity("Filler"));
		Entity kept = CreateQuietLifecycle(scene, "Kept");
		Entity doomed = CreateQuietLifecycle(scene, "Doomed");
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		RunFrames(scene, 2);

		// The destroyed entities' slots are reused by new entities; the doomed script stops, the kept one goes on.
		scene.DestroyEntity(doomed);
		for (const Entity filler : fillers)
			scene.DestroyEntity(filler);
		for (int index = 0; index < 60; index++)
			scene.CreateEntity("Newcomer");
		Entity late = CreateQuietLifecycle(scene, "Late");
		late.MarkModified<ScriptComponent>();
		RunFrames(scene, 3);
		CHECK(GetField<int32_t>(system, kept, "Lifecycle", "Updates") == 5);
		CHECK(GetField<int32_t>(system, late, "Lifecycle", "Updates") == 3);
		CHECK(system.GetInstanceCount() == 2);

		// A deactivated entity found through its cached handle receives no updates.
		kept.SetActive(false);
		RunFrames(scene, 2);
		CHECK(GetField<int32_t>(system, kept, "Lifecycle", "Updates") == 5);
		scene.OnRuntimeStop();
	}
}
