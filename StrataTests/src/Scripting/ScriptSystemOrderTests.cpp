#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"

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

}

TEST_SUITE("Scripting.Order")
{
	TEST_CASE("The update order is recomputed only after the hierarchy or the instances change")
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

		// A change of the hierarchy: recomputed once, at the next frame.
		REQUIRE(scene.SetParent(entities[7], plain));
		RunFrames(scene, 3);
		CHECK(system.GetUpdateOrderRebuildCount() == rebuilds + 1);
		rebuilds = system.GetUpdateOrderRebuildCount();

		// New and removed instances: recomputed once each time.
		AddScriptEntry(plain, "HiddenCallbacks");
		plain.MarkModified<ScriptComponent>();
		RunFrames(scene, 3);
		CHECK(system.GetUpdateOrderRebuildCount() == rebuilds + 1);
		CHECK(GetField<int32_t>(system, plain, "HiddenCallbacks", "Updates") == 3);
		plain.RemoveComponent<ScriptComponent>();
		RunFrames(scene, 3);
		CHECK(system.GetUpdateOrderRebuildCount() == rebuilds + 2);
		CHECK(system.GetInstanceCount() == 20);
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
