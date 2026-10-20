#include <doctest/doctest.h>

#include "Perf/SceneGenerators.h"
#include "Physics/PhysicsTestUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Process.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Physics/PhysicsWorld.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Prefab.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneSerializer.h"
#include "Strata/Scene/SceneServices.h"
#include "TestHelpers.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Warnings and errors logged since `logStart`.
	size_t CountLogWarnings(uint64_t logStart)
	{
		size_t count = 0;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(logStart))
		{
			if (entry.Level >= LogLevel::Warn)
				count++;
		}
		return count;
	}

	// Forgets what earlier worlds needed (process-wide, per scene) before and after a test, so tests do not size each other's
	// worlds.
	class ScopedCapacityDemands
	{
	public:
		ScopedCapacityDemands() { PhysicsSystem::ForgetCapacityDemands(); }
		~ScopedCapacityDemands() { PhysicsSystem::ForgetCapacityDemands(); }

		ScopedCapacityDemands(const ScopedCapacityDemands&) = delete;
		ScopedCapacityDemands& operator=(const ScopedCapacityDemands&) = delete;
	};

	std::vector<Entity> CreateBoxRow(Scene& scene, uint32_t count, float height = 0.5f)
	{
		std::vector<Entity> boxes;
		for (uint32_t index = 0; index < count; index++)
			boxes.push_back(CreateDynamicBox(scene, "Box " + std::to_string(index), glm::vec3(static_cast<float>(index) * 2.0f - 10.0f, height, 0.0f)));
		return boxes;
	}

	// Steps the scene until `condition` holds, at most `maxSteps` fixed steps (simulation time, the same on every machine).
	template<typename Condition>
	bool WaitForSteps(Scene& scene, uint32_t maxSteps, Condition condition)
	{
		for (uint32_t step = 0; step < maxSteps; step++)
		{
			if (condition())
				return true;
			StepScene(scene, 1);
		}
		return condition();
	}

	// Spawns `count` boxes (dynamic or static) in a grid, has the world build their bodies, and destroys them again before the
	// next step, which then removes the bodies. False if the bodies were not built.
	bool SpawnAndRemoveBoxes(Scene& scene, PhysicsSystem& physics, uint32_t count, bool dynamic)
	{
		std::vector<Entity> boxes;
		boxes.reserve(count);
		for (uint32_t index = 0; index < count; index++)
		{
			const glm::vec3 position(static_cast<float>(index % 32) * 1.5f, 1.0f, static_cast<float>(index / 32) * 1.5f);
			boxes.push_back(dynamic ? CreateDynamicBox(scene, "Spawned", position) : CreateStaticBox(scene, "Spawned", position, glm::vec3(0.5f)));
		}
		const bool built = physics.HasBody(boxes.front()) && physics.HasBody(boxes.back());
		scene.DestroyEntities(boxes);
		StepScene(scene, 1);
		return built;
	}

}

namespace Strata::Tests
{

	int RunBroadPhaseChurnProcess()
	{
#if defined(_MSC_VER)
		// A broad phase that leaked its nodes would end the process: let the exit code say so, without a dialog.
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
		LogSpecification logSpecification;
		logSpecification.Level = LogLevel::Warn;
		Log::Init(logSpecification);

		// A world reserved for 1,024 bodies. Spawning 1,000 bodies and removing them before the next step, again and again,
		// would grow its broad phase's trees without end (or exhaust a fixed node pool) unless the nodes of removed bodies are
		// freed, in the tree of moving bodies and in the tree of static ones.
		constexpr uint32_t c_Rounds = 20;
		constexpr uint32_t c_Bodies = 1000;
		int result = 0;
		{
			Scene scene("Churn");
			scene.GetSettings().PhysicsCapacity.MaxBodies = 1024;
			scene.OnRuntimeStart();
			PhysicsSystem* physics = scene.GetSystem<PhysicsSystem>();
			for (const bool dynamic : { true, false })
			{
				for (uint32_t round = 0; round < c_Rounds && physics && result == 0; round++)
				{
					if (!SpawnAndRemoveBoxes(scene, *physics, c_Bodies, dynamic))
					{
						std::printf("%s bodies were not built in round %u\n", dynamic ? "Dynamic" : "Static", round);
						result = 1;
					}
				}
			}
			if (!physics)
			{
				std::printf("The scene has no physics system\n");
				result = 1;
			}
			scene.OnRuntimeStop();
		}
		if (result == 0)
			std::printf("survived %u rounds of %u dynamic and of %u static bodies\n", c_Rounds, c_Bodies, c_Bodies);
		std::fflush(stdout);
		Log::Shutdown();
		return result;
	}

}

