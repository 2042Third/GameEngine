#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/DynamicLibrary.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scripting/ScriptHostAPI.h"

#include "StrataScript/ScriptABI.h"

#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>

#include <cstddef>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	bool Near(const glm::vec3& a, const glm::vec3& b, float epsilon = 0.05f)
	{
		return glm::all(glm::epsilonEqual(a, b, epsilon));
	}

	// A static box whose top face is the plane y = 0.
	Entity CreateGround(Scene& scene)
	{
		Entity ground = scene.CreateEntity("Ground");
		ground.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 20.0f, 0.5f, 20.0f };
		return ground;
	}

	// A dynamic unit cube at `position`.
	Entity CreateCube(Scene& scene, const std::string& name, const glm::vec3& position)
	{
		Entity cube = scene.CreateEntity(name);
		cube.GetComponent<TransformComponent>().Translation = position;
		cube.AddComponent<RigidBodyComponent>();
		cube.AddComponent<BoxColliderComponent>();
		return cube;
	}

	bool Contains(std::string_view text, std::string_view part)
	{
		return text.find(part) != std::string_view::npos;
	}

	void TeleportAway(Scene& scene, Entity entity, const glm::vec3& position)
	{
		PhysicsSystem* physics = scene.GetSystem<PhysicsSystem>();
		REQUIRE(physics != nullptr);
		REQUIRE(physics->Teleport(entity, position, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
	}

}

TEST_SUITE("Scripting.Collisions")
{
	TEST_CASE("Both entities of a contact receive collision callbacks, with the normal pointing at the other")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		CreateLogEntity(scene);
		Entity ground = CreateGround(scene);
		AddScriptEntry(ground, "ContactRecorder");
		Entity cube = CreateCube(scene, "Cube", { 0.0f, 0.55f, 0.0f });
		AddScriptEntry(cube, "ContactRecorder");
		scene.OnRuntimeStart();
		RunFrames(scene, 30);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<int32_t>(system, cube, "ContactRecorder", "CollisionEnters") == 1);
		CHECK(GetField<int32_t>(system, ground, "ContactRecorder", "CollisionEnters") == 1);
		CHECK(GetField<int32_t>(system, cube, "ContactRecorder", "CollisionExits") == 0);
		CHECK(GetField<int32_t>(system, cube, "ContactRecorder", "TriggerEnters") == 0);
		CHECK(GetField<UUID>(system, cube, "ContactRecorder", "LastOther") == ground.GetUUID());
		CHECK(GetField<UUID>(system, ground, "ContactRecorder", "LastOther") == cube.GetUUID());
		CHECK(GetField<bool>(system, cube, "ContactRecorder", "LastOtherValid"));
		CHECK(Near(GetField<glm::vec3>(system, cube, "ContactRecorder", "LastNormal"), glm::vec3(0.0f, -1.0f, 0.0f)));
		CHECK(Near(GetField<glm::vec3>(system, ground, "ContactRecorder", "LastNormal"), glm::vec3(0.0f, 1.0f, 0.0f)));
		const glm::vec3 point = GetField<glm::vec3>(system, cube, "ContactRecorder", "LastPoint");
		CHECK(std::abs(point.y) < 0.05f);
		CHECK(Near(point, GetField<glm::vec3>(system, ground, "ContactRecorder", "LastPoint"), 1e-4f));

		// Parting ends the contact for both.
		TeleportAway(scene, cube, { 0.0f, 5.0f, 0.0f });
		RunFrames(scene, 2);
		CHECK(GetField<int32_t>(system, cube, "ContactRecorder", "CollisionExits") == 1);
		CHECK(GetField<int32_t>(system, ground, "ContactRecorder", "CollisionExits") == 1);
		CHECK(GetField<UUID>(system, cube, "ContactRecorder", "LastOther") == ground.GetUUID());
		const std::string log = GetLog(scene);
		CHECK(Contains(log, "Cube.ContactRecorder.CollisionEnter;"));
		CHECK(Contains(log, "Ground.ContactRecorder.CollisionExit;"));
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Every contact that began ends for the same scripts, also when an entity was deactivated meanwhile")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		CreateLogEntity(scene);
		Entity ground = CreateGround(scene);
		AddScriptEntry(ground, "ContactRecorder");
		Entity cube = CreateCube(scene, "Cube", { 0.0f, 0.55f, 0.0f });
		AddScriptEntry(cube, "ContactRecorder");
		scene.OnRuntimeStart();
		RunFrames(scene, 30);
		ScriptSystem& system = GetScriptSystem(scene);
		REQUIRE(GetField<int32_t>(system, cube, "ContactRecorder", "CollisionEnters") == 1);

		// A script added during the contact knows of no contact that began before it existed.
		REQUIRE(system.AddScript(cube, "LateContactProbe"));
		RunFrames(scene, 1);

		// Deactivating the resting cube takes its body out of the simulation: the contact ends for both entities' scripts
		// that saw it begin (pooled objects get their Exit before they are reused).
		cube.SetActive(false);
		RunFrames(scene, 2);
		CHECK(GetField<int32_t>(system, ground, "ContactRecorder", "CollisionExits") == 1);
		CHECK(GetField<int32_t>(system, cube, "ContactRecorder", "CollisionExits") == 1);
		CHECK(Contains(GetLog(scene), "Cube.ContactRecorder.CollisionExit;"));
		CHECK(GetField<int32_t>(system, cube, "LateContactProbe", "Exits") == 0);

		// Active again, the cube touches the ground again: a new contact for every script.
		cube.SetActive(true);
		RunFrames(scene, 5);
		CHECK(GetField<int32_t>(system, cube, "ContactRecorder", "CollisionEnters") == 2);
		CHECK(GetField<int32_t>(system, ground, "ContactRecorder", "CollisionEnters") == 2);
		CHECK(GetField<int32_t>(system, cube, "LateContactProbe", "Enters") == 1);
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}
	TEST_CASE("Triggers report trigger callbacks to both entities")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		CreateLogEntity(scene);
		Entity ground = CreateGround(scene);
		Entity zone = scene.CreateEntity("Zone");
		zone.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
		RigidBodyComponent& zoneBody = zone.AddComponent<RigidBodyComponent>();
		zoneBody.Type = RigidBodyType::Static;
		zoneBody.IsTrigger = true;
		zone.AddComponent<BoxColliderComponent>().HalfExtents = { 2.0f, 1.0f, 2.0f };
		AddScriptEntry(zone, "ContactRecorder");
		Entity visitor = CreateCube(scene, "Visitor", { 0.0f, 3.0f, 0.0f });
		AddScriptEntry(visitor, "ContactRecorder");
		scene.OnRuntimeStart();
		RunFrames(scene, 90);

		// The visitor fell through the trigger onto the ground.
		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<int32_t>(system, visitor, "ContactRecorder", "TriggerEnters") == 1);
		CHECK(GetField<int32_t>(system, zone, "ContactRecorder", "TriggerEnters") == 1);
		CHECK(GetField<UUID>(system, zone, "ContactRecorder", "LastOther") == visitor.GetUUID());
		CHECK(GetField<int32_t>(system, zone, "ContactRecorder", "CollisionEnters") == 0);
		CHECK(GetField<int32_t>(system, visitor, "ContactRecorder", "CollisionEnters") == 1);
		CHECK(GetField<UUID>(system, visitor, "ContactRecorder", "LastOther") == ground.GetUUID());
		CHECK(GetField<int32_t>(system, visitor, "ContactRecorder", "TriggerExits") == 0);

		TeleportAway(scene, visitor, { 10.0f, 5.0f, 0.0f });
		RunFrames(scene, 2);
		CHECK(GetField<int32_t>(system, visitor, "ContactRecorder", "TriggerExits") == 1);
		CHECK(GetField<int32_t>(system, zone, "ContactRecorder", "TriggerExits") == 1);
		CHECK(GetField<int32_t>(system, visitor, "ContactRecorder", "CollisionExits") == 1);
		CHECK(GetField<int32_t>(system, zone, "ContactRecorder", "CollisionExits") == 0);
		CHECK(Contains(GetLog(scene), "Zone.ContactRecorder.TriggerExit;"));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts may destroy their own entity inside a contact callback")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		CreateLogEntity(scene);
		Entity ground = CreateGround(scene);
		AddScriptEntry(ground, "ContactRecorder");
		Entity fragile = CreateCube(scene, "Fragile", { 0.0f, 0.55f, 0.0f });
		const UUID fragileID = fragile.GetUUID();
		AddScriptEntry(fragile, "DestroySelfOnContact");
		scene.OnRuntimeStart();
		RunFrames(scene, 30);

		// Destroyed at the end of the frame of the contact (OnDestroy ran); the ground saw the contact begin and end.
		CHECK_FALSE(scene.GetEntityByUUID(fragileID).IsValid());
		const std::string log = GetLog(scene);
		CHECK(Contains(log, "Fragile.DestroySelfOnContact.CollisionEnter;Fragile.DestroySelfOnContact.Destroy;"));
		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<int32_t>(system, ground, "ContactRecorder", "CollisionEnters") == 1);
		CHECK(GetField<int32_t>(system, ground, "ContactRecorder", "CollisionExits") == 1);
		CHECK(GetField<UUID>(system, ground, "ContactRecorder", "LastOther") == fragileID);
		CHECK_FALSE(GetField<bool>(system, ground, "ContactRecorder", "LastOtherValid"));
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts may destroy the other entity inside a contact callback")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		CreateLogEntity(scene);
		CreateGround(scene);
		// A static slab above the ground, with scripts of its own that must not run once it is gone.
		Entity glass = scene.CreateEntity("Glass");
		glass.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 0.0f };
		glass.AddComponent<BoxColliderComponent>().HalfExtents = { 1.0f, 0.1f, 1.0f };
		AddScriptEntry(glass, "ContactRecorder");
		const UUID glassID = glass.GetUUID();
		Entity breaker = CreateCube(scene, "Breaker", { 0.0f, 1.65f, 0.0f });
		AddScriptEntry(breaker, "DestroyOtherOnContact");
		scene.OnRuntimeStart();
		RunFrames(scene, 90);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK_FALSE(scene.GetEntityByUUID(glassID).IsValid());
		CHECK(GetField<UUID>(system, breaker, "DestroyOtherOnContact", "Destroyed") == glassID);
		CHECK(GetField<int32_t>(system, breaker, "DestroyOtherOnContact", "Exits") == 1);
		CHECK_FALSE(GetField<bool>(system, breaker, "DestroyOtherOnContact", "ExitOtherValid"));
		// After the glass broke, the breaker fell onto the ground and destroyed nothing more.
		CHECK(GetField<int32_t>(system, breaker, "DestroyOtherOnContact", "Contacts") == 2);
		CHECK(scene.FindEntityByName("Ground").IsValid());
		CHECK(scene.GetWorldTransform(breaker)[3].y < 0.6f);
		CHECK(Contains(GetLog(scene), "Glass.ContactRecorder.CollisionEnter;"));
		CHECK_FALSE(Contains(GetLog(scene), "Glass.ContactRecorder.CollisionExit;"));
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts removed or disabled inside a contact callback receive no further callbacks")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		CreateLogEntity(scene);
		CreateGround(scene);
		Entity remover = CreateCube(scene, "Remover", { 0.0f, 0.55f, 0.0f });
		AddScriptEntry(remover, "RemoveSelfOnContact");
		Entity thrower = CreateCube(scene, "Thrower", { 3.0f, 0.55f, 0.0f });
		AddScriptEntry(thrower, "ContactThrower");
		scene.OnRuntimeStart();
		RunFrames(scene, 30);
		TeleportAway(scene, remover, { 0.0f, 5.0f, 0.0f });
		TeleportAway(scene, thrower, { 3.0f, 5.0f, 0.0f });
		RunFrames(scene, 2);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK_FALSE(system.HasInstance(remover, "RemoveSelfOnContact"));
		CHECK(Contains(GetLog(scene), "Remover.RemoveSelfOnContact.Destroy;"));
		CHECK_FALSE(Contains(GetLog(scene), "Remover.RemoveSelfOnContact.CollisionExit;"));
		// The thrower's OnCollisionEnter threw: the script is disabled and its OnCollisionExit never ran.
		CHECK(GetField<int32_t>(system, thrower, "ContactThrower", "Calls") == 1);
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}

	TEST_CASE("The SDK's contact callbacks refuse missing and truncated contacts")
	{
		DynamicLibrary library;
		REQUIRE_MESSAGE(library.Load(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)), library.GetLastError());
		const auto load = library.GetFunction<StrataScriptLoadFunction>(ST_SCRIPT_LOAD_SYMBOL);
		REQUIRE(load);
		StrataScriptModuleAPI api = {};
		api.StructSize = sizeof(StrataScriptModuleAPI);
		REQUIRE(load(&GetScriptHostAPI(), ST_SCRIPT_ABI_VERSION, &api) == StrataScriptResult_Ok);

		const StrataScriptClassDesc* counter = nullptr;
		for (uint32_t index = 0; index < api.ClassCount; index++)
		{
			const StrataScriptString name = api.Classes[index]->Name;
			if (std::string_view(name.Data, static_cast<size_t>(name.Size)) == "ContactCounter")
				counter = api.Classes[index];
		}
		REQUIRE(counter != nullptr);
		REQUIRE(ST_SCRIPT_HAS_MEMBER(StrataScriptClassDesc, counter, OnTriggerExit));
		// Only overridden callbacks are offered.
		REQUIRE(counter->OnCollisionEnter != nullptr);
		CHECK(counter->OnCollisionExit == nullptr);
		CHECK(counter->OnTriggerEnter == nullptr);
		CHECK(counter->OnTriggerExit == nullptr);

		StrataScriptInstance instance = nullptr;
		REQUIRE(counter->Create(nullptr, 0, &instance) == StrataScriptResult_Ok);
		CHECK(counter->OnCollisionEnter(instance, nullptr) == StrataScriptResult_InvalidArgument);
		StrataScriptCollision truncated = {};
		truncated.StructSize = static_cast<uint32_t>(offsetof(StrataScriptCollision, Normal));
		CHECK(counter->OnCollisionEnter(instance, &truncated) == StrataScriptResult_InvalidArgument);
		StrataScriptCollision contact = {};
		contact.StructSize = sizeof(StrataScriptCollision);
		contact.Normal[1] = 1.0f;
		CHECK(counter->OnCollisionEnter(instance, &contact) == StrataScriptResult_Ok);

		StrataScriptValue contacts = {};
		REQUIRE(counter->GetField(instance, 0, &contacts) == StrataScriptResult_Ok);
		CHECK(contacts.Type == StrataScriptValueType_Int);
		CHECK(contacts.As.Int == 1);
		StrataScriptValue normal = {};
		REQUIRE(counter->GetField(instance, 1, &normal) == StrataScriptResult_Ok);
		CHECK(normal.As.Vector[1] == 1.0f);
		CHECK(counter->Destroy(instance) == StrataScriptResult_Ok);
		CHECK(api.Unload() == StrataScriptResult_Ok);
	}
}
