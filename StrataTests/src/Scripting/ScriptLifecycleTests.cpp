#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Scene/SceneSerializer.h"

#include <string>

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

}

TEST_SUITE("Scripting.Lifecycle")
{
	TEST_CASE("Callbacks run in hierarchy order, then in entry order")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		scene.GetSettings().FixedTimestep = 0.01f;
		CreateLogEntity(scene);
		// Created out of hierarchy order on purpose: Parent > Child, then Root2.
		Entity root2 = scene.CreateEntity("Root2");
		Entity parent = scene.CreateEntity("Parent");
		Entity child = scene.CreateEntity("Child");
		scene.SetParent(child, parent);
		REQUIRE(scene.SetSiblingIndex(parent, 1));
		AddScriptEntry(root2, "Lifecycle");
		AddScriptEntry(child, "Lifecycle");
		AddScriptEntry(parent, "Lifecycle");
		AddScriptEntry(parent, "LifecycleSecond");
		AddScriptEntry(parent, "Idle");

		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		CHECK(system.GetInstanceCount() == 5);
		CHECK(GetLog(scene) == Events({ "Parent.Lifecycle.Create", "Parent.LifecycleSecond.Create", "Child.Lifecycle.Create", "Root2.Lifecycle.Create" }));

		ClearLog(scene);
		scene.OnUpdateRuntime(0.01f); // Exactly one fixed step
		CHECK(GetLog(scene) == Events({
			"Parent.Lifecycle.Update", "Parent.LifecycleSecond.Update", "Child.Lifecycle.Update", "Root2.Lifecycle.Update",
			"Parent.Lifecycle.Fixed", "Parent.LifecycleSecond.Fixed", "Child.Lifecycle.Fixed", "Root2.Lifecycle.Fixed",
			"Parent.Lifecycle.Late", "Parent.LifecycleSecond.Late", "Child.Lifecycle.Late", "Root2.Lifecycle.Late" }));

		RunFrames(scene, 2, 0.01f);
		CHECK(GetField<int32_t>(system, parent, "Lifecycle", "Creates") == 1);
		CHECK(GetField<int32_t>(system, parent, "Lifecycle", "Updates") == 3);
		CHECK(GetField<int32_t>(system, parent, "Lifecycle", "FixedUpdates") == 3);
		CHECK(GetField<int32_t>(system, parent, "Lifecycle", "LateUpdates") == 3);
		CHECK(GetField<int32_t>(system, parent, "LifecycleSecond", "Updates") == 3);

		// The order follows the hierarchy as it is at the start of each frame.
		scene.SetParent(child, Entity());
		ClearLog(scene);
		scene.OnUpdateRuntime(0.0f); // No fixed step
		CHECK(GetLog(scene) == Events({
			"Parent.Lifecycle.Update", "Parent.LifecycleSecond.Update", "Root2.Lifecycle.Update", "Child.Lifecycle.Update",
			"Parent.Lifecycle.Late", "Parent.LifecycleSecond.Late", "Root2.Lifecycle.Late", "Child.Lifecycle.Late" }));

		// Stopping destroys every instance: descendants before ancestors, the last script of an entity first.
		scene.SetParent(child, parent);
		ClearLog(scene);
		scene.OnRuntimeStop();
		CHECK(GetLog(scene) == Events({ "Root2.Lifecycle.Destroy", "Child.Lifecycle.Destroy", "Parent.LifecycleSecond.Destroy", "Parent.Lifecycle.Destroy" }));
		CHECK(scene.GetSystem<ScriptSystem>() == nullptr);
	}

	TEST_CASE("Private and protected callback overrides run")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		const ScriptClassInfo* info = engine->FindClass("HiddenCallbacks");
		REQUIRE(info);
		CHECK(info->Implements(ScriptCallback::OnCreate));
		CHECK(info->Implements(ScriptCallback::OnUpdate));
		CHECK_FALSE(info->Implements(ScriptCallback::OnLateUpdate));

		Scene scene;
		Entity entity = scene.CreateEntity("Hidden");
		AddScriptEntry(entity, "HiddenCallbacks");
		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);
		RunFrames(scene, 2);
		CHECK(GetField<int32_t>(system, entity, "HiddenCallbacks", "Creates") == 1);
		CHECK(GetField<int32_t>(system, entity, "HiddenCallbacks", "Updates") == 2);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Inactive entities receive no updates")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity parent = scene.CreateEntity("Parent");
		Entity child = scene.CreateChildEntity(parent, "Child");
		for (Entity entity : { parent, child })
		{
			ScriptEntry& entry = AddScriptEntry(entity, "Lifecycle");
			AddFieldOverride(entry, "RecordUpdates", PropertyType::Bool, false);
		}

		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		RunFrames(scene, 2);
		parent.SetActive(false);
		RunFrames(scene, 3);
		CHECK(GetField<int32_t>(system, parent, "Lifecycle", "Updates") == 2);
		CHECK(GetField<int32_t>(system, child, "Lifecycle", "Updates") == 2);
		CHECK(GetField<int32_t>(system, child, "Lifecycle", "LateUpdates") == 2);

		parent.SetActive(true);
		child.SetActive(false);
		RunFrames(scene, 1);
		CHECK(GetField<int32_t>(system, parent, "Lifecycle", "Updates") == 3);
		CHECK(GetField<int32_t>(system, child, "Lifecycle", "Updates") == 2);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Script components changed while playing create and destroy instances")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		CreateLogEntity(scene);
		Entity first = scene.CreateEntity("First");
		Entity second = scene.CreateEntity("Second");
		AddScriptEntry(first, "Lifecycle");
		AddScriptEntry(first, "NoSuchClass"); // Unknown classes are skipped with a warning
		AddScriptEntry(first, "Lifecycle");   // Duplicates too

		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		CHECK(system.GetInstanceCount() == 1);

		// A script added by the editor (or any engine code) while playing.
		ClearLog(scene);
		AddScriptEntry(second, "Lifecycle");
		second.MarkModified<ScriptComponent>();
		scene.OnUpdateRuntime(0.0f);
		CHECK(GetLog(scene) == Events({ "Second.Lifecycle.Create", "First.Lifecycle.Update", "Second.Lifecycle.Update", "First.Lifecycle.Late", "Second.Lifecycle.Late" }));
		CHECK(system.GetInstanceCount() == 2);

		// Removing the component calls OnDestroy while the entity is still valid.
		ClearLog(scene);
		second.RemoveComponent<ScriptComponent>();
		scene.OnUpdateRuntime(0.0f);
		CHECK(GetLog(scene) == Events({ "Second.Lifecycle.Destroy", "First.Lifecycle.Update", "First.Lifecycle.Late" }));
		CHECK(system.GetInstanceCount() == 1);

		// Destroying an entity outside the update (e.g. from the editor) calls OnDestroy right away, entity still valid.
		ClearLog(scene);
		scene.DestroyEntity(first);
		CHECK(GetLog(scene) == Events({ "First.Lifecycle.Destroy" }));
		CHECK(system.GetInstanceCount() == 0);
		scene.OnRuntimeStop();
		CHECK(GetLog(scene) == Events({ "First.Lifecycle.Destroy" }));
	}

	TEST_CASE("Scripts create and destroy entities and scripts during updates")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		CreateLogEntity(scene);
		Entity destroyer = scene.CreateEntity("Destroyer");
		AddScriptEntry(destroyer, "Destroyer");
		Entity victim = scene.CreateEntity("Victim");
		Entity victimChild = scene.CreateChildEntity(victim, "VictimChild");
		AddScriptEntry(victim, "Lifecycle");
		AddScriptEntry(victimChild, "Lifecycle");
		Entity adder = scene.CreateEntity("Adder");
		AddScriptEntry(adder, "ScriptAdder");

		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		ClearLog(scene);
		const size_t entityCount = scene.GetEntityCount();

		// Frame 1: the victim (and its child) are destroyed at the end of the frame; their scripts see OnDestroy first,
		// descendants before ancestors, while the entities are still valid. Scripts added by scripts start before their
		// first update.
		scene.OnUpdateRuntime(0.0f);
		CheckScriptChecks(system, destroyer, "Destroyer", 3);
		CheckScriptChecks(system, adder, "ScriptAdder", 5);
		CHECK_FALSE(victim.IsValid());
		CHECK_FALSE(victimChild.IsValid());
		const Entity spawned = scene.GetEntityByUUID(GetField<UUID>(system, destroyer, "Destroyer", "Spawned"));
		REQUIRE(spawned.IsValid());
		CHECK(system.HasInstance(spawned, "Lifecycle"));
		CHECK(system.HasInstance(adder, "Lifecycle"));
		CHECK(scene.GetEntityCount() == entityCount - 2 + 1);
		CHECK(GetLog(scene) == Events({
			"Victim.Lifecycle.Update", "VictimChild.Lifecycle.Update",
			"Spawned.Lifecycle.Create", "Adder.Lifecycle.Create",
			"Victim.Lifecycle.Late", "VictimChild.Lifecycle.Late", "Spawned.Lifecycle.Late", "Adder.Lifecycle.Late",
			"VictimChild.Lifecycle.Destroy", "Victim.Lifecycle.Destroy" }));

		// Frame 2: the destroyer destroys itself; the adder removes its Lifecycle script (OnDestroy at the next sync point).
		ClearLog(scene);
		scene.OnUpdateRuntime(0.0f);
		CHECK_FALSE(destroyer.IsValid());
		CHECK_FALSE(system.HasInstance(adder, "Lifecycle"));
		CHECK(GetLog(scene) == Events({ "Spawned.Lifecycle.Update", "Adder.Lifecycle.Destroy", "Spawned.Lifecycle.Late" }));
		CheckScriptChecks(system, adder, "ScriptAdder", 8);
		CHECK(system.GetInstanceCount() == 2);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts removed while the frame's destruction is flushed are destroyed when the scene stops")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		const LiveInstanceCounter liveInstances(*engine);
		const int64_t baseline = liveInstances.Get();
		Scene scene;
		CreateLogEntity(scene);
		Entity remover = scene.CreateEntity("Remover");
		ScriptEntry& removerEntry = AddScriptEntry(remover, "RemoveOnDestroy");
		AddFieldOverride(removerEntry, "Target", PropertyType::String, std::string("Victim"));
		AddFieldOverride(removerEntry, "DestroySelf", PropertyType::Bool, true);
		Entity victim = scene.CreateEntity("Victim");
		AddFieldOverride(AddScriptEntry(victim, "Lifecycle"), "RecordUpdates", PropertyType::Bool, false);

		scene.OnRuntimeStart();
		CHECK(liveInstances.Get() == baseline + 1);
		ClearLog(scene);
		// The remover destroys its entity; its OnDestroy, while the scene flushes destruction at the end of the frame,
		// removes the victim's script. The instance is destroyed at the next sync point - here, when the scene stops.
		scene.OnUpdateRuntime(0.0f);
		CHECK_FALSE(remover.IsValid());
		CHECK_FALSE(GetScriptSystem(scene).HasInstance(victim, "Lifecycle"));
		CHECK(GetLog(scene).empty());

		scene.OnRuntimeStop();
		CHECK(GetLog(scene) == Events({ "Victim.Lifecycle.Destroy" }));
		CHECK(liveInstances.Get() == baseline);
	}

	TEST_CASE("Scripts removed while the frame's destruction is flushed are destroyed by a reload")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		CreateLogEntity(scene);
		Entity remover = scene.CreateEntity("Remover");
		ScriptEntry& removerEntry = AddScriptEntry(remover, "RemoveOnDestroy");
		AddFieldOverride(removerEntry, "Target", PropertyType::String, std::string("Victim"));
		AddFieldOverride(removerEntry, "DestroySelf", PropertyType::Bool, true);
		Entity victim = scene.CreateEntity("Victim");
		AddFieldOverride(AddScriptEntry(victim, "Lifecycle"), "RecordUpdates", PropertyType::Bool, false);

		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		ClearLog(scene);
		scene.OnUpdateRuntime(0.0f);
		CHECK(GetLog(scene).empty());

		// The removed script ends in the old module, with OnDestroy, and is not carried over.
		REQUIRE(engine->Reload());
		CHECK(GetLog(scene) == Events({ "Victim.Lifecycle.Destroy" }));
		scene.OnUpdateRuntime(0.0f);
		CHECK_FALSE(system.HasInstance(victim, "Lifecycle"));
		CHECK(system.GetInstanceCount() == 0);
		scene.OnRuntimeStop();
		CHECK(GetLog(scene) == Events({ "Victim.Lifecycle.Destroy" }));
	}

	TEST_CASE("A script removed and added again while it cannot be constructed is still destroyed")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		const LiveInstanceCounter liveInstances(*engine);
		const int64_t baseline = liveInstances.Get();
		Scene scene;
		CreateLogEntity(scene);
		Entity entity = scene.CreateEntity("Entity");
		AddScriptEntry(entity, "Readder");
		AddFieldOverride(AddScriptEntry(entity, "Fragile"), "RecordUpdates", PropertyType::Bool, false);

		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		CHECK(liveInstances.Get() == baseline + 1);
		ClearLog(scene);

		// Readder removes Fragile, then fails to add it again. The removed instance gets OnDestroy and is deleted at the
		// end of the update; the entry the failed AddScript left behind gets a new instance once Fragile constructs again.
		scene.OnUpdateRuntime(0.0f);
		CheckScriptChecks(system, entity, "Readder", 2);
		CHECK(GetLog(scene).starts_with(Events({ "Entity.Fragile.Destroy", "Entity.Fragile.Create" })));
		CHECK(system.HasInstance(entity, "Fragile"));
		CHECK(liveInstances.Get() == baseline + 1);

		scene.OnRuntimeStop();
		CHECK(liveInstances.Get() == baseline);
	}

	TEST_CASE("Scripts removed when a sync point stops early are destroyed")
	{
		// Each Replicator generation creates the next and removes the previous one: far more than one sync point runs, so
		// sync points stop early with a removal pending.
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		const LiveInstanceCounter liveInstances(*engine);
		const int64_t baseline = liveInstances.Get();
		Scene scene;
		Entity origin = scene.CreateEntity("Origin");
		AddFieldOverride(AddScriptEntry(origin, "Replicator"), "MaxGenerations", PropertyType::Int, int32_t(300));

		scene.OnRuntimeStart();
		RunFrames(scene, 10);
		// Only the last generation is left; every other instance was deleted.
		CHECK(GetScriptSystem(scene).GetInstanceCount() == 1);
		CHECK(liveInstances.Get() == baseline + 1);

		scene.OnRuntimeStop();
		CHECK(liveInstances.Get() == baseline);
	}

	TEST_CASE("Spawning many scripted entities in one update takes work linear in their number")
	{
		constexpr int32_t c_Count = 2000;
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity spawner = scene.CreateEntity("Spawner");
		AddFieldOverride(AddScriptEntry(spawner, "MassSpawner"), "Count", PropertyType::Int, c_Count);
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);
		const uint64_t reconcilesBefore = system.GetReconcileCount();

		// Every AddScript creates its instance right away; each spawned entity must be looked at a bounded number of times
		// (once when its script is added, once at the next sync point), not once per entity spawned after it.
		scene.OnUpdateRuntime(0.0f);
		CHECK(GetField<int32_t>(system, spawner, "MassSpawner", "Spawned") == c_Count);
		CHECK(system.GetInstanceCount() == static_cast<size_t>(c_Count) + 1);
		const uint64_t reconciles = system.GetReconcileCount() - reconcilesBefore;
		INFO("Entities reconciled: ", reconciles);
		CHECK(reconciles <= 3 * static_cast<uint64_t>(c_Count));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Restarting creates fresh instances; play copies leave the edited scene alone")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Ref<Scene> edited = CreateRef<Scene>();
		Entity entity = edited->CreateEntity("Scripted");
		ScriptEntry& entry = AddScriptEntry(entity, "Lifecycle");
		AddFieldOverride(entry, "RecordUpdates", PropertyType::Bool, false);

		for (int session = 0; session < 2; session++)
		{
			Ref<Scene> play = Scene::Copy(edited);
			play->OnRuntimeStart();
			ScriptSystem& system = GetScriptSystem(*play);
			const Entity copy = play->GetEntityByUUID(entity.GetUUID());
			RunFrames(*play, 3);
			CHECK(GetField<int32_t>(system, copy, "Lifecycle", "Creates") == 1);
			CHECK(GetField<int32_t>(system, copy, "Lifecycle", "Updates") == 3);
			play->OnRuntimeStop();
		}
		CHECK_FALSE(edited->IsRunning());
		CHECK(entity.GetComponent<ScriptComponent>().Scripts.size() == 1);
	}
}