TEST_SUITE("Physics.Capacity")
{
	TEST_CASE("Bodies spawned and removed again before every step never exhaust the broad phase")
	{
		// A broad phase that runs out of nodes ends the process, so the spawns run in a helper process
		// (RunBroadPhaseChurnProcess).
		ProcessSpecification specification;
		specification.Executable = GetTestExecutablePath();
		specification.Arguments = { "--strata-test-helper=broad-phase-churn" };
		const Process::RunResult result = Process::Run(specification, std::chrono::minutes(5));
		INFO("Output: ", result.Output);
		REQUIRE(result.Started);
		CHECK_FALSE(result.TimedOut);
		CHECK(result.ExitCode == 0);
		CHECK(result.Output.find("survived 20 rounds") != std::string::npos);
	}

	TEST_CASE("Automatic capacities follow the bodies a scene starts with")
	{
		const PhysicsSettings automatic;
		CHECK(automatic.MaxBodies == 0);
		CHECK(automatic.MaxContactConstraints == 0);

		// Small scenes get the minimums.
		const PhysicsSettings smallSettings = PhysicsWorld::ResolveCapacities(automatic, 10);
		CHECK(smallSettings.MaxBodies == 1024);
		CHECK(smallSettings.MaxContactConstraints == 2048);

		// Large ones: the next power of two of twice the bodies, and two contacts per body.
		const PhysicsSettings large = PhysicsWorld::ResolveCapacities(automatic, 70'000);
		CHECK(large.MaxBodies == 262'144);
		CHECK(large.MaxContactConstraints == 140'000);

		// Given capacities stay.
		PhysicsSettings given;
		given.MaxBodies = 100;
		given.MaxContactConstraints = 300;
		const PhysicsSettings kept = PhysicsWorld::ResolveCapacities(given, 70'000, { 500'000, 500'000 });
		CHECK(kept.MaxBodies == 100);
		CHECK(kept.MaxContactConstraints == 300);

		// What an earlier world needed raises the automatic ones.
		const PhysicsSettings grown = PhysicsWorld::ResolveCapacities(automatic, 10, { 100'000, 400'000 });
		CHECK(grown.MaxBodies == 262'144);
		CHECK(grown.MaxContactConstraints == 400'000);

		// Nothing exceeds the limits a scene may ask for.
		const PhysicsSettings huge = PhysicsWorld::ResolveCapacities(automatic, 20'000'000);
		CHECK(huge.MaxBodies == PhysicsCapacitySettings::c_MaxBodiesLimit);
		CHECK(huge.MaxContactConstraints == PhysicsCapacitySettings::c_MaxContactConstraintsLimit);
		// The automatic sizing has room for a scene full of bodies.
		const PhysicsSettings full = PhysicsWorld::ResolveCapacities(automatic, static_cast<uint32_t>(Scene::c_MaxEntities));
		CHECK(full.MaxBodies == PhysicsCapacitySettings::c_MaxBodiesLimit);
		CHECK(full.MaxBodies >= 2 * Scene::c_MaxEntities);
	}

	TEST_CASE("A scene's capacities size its world, before the default settings and automatic sizing")
	{
		ScopedCapacityDemands demands;
		Scene scene("Capacities");
		CreateGround(scene);
		CreateBoxRow(scene, 3);
		scene.GetSettings().PhysicsCapacity.MaxBodies = 16;
		scene.GetSettings().PhysicsCapacity.MaxContactConstraints = 64;

		scene.OnRuntimeStart();
		PhysicsStats stats = GetPhysics(scene).GetStats();
		CHECK(stats.MaxBodies == 16);
		CHECK(stats.MaxContactConstraints == 64);
		CHECK(stats.BodyCount == 4);
		scene.OnRuntimeStop();

		// Without scene capacities, the default settings give what they set and the rest is automatic.
		scene.GetSettings().PhysicsCapacity = {};
		PhysicsSettings defaults;
		defaults.MaxContactConstraints = 1000;
		ScopedPhysicsSettings scopedDefaults(defaults);
		scene.OnRuntimeStart();
		stats = GetPhysics(scene).GetStats();
		CHECK(stats.MaxBodies == 1024);
		CHECK(stats.MaxContactConstraints == 1000);
		CHECK(stats.Demand.IsEmpty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("At its growth limit a full world keeps bodies waiting, reported in one error, counted and remembered")
	{
		ScopedCapacityDemands demands;
		PhysicsSettings limited;
		limited.GrowthLimit = 4;
		ScopedPhysicsSettings scopedLimit(limited);
		Scene scene("Crowded");
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		scene.GetSettings().PhysicsCapacity.MaxBodies = 4;
		const std::vector<Entity> boxes = CreateBoxRow(scene, 10);

		// Four boxes get a body; the six others wait for a slot, named in a single error.
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		PhysicsStats stats = physics.GetStats();
		CHECK(stats.BodyCount == 4);
		CHECK(stats.BodyOverflowCount == 6);
		CHECK(stats.PendingBodyCount == 6);
		CHECK(stats.Demand.Bodies == 10);
		CHECK(CountLogMessages(logStart, "cannot create a body") == 1);
		CHECK(CountLogMessages(logStart, "'Box 4' and 5 more entities: the world is at its growth limit (PhysicsSettings::GrowthLimit) of 4 bodies and cannot grow") == 1);

		// Waiting entities get the slots of removed bodies; while some still wait, nothing is logged again, and the world
		// never grows.
		StepScene(scene, 5);
		scene.DestroyEntities(std::vector<Entity>(boxes.begin(), boxes.begin() + 3));
		StepScene(scene, 2);
		stats = physics.GetStats();
		CHECK(stats.BodyCount == 4);
		CHECK(stats.BodyOverflowCount == 3);
		CHECK(stats.GrowthCount == 0);
		CHECK(stats.MaxBodies == 4);
		CHECK(CountLogMessages(logStart, "cannot create a body") == 1);

		// Once every entity has a body, a new overflow is reported again.
		scene.DestroyEntities(std::vector<Entity>(boxes.begin() + 3, boxes.begin() + 6));
		StepScene(scene, 2);
		stats = physics.GetStats();
		CHECK(stats.BodyCount == 4);
		CHECK(stats.BodyOverflowCount == 0);
		CreateBoxRow(scene, 2, 5.0f);
		CHECK_FALSE(physics.HasBody(scene.FindEntityByName("Box 1")));
		CHECK(physics.GetStats().BodyOverflowCount == 2);
		CHECK(CountLogMessages(logStart, "cannot create a body") == 2);
		CHECK(CountLogMessages(logStart, "'Box 0' and 1 more entities") == 1);

		// What the world lacked is remembered for the scene when it stops.
		CHECK(PhysicsSystem::GetRememberedCapacityDemand(scene.GetLineageID()).IsEmpty());
		scene.OnRuntimeStop();
		CHECK(PhysicsSystem::GetRememberedCapacityDemand(scene.GetLineageID()).Bodies == 10);
		const Scene other("Crowded"); // The same name, another scene
		CHECK(PhysicsSystem::GetRememberedCapacityDemand(other.GetLineageID()).IsEmpty());
	}

	TEST_CASE("A world grows past its reserved room in place, without a warning, and remembers it")
	{
		ScopedCapacityDemands demands;
		Scene scene("Growing");
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		scene.GetSettings().PhysicsCapacity.MaxBodies = 4;
		const std::vector<Entity> boxes = CreateBoxRow(scene, 10);

		// Every body is built at once: box3d grows its storage, and the world counts one growth of its room (no error, no
		// warning).
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		PhysicsStats stats = physics.GetStats();
		CHECK(stats.BodyCount == 10);
		CHECK(stats.BodyOverflowCount == 0);
		CHECK(stats.MaxBodies == 16); // The next power of two of twice the room and of the bodies
		CHECK(stats.GrowthCount == 1);
		CHECK(stats.LastGrowthFrom == 4);
		CHECK(stats.LastGrowthTo == 16);
		CHECK(stats.LastGrowthMs >= 0.0f);
		CHECK(stats.Demand.Bodies == 16);
		CHECK(CountLogMessages(logStart, "cannot create a body") == 0);
		CHECK(CountLogWarnings(logStart) == 0);
		for (const Entity box : boxes)
			CHECK(physics.HasBody(box));

		// The bodies behave as before: queries find them, new ones are built, removed ones go.
		CHECK(physics.Raycast(glm::vec3(-10.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f).has_value());
		CreateBoxRow(scene, 3, 10.0f);
		scene.DestroyEntities(std::vector<Entity>(boxes.begin(), boxes.begin() + 2));
		StepScene(scene, 2);
		stats = physics.GetStats();
		CHECK(stats.BodyCount == 11);
		CHECK(stats.GrowthCount == 1);
		scene.OnRuntimeStop();
		// The scene's next world is reserved for what this one grew to.
		CHECK(PhysicsSystem::GetRememberedCapacityDemand(scene.GetLineageID()).Bodies == 16);
	}

	TEST_CASE("Growth keeps every body's ID, motion, sleep and touching pairs")
	{
		// The scale audit's streamed cells, scaled down for Debug runs: a world created for 4,096 bodies, four "cells" of
		// 2,500 static boxes arriving at once, while 100 bodies move with known velocities and 20 rest asleep on a floor.
		ScopedCapacityDemands demands;
		ScopedJobSystem jobs(3);
		Scene scene("Streamed");
		scene.GetSettings().PhysicsCapacity.MaxBodies = 4096;
		CreateGround(scene);
		std::vector<Entity> resting;
		for (uint32_t index = 0; index < 20; index++)
			resting.push_back(CreateDynamicBox(scene, "Resting " + std::to_string(index), glm::vec3(static_cast<float>(index) * 2.0f - 20.0f, 0.5f, 10.0f)));
		std::vector<Entity> moving;
		for (uint32_t index = 0; index < 100; index++)
		{
			Entity sphere = CreateDynamicSphere(scene, "Moving " + std::to_string(index), glm::vec3(static_cast<float>(index % 10) * 3.0f - 15.0f, 50.0f + static_cast<float>(index / 10) * 3.0f, -20.0f));
			RigidBodyComponent& body = sphere.GetComponent<RigidBodyComponent>();
			body.GravityScale = 0.0f;
			body.LinearDamping = 0.0f;
			body.AngularDamping = 0.0f;
			moving.push_back(sphere);
		}

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics); // Tracks every touching pair
		for (uint32_t index = 0; index < moving.size(); index++)
		{
			REQUIRE(physics.SetLinearVelocity(moving[index], glm::vec3(0.01f * static_cast<float>(index), 0.0f, 0.5f)));
			REQUIRE(physics.SetAngularVelocity(moving[index], glm::vec3(0.0f, 0.02f * static_cast<float>(index), 0.0f)));
		}
		REQUIRE(WaitForSteps(scene, 600, [&]() { return std::all_of(resting.begin(), resting.end(), [&](Entity box) { return physics.IsSleeping(box); }); }));
		const size_t restingPairs = physics.GetStats().ContactPairCount;
		CHECK(restingPairs == 20);
		recorder.Clear();

		// The cells arrive: 10,000 bodies more than the world has room for.
		for (uint32_t cell = 0; cell < 4; cell++)
		{
			for (uint32_t index = 0; index < 2500; index++)
			{
				const glm::vec3 position(1000.0f + static_cast<float>(cell) * 200.0f + static_cast<float>(index % 50) * 3.0f, 0.0f, static_cast<float>(index / 50) * 3.0f);
				CreateStaticBox(scene, "Cell Box", position, glm::vec3(1.0f));
			}
		}
		std::vector<glm::vec3> linear;
		std::vector<glm::vec3> angular;
		for (const Entity sphere : moving)
		{
			linear.push_back(physics.GetLinearVelocity(sphere));
			angular.push_back(physics.GetAngularVelocity(sphere));
		}
		std::vector<uint64_t> ids;
		const PhysicsWorld* world = physics.GetWorld();
		REQUIRE(world != nullptr);
		for (const Entity entity : resting)
			ids.push_back(world->GetBodyID(entity));
		for (const Entity entity : moving)
			ids.push_back(world->GetBodyID(entity));

		StepScene(scene, 1);
		const PhysicsStats stats = physics.GetStats();
		CHECK(stats.BodyOverflowCount == 0);
		CHECK(stats.GrowthCount == 1);
		size_t index = 0;
		for (const Entity entity : resting)
			CHECK(world->GetBodyID(entity) == ids[index++]);
		for (const Entity entity : moving)
			CHECK(world->GetBodyID(entity) == ids[index++]);
		CHECK(stats.MaxBodies == 16384);
		CHECK(stats.BodyCount == 1 + 20 + 100 + 10'000);
		for (size_t index = 0; index < moving.size(); index++)
		{
			const glm::vec3 linearAfter = physics.GetLinearVelocity(moving[index]);
			const glm::vec3 angularAfter = physics.GetAngularVelocity(moving[index]);
			CHECK(glm::all(glm::lessThanEqual(glm::abs(linearAfter - linear[index]), glm::vec3(1.0e-6f))));
			CHECK(glm::all(glm::lessThanEqual(glm::abs(angularAfter - angular[index]), glm::vec3(1.0e-6f))));
		}
		for (const Entity box : resting)
			CHECK(physics.IsSleeping(box));
		StepScene(scene, 30);
		CHECK(physics.GetStats().ContactPairCount == restingPairs);
		CHECK(recorder.Count(CollisionEventType::Begin) == 0);
		CHECK(recorder.Count(CollisionEventType::End) == 0);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A walking character controller keeps its position, velocity and ground across a growth")
	{
		ScopedCapacityDemands demands;
		Scene scene("Walker");
		scene.GetSettings().PhysicsCapacity.MaxBodies = 64;
		CreateGround(scene);
		Entity walker = scene.CreateEntity("Walker");
		CharacterControllerComponent& controller = walker.AddComponent<CharacterControllerComponent>();
		controller.Height = 1.8f;
		controller.Radius = 0.3f;

		scene.OnRuntimeStart();
		ICharacterControl* characters = scene.GetService<ICharacterControl>();
		REQUIRE(characters != nullptr);
		REQUIRE(characters->MoveCharacter(walker, glm::vec3(2.0f, 0.0f, 0.0f)));
		StepScene(scene, 30);
		const std::optional<CharacterState> before = characters->GetCharacterState(walker);
		REQUIRE(before.has_value());
		REQUIRE(before->Ground == CharacterGroundState::OnGround);
		const glm::vec3 positionBefore = GetWorldPosition(scene, walker);

		for (uint32_t index = 0; index < 100; index++)
			CreateStaticBox(scene, "Far Box", glm::vec3(500.0f + static_cast<float>(index) * 3.0f, 0.0f, 0.0f), glm::vec3(1.0f));
		PhysicsWorld* world = GetPhysics(scene).GetWorld();
		REQUIRE(world != nullptr);
		REQUIRE(world->Grow());
		CHECK(world->GetStats().MaxBodies == 128);
		const std::optional<CharacterState> after = characters->GetCharacterState(walker);
		REQUIRE(after.has_value());
		CHECK(after->Ground == before->Ground);
		CHECK(after->Velocity == before->Velocity);
		CHECK(after->GroundNormal == before->GroundNormal);
		CHECK(GetWorldPosition(scene, walker) == positionBefore);

		// It walks on, on the ground, and dynamic bodies still land on its presence body.
		Entity crate = CreateDynamicBox(scene, "Crate", positionBefore + glm::vec3(1.0f, 4.0f, 0.0f), glm::vec3(0.2f));
		StepScene(scene, 30);
		const std::optional<CharacterState> walking = characters->GetCharacterState(walker);
		REQUIRE(walking.has_value());
		CHECK(walking->Ground == CharacterGroundState::OnGround);
		CHECK(GetWorldPosition(scene, walker).x > positionBefore.x + 0.5f);
		CHECK(GetPhysics(scene).HasBody(crate));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Bodies waiting for a slot cost the steps nothing and get freed slots in the order they started waiting")
	{
		ScopedCapacityDemands demands;
		PhysicsSettings limited;
		limited.GrowthLimit = 1024; // A memory cap: the world may not grow
		ScopedPhysicsSettings scopedLimit(limited);
		Scene scene("Waiting");
		scene.GetSettings().PhysicsCapacity.MaxBodies = 1024;
		std::vector<Entity> boxes;
		boxes.reserve(3000);
		for (uint32_t index = 0; index < 3000; index++)
		{
			const glm::vec3 position(static_cast<float>(index % 64) * 2.0f, 0.0f, static_cast<float>(index / 64) * 2.0f);
			boxes.push_back(CreateStaticBox(scene, "Box " + std::to_string(index), position, glm::vec3(0.5f)));
		}

		// The first 1,024 boxes (in hierarchy order) get bodies; the others wait, and the steps do not look at them.
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		PhysicsStats stats = physics.GetStats();
		CHECK(stats.BodyCount == 1024);
		CHECK(stats.BodyOverflowCount == 1976);
		for (uint32_t step = 0; step < 3; step++)
		{
			StepScene(scene, 1);
			CHECK(physics.GetStats().SyncedBodyCount == 0);
		}
		CHECK(physics.HasBody(boxes[1023]));
		CHECK_FALSE(physics.HasBody(boxes[1024]));

		// 100 bodies go: exactly the first 100 waiting boxes get their slots, in the next update.
		scene.DestroyEntities(std::vector<Entity>(boxes.begin(), boxes.begin() + 100));
		StepScene(scene, 1);
		stats = physics.GetStats();
		CHECK(stats.BodyCount == 1024);
		CHECK(stats.BodyOverflowCount == 1876);
		CHECK(stats.SyncedBodyCount == 0);
		uint32_t given = 0;
		for (uint32_t index = 1024; index < 1124; index++)
			given += physics.HasBody(boxes[index]) ? 1u : 0u;
		CHECK(given == 100);
		CHECK_FALSE(physics.HasBody(boxes[1124]));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Bodies built on the job threads get the same IDs and simulate the same, whatever the number of threads")
	{
		// A mix of static, dynamic and kinematic bodies, capsules and compound bodies of collider children, built in one round
		// (the parallel path) with one worker and with the application's workers. Scaled down in Debug builds.
#if defined(ST_DEBUG)
		constexpr uint32_t c_Count = 4000;
		constexpr uint32_t c_Steps = 60;
#else
		constexpr uint32_t c_Count = 20'000;
		constexpr uint32_t c_Steps = 120;
#endif
		struct Run
		{
			std::vector<uint64_t> BodyIDs;
			std::vector<glm::mat4> Poses;
			uint64_t ParallelBuilds = 0;
		};
		const auto run = [&]() -> Run
		{
			Scene scene("Mixed");
			Entity ground = scene.CreateEntityWithUUID(UUID(0x10), "Ground");
			ground.GetTransform().Translation = glm::vec3(125.0f, -0.5f, 125.0f);
			ground.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(200.0f, 0.5f, 200.0f);
			std::vector<Entity> entities;
			for (uint32_t index = 0; index < c_Count; index++)
			{
				Entity entity = scene.CreateEntityWithUUID(UUID(0x1000 + static_cast<uint64_t>(index)), "Body");
				const glm::vec3 cell(static_cast<float>(index % 100) * 2.5f, 0.0f, static_cast<float>(index / 100) * 2.5f);
				switch (index % 10)
				{
					case 5:
						entity.AddComponent<RigidBodyComponent>();
						entity.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.4f);
						entity.GetTransform().Translation = cell + glm::vec3(0.0f, 3.0f + static_cast<float>(index % 3), 0.0f);
						entity.GetTransform().Rotation = glm::angleAxis(0.1f * static_cast<float>(index % 7), glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)));
						break;
					case 6:
						entity.AddComponent<RigidBodyComponent>();
						entity.AddComponent<SphereColliderComponent>().Radius = 0.4f;
						entity.GetTransform().Translation = cell + glm::vec3(0.1f, 4.0f, 0.0f);
						break;
					case 7:
						entity.AddComponent<RigidBodyComponent>();
						entity.AddComponent<CapsuleColliderComponent>();
						entity.GetTransform().Translation = cell + glm::vec3(0.0f, 5.0f, 0.1f);
						break;
					case 8:
						entity.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
						entity.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.3f, 0.1f, 0.3f);
						entity.GetTransform().Translation = cell + glm::vec3(0.0f, 7.0f, 0.0f);
						break;
					case 9:
					{
						entity.AddComponent<RigidBodyComponent>();
						entity.GetTransform().Translation = cell + glm::vec3(0.0f, 8.0f, 0.0f);
						for (const float offset : { -0.4f, 0.4f })
						{
							Entity child = scene.CreateChildEntity(entity, "Part");
							child.GetTransform().Translation = glm::vec3(offset, 0.0f, 0.0f);
							child.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.3f);
						}
						break;
					}
					default:
						entity.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.5f, 0.25f + 0.05f * static_cast<float>(index % 5), 0.5f);
						entity.GetTransform().Translation = cell + glm::vec3(0.0f, 0.25f, 0.0f);
						break;
				}
				entities.push_back(entity);
			}

			scene.OnRuntimeStart();
			PhysicsSystem& physics = GetPhysics(scene);
			Run result;
			const PhysicsWorld* world = physics.GetWorld();
			REQUIRE(world != nullptr);
			for (const Entity entity : entities)
				result.BodyIDs.push_back(world->GetBodyID(entity));
			result.ParallelBuilds = physics.GetStats().ParallelBuildCount;
			StepScene(scene, c_Steps);
			for (const Entity entity : entities)
				result.Poses.push_back(scene.GetWorldTransform(entity));
			scene.OnRuntimeStop();
			return result;
		};

		Run single;
		{
			ScopedJobSystem jobs(1);
			single = run();
		}
		Run many;
		{
			Perf::ScopedApplicationJobSystem jobs;
			many = run();
		}
		CHECK(single.ParallelBuilds >= 1);
		CHECK(many.ParallelBuilds == single.ParallelBuilds);
		REQUIRE(single.BodyIDs.size() == many.BodyIDs.size());
		uint32_t differentIDs = 0;
		uint32_t differentPoses = 0;
		uint32_t withoutBody = 0;
		for (size_t index = 0; index < single.BodyIDs.size(); index++)
		{
			differentIDs += single.BodyIDs[index] != many.BodyIDs[index] ? 1u : 0u;
			withoutBody += single.BodyIDs[index] == PhysicsWorld::c_NoBodyID ? 1u : 0u;
			differentPoses += std::memcmp(&single.Poses[index], &many.Poses[index], sizeof(glm::mat4)) != 0 ? 1u : 0u;
		}
		CHECK(withoutBody == 0);
		CHECK(differentIDs == 0);
		CHECK(differentPoses == 0);
	}

	TEST_CASE("IPhysicsCapacity reports bodies, waits, the last update's work and what the world reserves, and reserves room")
	{
		ScopedCapacityDemands demands;
		Scene scene("Capacity");
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		CHECK(scene.GetService<IPhysicsCapacity>() == nullptr); // Only while the scene runs
		CreateBoxRow(scene, 10);
		Entity flat = CreateDynamicBox(scene, "Flat", glm::vec3(0.0f, 20.0f, 0.0f));
		flat.GetTransform().Scale = glm::vec3(0.0f);

		scene.OnRuntimeStart();
		IPhysicsCapacity* capacity = scene.GetService<IPhysicsCapacity>();
		REQUIRE(capacity != nullptr);
		PhysicsWorld* world = GetPhysics(scene).GetWorld();
		REQUIRE(world != nullptr);
		PhysicsCapacityInfo info = capacity->GetCapacityInfo();
		CHECK(info.Bodies == 10);
		CHECK(info.MaxBodies == 1024);
		CHECK(info.GrowthLimit == PhysicsCapacitySettings::c_MaxBodiesLimit);
		CHECK(info.WaitingForSlot == 0);
		CHECK(info.PendingOther == 1); // The box scaled to zero
		// Constant time (the editor reads it every frame): the estimate of what box3d reserved and held, not its count of its
		// bytes.
		CHECK(info.ReservedBytes == world->GetReservedBytes());
		CHECK(info.ReservedBytes >= PhysicsWorld::EstimateReservedBytes(world->GetSettings()));
		const uint64_t reservedBefore = info.ReservedBytes;

		// The work of each update: bodies built, then destroyed.
		StepScene(scene, 1);
		std::vector<Entity> spawned = CreateBoxRow(scene, 50, 5.0f);
		StepScene(scene, 1);
		info = capacity->GetCapacityInfo();
		CHECK(info.AddedLastUpdate == 50);
		CHECK(info.RemovedLastUpdate == 0);
		CHECK(GetPhysics(scene).GetStats().BodiesAddedLastUpdate == 50);
		scene.DestroyEntities(std::vector<Entity>(spawned.begin(), spawned.begin() + 20));
		StepScene(scene, 1);
		info = capacity->GetCapacityInfo();
		CHECK(info.AddedLastUpdate == 0);
		CHECK(info.RemovedLastUpdate == 20);
		CHECK(info.Bodies == 40);
		StepScene(scene, 1);
		CHECK(capacity->GetCapacityInfo().RemovedLastUpdate == 0);

		// Room asked for grows the world's room before the next step, and what it takes grows with it.
		capacity->Reserve(100'000);
		CHECK(capacity->GetCapacityInfo().MaxBodies == 1024);
		StepScene(scene, 1);
		info = capacity->GetCapacityInfo();
		CHECK(info.MaxBodies == 131072);
		CHECK(info.GrowthCount == 1);
		CHECK(info.ReservedBytes == world->GetReservedBytes());
		CHECK(info.ReservedBytes >= reservedBefore);
		CHECK(GetPhysics(scene).GetStats().ChangeTime >= info.LastGrowthMs);
		scene.OnRuntimeStop();
		CHECK(scene.GetService<IPhysicsCapacity>() == nullptr);
	}

	TEST_CASE("The reserved bytes of a world are within 15% of box3d's count of them")
	{
		PhysicsSettings smallSettings = PhysicsWorld::ResolveCapacities(PhysicsSettings(), 10);
		PhysicsSettings large = PhysicsWorld::ResolveCapacities(PhysicsSettings(), 100'000);
		CHECK(PhysicsWorld::EstimateReservedBytes(large) > PhysicsWorld::EstimateReservedBytes(smallSettings));

		// Worlds whose room is used: boxes resting on a floor, each touching it.
		for (const uint32_t count : { 1024u, 16384u })
		{
			Scene scene("Bytes");
			Entity floor = scene.CreateEntity("Floor");
			floor.GetTransform().Translation = glm::vec3(0.0f, -0.5f, 0.0f);
			floor.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(400.0f, 0.5f, 400.0f);
			for (uint32_t index = 0; index + 1 < count; index++)
				CreateDynamicBox(scene, "Box", glm::vec3(static_cast<float>(index % 128) * 1.5f - 96.0f, 0.5f, static_cast<float>(index / 128) * 1.5f - 96.0f));
			scene.GetSettings().PhysicsCapacity.MaxBodies = count;
			scene.GetSettings().PhysicsCapacity.MaxContactConstraints = count;
			scene.OnRuntimeStart();
			StepScene(scene, 2);
			const PhysicsWorld* world = GetPhysics(scene).GetWorld();
			REQUIRE(world != nullptr);
			const double estimated = static_cast<double>(world->GetReservedBytes());
			const double counted = static_cast<double>(world->CountLibraryBytes());
			INFO("Bodies: ", count, ", estimated ", estimated / 1.0e6, " MB, box3d counts ", counted / 1.0e6, " MB");
			CHECK(estimated == doctest::Approx(counted).epsilon(0.15));
			scene.OnRuntimeStop();
		}
	}

	TEST_CASE("The body candidates of a scene count rigid bodies, colliders of their own and character controllers")
	{
		Scene scene("Candidates");
		CreateBoxRow(scene, 3);                                            // Rigid bodies with colliders: 3
		CreateStaticBox(scene, "Static", glm::vec3(0.0f), glm::vec3(1.0f)); // A collider of its own: 1
		Entity both = CreateStaticBox(scene, "Both", glm::vec3(5.0f), glm::vec3(1.0f));
		both.AddComponent<SphereColliderComponent>();                      // Two colliders, one body: 1
		scene.CreateEntity("Walker").AddComponent<CharacterControllerComponent>(); // A presence body: 1
		CHECK(PhysicsWorld::CountBodyCandidates(scene) == 6);
	}

	TEST_CASE("Bodies leaving the simulation together go in one batch, and what rested on them wakes")
	{
		// A cell: 2,500 static boxes under one root, 2 m apart over 100 x 100 m; a crate of the persistent scene rests on one.
		Scene scene("Leaving");
		Entity cell = scene.CreateEntity("Cell");
		for (uint32_t index = 0; index < 2500; index++)
		{
			Entity box = scene.CreateChildEntity(cell, "Floor");
			box.GetTransform().Translation = glm::vec3(static_cast<float>(index % 50) * 2.0f, -0.5f, static_cast<float>(index / 50) * 2.0f);
			box.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(1.0f, 0.5f, 1.0f);
		}
		Entity crate = CreateDynamicBox(scene, "Crate", glm::vec3(51.0f, 0.5f, 51.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(WaitForSteps(scene, 600, [&]() { return physics.IsSleeping(crate); }));
		const PhysicsStats before = physics.GetStats();
		REQUIRE(before.StaticBodyCount == 2500);

		// The cell leaves the simulation in one update: box3d disables its bodies, waking what touched them, without a query of
		// its own per body.
		cell.AddComponent<InactiveComponent>();
		StepScene(scene, 1);
		const PhysicsStats after = physics.GetStats();
		CHECK(after.StaticBodyCount == 0);
		CHECK(after.WakeQueryCount - before.WakeQueryCount <= 49);
		CHECK_FALSE(physics.IsSleeping(crate));
		const float height = GetWorldPosition(scene, crate).y;
		StepScene(scene, 30);
		CHECK(GetWorldPosition(scene, crate).y < height - 1.0f); // Nothing holds it up any more

		// Back again in one update, the bodies return.
		cell.RemoveComponent<InactiveComponent>();
		StepScene(scene, 1);
		CHECK(physics.GetStats().StaticBodyCount == 2500);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Contacts beyond the reserved count are counted in the demand and reserved by the scene's next world")
	{
		ScopedCapacityDemands demands;
		Scene scene("Pile");
		scene.GetSettings().PhysicsCapacity.MaxContactConstraints = 4;
		CreateGround(scene);
		CreateBoxRow(scene, 10, 0.45f);

		// box3d grows its contact storage: nothing is dropped and nothing is warned about, and the demand records it.
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		StepScene(scene, 10);
		const PhysicsStats stats = GetPhysics(scene).GetStats();
		CHECK(stats.Demand.ContactConstraints >= 10);
		CHECK(CountLogWarnings(logStart) == 0);
		scene.OnRuntimeStop();
		const uint32_t remembered = PhysicsSystem::GetRememberedCapacityDemand(scene.GetLineageID()).ContactConstraints;
		CHECK(remembered == stats.Demand.ContactConstraints);

		// The scene's next world with automatic room reserves at least that.
		scene.GetSettings().PhysicsCapacity = {};
		scene.OnRuntimeStart();
		CHECK(GetPhysics(scene).GetStats().MaxContactConstraints >= remembered);
		scene.OnRuntimeStop();
	}

	TEST_CASE("An automatic room that a world outgrew is reserved by the scene's next world, and only in that scene's")
	{
		ScopedCapacityDemands demands;
		const auto createScene = [](Scene& scene)
		{
			scene.GetSettings().Gravity = glm::vec3(0.0f);
			CreateBoxRow(scene, 10);
		};
		Ref<Scene> scene = CreateRef<Scene>("Spawner");
		createScene(*scene);

		// 10 bodies get the automatic room of 1,024; 1,500 spawned ones outgrow it.
		scene->OnRuntimeStart();
		CHECK(GetPhysics(*scene).GetStats().MaxBodies == 1024);
		for (uint32_t index = 0; index < 1500; index++)
			CreateDynamicSphere(*scene, "Spawned", glm::vec3(static_cast<float>(index % 50) * 2.0f, 10.0f, static_cast<float>(index / 50) * 2.0f), 0.25f);
		StepScene(*scene, 1);
		const PhysicsStats stats = GetPhysics(*scene).GetStats();
		CHECK(stats.GrowthCount == 1);
		CHECK(stats.MaxBodies == 2048);
		CHECK(stats.Demand.Bodies == 2048);
		scene->OnRuntimeStop();

		// Its next world reserves room for what it grew to, and so does a copy of it (what the editor plays).
		scene->OnRuntimeStart();
		CHECK(GetPhysics(*scene).GetStats().MaxBodies >= 2048);
		CHECK(GetPhysics(*scene).GetStats().GrowthCount == 0);
		scene->OnRuntimeStop();
		Ref<Scene> copy = Scene::Copy(scene);
		copy->OnRuntimeStart();
		CHECK(GetPhysics(*copy).GetStats().MaxBodies >= 2048);
		copy->OnRuntimeStop();

		// Another scene of the same name is sized for its own bodies.
		Scene namesake("Spawner");
		createScene(namesake);
		namesake.OnRuntimeStart();
		CHECK(GetPhysics(namesake).GetStats().MaxBodies == 1024);
		namesake.OnRuntimeStop();
	}

	TEST_CASE("A scene loaded from an asset is remembered by the asset, across the scenes made from it")
	{
		ScopedCapacityDemands demands;
		Scene source("Crowded Level");
		source.GetSettings().Gravity = glm::vec3(0.0f);
		source.GetSettings().PhysicsCapacity.MaxBodies = 4;
		CreateBoxRow(source, 10);
		const std::string document = SceneSerializer::Serialize(source).dump();
		std::string error;
		const Ref<SceneAsset> asset = SceneAsset::Deserialize(std::span(reinterpret_cast<const uint8_t*>(document.data()), document.size()), &error);
		REQUIRE_MESSAGE(asset, error);
		asset->Handle = UUID();

		// Every scene made from the asset is that asset's scene, also after the asset was loaded again.
		const Ref<Scene> first = asset->CreateScene(&error);
		REQUIRE_MESSAGE(first, error);
		CHECK(first->GetSourceAsset() == asset->Handle);
		CHECK(first->GetLineageID() == asset->Handle);
		first->OnRuntimeStart();
		CHECK(GetPhysics(*first).GetStats().GrowthCount == 1);
		first->OnRuntimeStop();
		CHECK(PhysicsSystem::GetRememberedCapacityDemand(asset->Handle).Bodies == 16);

		const Ref<Scene> second = asset->CreateScene(&error);
		REQUIRE_MESSAGE(second, error);
		CHECK(second->GetLineageID() == first->GetLineageID());
		CHECK(Scene::Copy(second)->GetSourceAsset() == asset->Handle);
		// The source scene, made in code, is not that asset's.
		CHECK(source.GetLineageID() != asset->Handle);
		CHECK(PhysicsSystem::GetRememberedCapacityDemand(source.GetLineageID()).IsEmpty());
	}
}
