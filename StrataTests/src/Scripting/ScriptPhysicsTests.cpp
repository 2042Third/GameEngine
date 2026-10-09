#include <doctest/doctest.h>

#include "Scripting/ScriptTestUtils.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Scene/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The scene the physics test scripts expect (StrataTests/Scripts/API/PhysicsScripts.cpp), with "Tester" running
	// `className`.
	Entity CreatePhysicsScene(Scene& scene, const std::string& className)
	{
		Entity ground = scene.CreateEntity("Ground");
		ground.GetComponent<TransformComponent>().Translation = { 0.0f, -0.5f, 0.0f };
		ground.AddComponent<BoxColliderComponent>().HalfExtents = { 10.0f, 0.5f, 10.0f };

		Entity crate = scene.CreateEntity("Crate");
		crate.GetComponent<TransformComponent>().Translation = { 0.0f, 5.0f, 0.0f };
		RigidBodyComponent& crateBody = crate.AddComponent<RigidBodyComponent>();
		crateBody.GravityScale = 0.0f;
		crateBody.LinearDamping = 0.0f;
		crateBody.AngularDamping = 0.0f;
		crateBody.Layer = 2;
		crate.AddComponent<BoxColliderComponent>();

		Entity platform = scene.CreateEntity("Platform");
		platform.GetComponent<TransformComponent>().Translation = { 5.0f, 2.0f, 0.0f };
		platform.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		platform.AddComponent<BoxColliderComponent>();

		Entity zone = scene.CreateEntity("Zone");
		zone.GetComponent<TransformComponent>().Translation = { -5.0f, 1.0f, 0.0f };
		RigidBodyComponent& zoneBody = zone.AddComponent<RigidBodyComponent>();
		zoneBody.Type = RigidBodyType::Static;
		zoneBody.IsTrigger = true;
		zoneBody.Layer = 3;
		zone.AddComponent<BoxColliderComponent>().HalfExtents = { 1.0f, 1.0f, 1.0f };

		scene.CreateEntity("Plain");
		Entity tester = scene.CreateEntity("Tester");
		AddScriptEntry(tester, className);
		return tester;
	}

	bool Near(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-3f)
	{
		return glm::all(glm::epsilonEqual(a, b, epsilon));
	}

}

TEST_SUITE("Scripting.Physics")
{
	TEST_CASE("Scripts push bodies, teleport them and query the physics world")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		const Entity tester = CreatePhysicsScene(scene, "PhysicsAPI");
		scene.OnRuntimeStart();
		RunFrames(scene, 4);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, tester, "PhysicsAPI", "Done"));
		CheckScriptChecks(system, tester, "PhysicsAPI", 39);
		CHECK_FALSE(engine->IsFaulted());

		// The engine sees what the script did: the platform where it was teleported, the crate moving as last pushed.
		PhysicsSystem* physics = scene.GetSystem<PhysicsSystem>();
		REQUIRE(physics != nullptr);
		const Entity platform = scene.FindEntityByName("Platform");
		CHECK(Near(glm::vec3(scene.GetWorldTransform(platform)[3]), glm::vec3(5.0f, 3.0f, 0.0f)));
		const Entity crate = scene.FindEntityByName("Crate");
		CHECK(Near(physics->GetLinearVelocity(crate), glm::vec3(0.0f, 0.0f, 1.0f)));
		CHECK(Near(physics->GetAngularVelocity(crate), glm::vec3(0.0f, -6.0f, 0.0f), 1e-2f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Physics host functions refuse misuse harmlessly")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		const Entity tester = CreatePhysicsScene(scene, "PhysicsMisuse");
		scene.OnRuntimeStart();
		RunFrames(scene, 1);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, tester, "PhysicsMisuse", "Done"));
		CheckScriptChecks(system, tester, "PhysicsMisuse", 50);
		CHECK_FALSE(engine->IsFaulted());
		CHECK(Near(glm::vec3(scene.GetWorldTransform(scene.FindEntityByName("Crate"))[3]), glm::vec3(0.0f, 5.0f, 0.0f)));
		scene.OnRuntimeStop();
	}

	TEST_CASE("The SDK's physics functions degrade gracefully on engines without them")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		const Entity tester = CreatePhysicsScene(scene, "OlderEnginePhysics");
		scene.OnRuntimeStart();
		RunFrames(scene, 1);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, tester, "OlderEnginePhysics", "Done"));
		CheckScriptChecks(system, tester, "OlderEnginePhysics", 6);
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts use physics from OnCreate, also on bodies they spawn while the scene starts")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		const Entity tester = CreatePhysicsScene(scene, "PhysicsAtStart");
		scene.OnRuntimeStart();

		const ScriptSystem& system = GetScriptSystem(scene);
		CheckScriptChecks(system, tester, "PhysicsAtStart", 5);
		const Entity spawned = scene.FindEntityByName("Spawned");
		REQUIRE(spawned.IsValid());
		CHECK(GetField<UUID>(system, tester, "PhysicsAtStart", "Spawned") == spawned.GetUUID());
		CheckScriptChecks(system, spawned, "SpawnedBody", 1);
		PhysicsSystem* physics = scene.GetSystem<PhysicsSystem>();
		REQUIRE(physics != nullptr);
		CHECK(Near(physics->GetLinearVelocity(scene.FindEntityByName("Crate")), glm::vec3(1.0f, 0.0f, 0.0f)));
		CHECK(Near(physics->GetLinearVelocity(spawned), glm::vec3(0.0f, 0.0f, 2.0f)));
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Physics functions find nothing in scenes without physics, until a physics component appears")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity tester = scene.CreateEntity("Tester");
		AddScriptEntry(tester, "PhysicsWithoutWorld");
		scene.OnRuntimeStart();
		REQUIRE(scene.GetSystem<PhysicsSystem>() != nullptr);
		CHECK(scene.GetSystem<PhysicsSystem>()->GetWorld() == nullptr);
		RunFrames(scene, 1);

		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, tester, "PhysicsWithoutWorld", "Done"));
		CheckScriptChecks(system, tester, "PhysicsWithoutWorld", 13);
		CHECK(scene.GetSystem<PhysicsSystem>()->GetWorld() != nullptr);
		CHECK(Near(glm::vec3(scene.GetWorldTransform(tester)[3]), glm::vec3(0.0f)));
		scene.OnRuntimeStop();
	}
}