TEST_SUITE("Scripting.Fields")
{
	TEST_CASE("Field overrides are applied before OnCreate")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity target = scene.CreateEntity("Target");
		Entity entity = scene.CreateEntity("Fields");
		ScriptEntry& entry = AddScriptEntry(entity, "FieldTypes");
		const glm::quat rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		AddFieldOverride(entry, "BoolField", PropertyType::Bool, false);
		AddFieldOverride(entry, "IntField", PropertyType::Int, int32_t(-7));
		AddFieldOverride(entry, "FloatField", PropertyType::Float, 2.25f);
		AddFieldOverride(entry, "Vec2Field", PropertyType::Vec2, glm::vec2(5.0f, 6.0f));
		AddFieldOverride(entry, "Vec3Field", PropertyType::Vec3, glm::vec3(7.0f, 8.0f, 9.0f));
		AddFieldOverride(entry, "Vec4Field", PropertyType::Vec4, glm::vec4(1.0f, 0.0f, 1.0f, 0.5f));
		AddFieldOverride(entry, "QuatField", PropertyType::Quat, rotation);
		AddFieldOverride(entry, "StringField", PropertyType::String, std::string("Overridden \xC3\xA9"));
		AddFieldOverride(entry, "EntityField", PropertyType::Entity, target.GetUUID());
		AddFieldOverride(entry, "AssetField", PropertyType::Asset, UUID(0xABCDEF));

		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, entity, "FieldTypes", "BoolField") == false);
		CHECK(GetField<int32_t>(system, entity, "FieldTypes", "IntField") == -7);
		CHECK(GetField<float>(system, entity, "FieldTypes", "FloatField") == 2.25f);
		CHECK(GetField<glm::vec2>(system, entity, "FieldTypes", "Vec2Field") == glm::vec2(5.0f, 6.0f));
		CHECK(GetField<glm::vec3>(system, entity, "FieldTypes", "Vec3Field") == glm::vec3(7.0f, 8.0f, 9.0f));
		CHECK(GetField<glm::vec4>(system, entity, "FieldTypes", "Vec4Field") == glm::vec4(1.0f, 0.0f, 1.0f, 0.5f));
		CHECK(GetField<glm::quat>(system, entity, "FieldTypes", "QuatField") == rotation);
		CHECK(GetField<std::string>(system, entity, "FieldTypes", "StringField") == "Overridden \xC3\xA9");
		CHECK(GetField<UUID>(system, entity, "FieldTypes", "EntityField") == target.GetUUID());
		CHECK(GetField<UUID>(system, entity, "FieldTypes", "AssetField") == UUID(0xABCDEF));
		CHECK(GetField<int32_t>(system, entity, "FieldTypes", "IntSeenInCreate") == -7);
		CHECK(GetField<std::string>(system, entity, "FieldTypes", "StringSeenInCreate") == "Overridden \xC3\xA9");
		scene.OnRuntimeStop();
	}

	TEST_CASE("Unknown and mistyped field overrides are ignored")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity entity = scene.CreateEntity("Fields");
		ScriptEntry& entry = AddScriptEntry(entity, "FieldTypes");
		AddFieldOverride(entry, "NoSuchField", PropertyType::Int, int32_t(1));
		AddFieldOverride(entry, "IntField", PropertyType::Float, 3.0f);
		AddFieldOverride(entry, "FloatField", PropertyType::Float, int32_t(3)); // Value does not match its declared type
		AddFieldOverride(entry, "StringField", PropertyType::Color3, glm::vec3(1.0f));
		AddFieldOverride(entry, "intfield", PropertyType::Int, int32_t(5)); // Field names are case-sensitive
		AddFieldOverride(entry, "BoolField", PropertyType::Enum, int32_t(1)); // Enums are integers

		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<int32_t>(system, entity, "FieldTypes", "IntField") == 42);
		CHECK(GetField<float>(system, entity, "FieldTypes", "FloatField") == 1.5f);
		CHECK(GetField<std::string>(system, entity, "FieldTypes", "StringField") == "Hello");
		CHECK(GetField<bool>(system, entity, "FieldTypes", "BoolField") == true);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Field overrides survive a scene file round trip")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene source;
		Entity target = source.CreateEntity("Target");
		Entity entity = source.CreateEntity("Fields");
		ScriptEntry& entry = AddScriptEntry(entity, "FieldTypes");
		AddFieldOverride(entry, "IntField", PropertyType::Int, int32_t(11));
		AddFieldOverride(entry, "Vec3Field", PropertyType::Vec3, glm::vec3(0.5f, 0.25f, 0.125f));
		AddFieldOverride(entry, "StringField", PropertyType::String, std::string("Saved"));
		AddFieldOverride(entry, "EntityField", PropertyType::Entity, target.GetUUID());
		AddScriptEntry(entity, "Lifecycle");

		Scene loaded;
		std::string error;
		REQUIRE_MESSAGE(SceneSerializer::Deserialize(loaded, SceneSerializer::Serialize(source), &error), error);
		const Entity loadedEntity = loaded.GetEntityByUUID(entity.GetUUID());
		REQUIRE(loadedEntity.IsValid());

		loaded.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(loaded);
		CHECK(system.HasInstance(loadedEntity, "Lifecycle"));
		CHECK(GetField<int32_t>(system, loadedEntity, "FieldTypes", "IntField") == 11);
		CHECK(GetField<glm::vec3>(system, loadedEntity, "FieldTypes", "Vec3Field") == glm::vec3(0.5f, 0.25f, 0.125f));
		CHECK(GetField<std::string>(system, loadedEntity, "FieldTypes", "StringField") == "Saved");
		CHECK(GetField<UUID>(system, loadedEntity, "FieldTypes", "EntityField") == target.GetUUID());
		CHECK(GetField<float>(system, loadedEntity, "FieldTypes", "FloatField") == 1.5f); // Not overridden: the default
		loaded.OnRuntimeStop();
	}

	TEST_CASE("Live field values can be read and written")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity entity = scene.CreateEntity("Fields");
		AddScriptEntry(entity, "FieldTypes");
		scene.OnRuntimeStart();
		ScriptSystem& system = GetScriptSystem(scene);

		CHECK(system.SetFieldValue(entity, "FieldTypes", "StringField", std::string("Live")));
		CHECK(GetField<std::string>(system, entity, "FieldTypes", "StringField") == "Live");
		CHECK(system.SetFieldValue(entity, "FieldTypes", "QuatField", glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
		CHECK_FALSE(system.SetFieldValue(entity, "FieldTypes", "IntField", 1.0f));
		CHECK_FALSE(system.SetFieldValue(entity, "FieldTypes", "Missing", int32_t(1)));
		CHECK_FALSE(system.SetFieldValue(entity, "Lifecycle", "Updates", int32_t(1)));
		CHECK_FALSE(system.GetFieldValue(entity, "FieldTypes", "Missing").has_value());
		CHECK_FALSE(system.GetFieldValue(Entity(), "FieldTypes", "IntField").has_value());
		CHECK(GetField<int32_t>(system, entity, "FieldTypes", "IntField") == 42);
		scene.OnRuntimeStop();
	}
}
