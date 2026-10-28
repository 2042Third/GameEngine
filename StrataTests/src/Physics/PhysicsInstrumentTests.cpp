#include <doctest/doctest.h>

#include "Physics/PhysicsScenes.h"
#include "Physics/PhysicsTestUtils.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Process.h"
#include "Strata/Physics/PhysicsComponents.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/SceneServices.h"
#include "TestHelpers.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A stack of `count` 1 m boxes placed touching on the ground, the lowest resting on it.
	std::vector<Entity> CreateStack(Scene& scene, uint32_t count, const glm::vec3& base = glm::vec3(0.0f))
	{
		std::vector<Entity> boxes;
		for (uint32_t index = 0; index < count; index++)
			boxes.push_back(CreateDynamicBox(scene, "Stacked " + std::to_string(index), base + glm::vec3(0.0f, 0.5f + static_cast<float>(index), 0.0f)));
		return boxes;
	}

	// The constant-time counts agree with the walk of the records: the moving bodies are the dynamic and kinematic bodies in
	// the simulation and the character controllers' presence bodies, the awake ones among them (GetStats also asserts the
	// first in Debug builds).
	void CheckMovingCount(PhysicsSystem& physics)
	{
		const PhysicsStats stats = physics.GetStats();
		CHECK(stats.MovingBodyCount == stats.DynamicBodyCount + stats.KinematicBodyCount + stats.CharacterControllerCount);
		CHECK(stats.ActiveBodyCount <= stats.MovingBodyCount);
		CHECK(stats.SleepingBodyCount == stats.MovingBodyCount - stats.ActiveBodyCount);
		CHECK(physics.GetStepInfo().SleepingBodies == stats.SleepingBodyCount);
	}

}

TEST_SUITE("Physics.Instruments")
{
	TEST_CASE("A settling stack recycles contacts, then sleeps with nothing recycled")
	{
		Scene scene;
		CreateGround(scene);
		const std::vector<Entity> boxes = CreateStack(scene, 10);
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		physics.SetDetailedStats(true);

		uint64_t recycled = 0;
		uint32_t previousAwake = static_cast<uint32_t>(boxes.size());
		uint32_t steps = 0;
		for (; steps < 900; steps++)
		{
			StepScene(scene, 1);
			const PhysicsStats stats = physics.GetStats();
			REQUIRE(stats.DetailedStats);
			CAPTURE(steps);
			// Recycled contacts are contacts box3d kept: never more than it has. The awake contacts are counted after the step,
			// so only a step in which no island fell asleep bounds the recycled ones by them.
			CHECK(stats.RecycledContactCount <= stats.ContactCount);
			if (stats.ActiveBodyCount >= previousAwake)
				CHECK(stats.RecycledContactCount <= stats.AwakeContactCount);
			previousAwake = stats.ActiveBodyCount;
			recycled += stats.RecycledContactCount;
			if (stats.ActiveBodyCount == 0)
				break;
		}
		REQUIRE(steps < 900);
		MESSAGE("The stack slept after ", steps + 1, " steps, having recycled ", recycled, " contacts");
		// Resting contacts move less than box3d's recycle distance (5 cm) and turn less than its 10 degrees.
		CHECK(recycled > 0);

		// Asleep: box3d keeps the last collide pass's recycled count while no contact is awake; it reads 0.
		StepScene(scene, 2);
		const PhysicsStats stats = physics.GetStats();
		CHECK(stats.ActiveBodyCount == 0);
		CHECK(stats.SleepingBodyCount == boxes.size());
		CHECK(stats.MovingBodyCount == boxes.size());
		CHECK(stats.AwakeContactCount == 0);
		CHECK(stats.RecycledContactCount == 0);
		CHECK(stats.ContactCount >= boxes.size()); // The resting contacts sleep, kept
		CHECK(stats.IslandCount >= 1);
		CHECK(stats.SettlingBodyCount == 0);
		CHECK(stats.MaxBodySpeed == 0.0f);

		// A query reads box3d's counters once, with the same rule, also while they are not read every step.
		physics.SetDetailedStats(false);
		StepScene(scene, 1);
		CHECK(physics.GetStats().ContactCount == 0);
		CHECK_FALSE(physics.GetStats().DetailedStats);
		const PhysicsLibraryCounters queried = physics.GetWorld()->ReadLibraryCounters();
		CHECK(queried.Contacts == stats.ContactCount);
		CHECK(queried.RecycledContacts == 0);
		CHECK_FALSE(physics.GetStats().DetailedStats);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A falling body's speed is its velocity, and resting bodies jitter little")
	{
		Scene scene;
		CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Falling", glm::vec3(0.0f, 5.0f, 0.0f));
		// Without damping (the component's default slows a falling body by about 5% after a second).
		box.GetComponent<RigidBodyComponent>().LinearDamping = 0.0f;
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);

		// From half a second of fall until a meter above the ground: the displacement over a step lags box3d's velocity at its
		// end by less than half a step of gravity under its sub-stepped integration.
		StepScene(scene, 30);
		uint32_t compared = 0;
		while (GetWorldPosition(scene, box).y - 0.5f > 1.0f)
		{
			const float speed = glm::length(physics.GetLinearVelocity(box));
			const PhysicsStats stats = physics.GetStats();
			CHECK(stats.WrittenBodyCount == 1);
			CHECK(stats.MaxBodySpeed == doctest::Approx(speed).epsilon(0.02));
			CHECK(stats.SettlingBodyCount == 1);
			StepScene(scene, 1);
			compared++;
		}
		CHECK(compared > 5);

		// Landed and resting: what still moves moves by much less than a millimeter a step, then nothing is written.
		StepScene(scene, 600);
		const PhysicsStats rested = physics.GetStats();
		CHECK(rested.ActiveBodyCount == 0);
		CHECK(rested.SettlingBodyCount == 0);
		CHECK(rested.RestingJitter < 0.1f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("box3d's stage times nest as box3d measures them")
	{
		Scene scene;
		CreateGround(scene);
		for (uint32_t column = 0; column < 40; column++)
			CreateStack(scene, 5, glm::vec3(static_cast<float>(column % 8) * 1.5f - 6.0f, 0.2f, static_cast<float>(column / 8) * 1.5f - 3.0f));
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		for (uint32_t step = 0; step < 30; step++)
		{
			StepScene(scene, 1);
			const PhysicsStats stats = physics.GetStats();
			const PhysicsLibraryProfile& profile = stats.LibraryProfile;
			CAPTURE(step);
			// The stages are parts of the library step, timed inside the wall time around it.
			CHECK(profile.Pairs + profile.Collide + profile.Solve + profile.Sensors <= stats.LibraryStepTime + 0.05f);
			// Solve's parts are timed inside it; splitting islands runs beside it and is part of neither sum.
			CHECK(profile.Constraints + profile.Transforms + profile.Refit + profile.Continuous + profile.SleepIslands <= profile.Solve + 0.05f);
			CHECK(profile.Solve > 0.0f);
			// Strata's parts are parts of its step.
			CHECK(stats.ApplyTime + stats.CharacterTime + stats.CheckedPairTime <= stats.SyncTime + 0.01f);
			CHECK(stats.EventTime <= stats.ContactTime + 0.01f);
			CHECK(stats.SyncTime + stats.LibraryStepTime + stats.WriteBackTime + stats.ContactTime + stats.InstrumentTime <= stats.LastStepTime + 0.05f);
		}
		const PhysicsStats stats = physics.GetStats();
		CHECK(stats.LibraryWorkers >= 1);
		CHECK(stats.TotalStepMs >= stats.TotalLibraryMs);
		CHECK(stats.TotalLibraryMs > 0.0);
		CHECK(stats.LibraryContactPeak > 0);
		CHECK(stats.ContactRoomGrowths == 0); // 200 boxes' contacts fit the automatic room
		scene.OnRuntimeStop();
	}

	TEST_CASE("The moving bodies are counted as they come and go")
	{
		Scene scene;
		CreateGround(scene);
		std::vector<Entity> boxes = CreateStack(scene, 4);
		Entity platform = scene.CreateEntity("Platform");
		platform.GetTransform().Translation = glm::vec3(5.0f, 1.0f, 0.0f);
		platform.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		platform.AddComponent<BoxColliderComponent>();
		Entity parent = scene.CreateEntity("Parent");
		Entity child = CreateDynamicBox(scene, "Child", glm::vec3(-5.0f, 0.5f, 0.0f));
		scene.SetParent(child, parent);
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 1);
		CHECK(physics.GetStats().MovingBodyCount == 6);
		CheckMovingCount(physics);

		SUBCASE("Deactivation and activation")
		{
			parent.SetActive(false);
			StepScene(scene, 1);
			CHECK(physics.GetStats().MovingBodyCount == 5);
			CheckMovingCount(physics);
			parent.SetActive(true);
			StepScene(scene, 1);
			CHECK(physics.GetStats().MovingBodyCount == 6);
			CheckMovingCount(physics);
		}
		SUBCASE("Rebuilds, type changes and removals")
		{
			boxes[3].GetComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.4f);
			boxes[3].MarkModified<BoxColliderComponent>();
			StepScene(scene, 1);
			CHECK(physics.GetStats().MovingBodyCount == 6);
			CheckMovingCount(physics);
			platform.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
			platform.MarkModified<RigidBodyComponent>();
			StepScene(scene, 1);
			CHECK(physics.GetStats().MovingBodyCount == 5);
			CheckMovingCount(physics);
			boxes[2].RemoveComponent<RigidBodyComponent>(); // A static collider of its own now
			StepScene(scene, 1);
			CHECK(physics.GetStats().MovingBodyCount == 4);
			CheckMovingCount(physics);
			scene.DestroyEntities(std::vector<Entity> { boxes[0], boxes[1] });
			StepScene(scene, 1);
			CHECK(physics.GetStats().MovingBodyCount == 2);
			CheckMovingCount(physics);
		}
		SUBCASE("A degenerate transform suspends a body")
		{
			child.GetTransform().Scale = glm::vec3(0.0f);
			child.MarkModified<TransformComponent>();
			StepScene(scene, 1);
			CHECK(physics.GetStats().MovingBodyCount == 5);
			CheckMovingCount(physics);
			child.GetTransform().Scale = glm::vec3(1.0f);
			child.MarkModified<TransformComponent>();
			StepScene(scene, 1);
			CHECK(physics.GetStats().MovingBodyCount == 6);
			CheckMovingCount(physics);
		}
		SUBCASE("Character controllers' presence bodies")
		{
			Entity character = scene.CreateEntity("Character");
			character.GetTransform().Translation = glm::vec3(0.0f, 0.0f, 5.0f);
			character.AddComponent<CharacterControllerComponent>();
			StepScene(scene, 1);
			CHECK(physics.GetStats().MovingBodyCount == 7);
			CheckMovingCount(physics);
			character.GetComponent<CharacterControllerComponent>().Radius = 0.5f; // A new presence body
			character.MarkModified<CharacterControllerComponent>();
			StepScene(scene, 1);
			CHECK(physics.GetStats().MovingBodyCount == 7);
			CheckMovingCount(physics);
			scene.DestroyEntities(std::vector<Entity> { character });
			StepScene(scene, 1);
			CHECK(physics.GetStats().MovingBodyCount == 6);
			CheckMovingCount(physics);
		}
		SUBCASE("Many at once, built in parallel and removed together")
		{
			// At least c_ParallelBuildThreshold owners in a round build on the job threads.
			ScopedJobSystem jobs(4);
			std::vector<Entity> spawned;
			for (uint32_t index = 0; index < 300; index++)
				spawned.push_back(CreateDynamicBox(scene, "Spawned", glm::vec3(static_cast<float>(index % 20) * 1.2f - 12.0f, 3.0f, static_cast<float>(index / 20) * 1.2f + 3.0f)));
			StepScene(scene, 1);
			CHECK(physics.GetStats().MovingBodyCount == 306);
			CheckMovingCount(physics);
			scene.DestroyEntities(spawned);
			StepScene(scene, 1);
			CHECK(physics.GetStats().MovingBodyCount == 6);
			CheckMovingCount(physics);
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("The step's diagnostics are those of the stats, and reading them changes nothing")
	{
		// A running scene without physics components has the system but no world yet.
		Scene scene;
		scene.OnRuntimeStart();
		IPhysicsDiagnostics* diagnostics = scene.GetService<IPhysicsDiagnostics>();
		REQUIRE(diagnostics != nullptr);
		CHECK_FALSE(diagnostics->GetStepInfo().HasWorld);
		// Asked for before the world exists: the world gets it once the first bodies make it.
		diagnostics->SetDetailedStats(true);
		CreateGround(scene);
		CreateStack(scene, 6);
		CreateDynamicSphere(scene, "Ball", glm::vec3(3.0f, 2.0f, 0.0f));
		PhysicsSystem& physics = GetPhysics(scene);
		for (uint32_t step = 0; step < 20; step++)
		{
			StepScene(scene, 1);
			const PhysicsStepInfo before = diagnostics->GetStepInfo();
			const PhysicsStats stats = physics.GetStats();
			const PhysicsStepInfo after = diagnostics->GetStepInfo();
			REQUIRE(before.HasWorld);
			CHECK(before.DetailedStats);
			CHECK(before.StepMs == stats.LastStepTime);
			CHECK(before.LibraryMs == stats.LibraryStepTime);
			CHECK(before.AwakeBodies == stats.ActiveBodyCount);
			CHECK(before.SleepingBodies == stats.SleepingBodyCount);
			CHECK(before.Islands == stats.IslandCount);
			CHECK(before.Contacts == stats.ContactCount);
			CHECK(before.AwakeContacts == stats.AwakeContactCount);
			CHECK(before.RecycledContacts == stats.RecycledContactCount);
			CHECK(before.MaxBodySpeed == stats.MaxBodySpeed);
			CHECK(before.SettlingBodies == stats.SettlingBodyCount);
			CHECK(before.RestingJitterMm == stats.RestingJitter);
			CHECK(before.Steps == stats.StepCount);
			CHECK(before.TotalStepMs == stats.TotalStepMs);
			CHECK(before.TotalLibraryMs == stats.TotalLibraryMs);
			CHECK(before.Bodies == physics.GetCapacityInfo().Bodies);
			CHECK(after.StepMs == before.StepMs);
			CHECK(after.Contacts == before.Contacts);
			CHECK(after.RecycledContacts == before.RecycledContacts);
			CHECK(after.MaxBodySpeed == before.MaxBodySpeed);
		}
		scene.OnRuntimeStop();
	}

	TEST_CASE("The settling measures do not depend on the number of threads")
	{
		// More than c_ParallelWriteThreshold bodies write back in parallel chunks, whose sums combine exactly.
		const auto run = [](uint32_t workers)
		{
			std::vector<PhysicsStepInfo> steps;
			Scope<ScopedJobSystem> jobs = workers > 0 ? CreateScope<ScopedJobSystem>(workers) : nullptr;
			Scene scene;
			CreateGround(scene);
			for (uint32_t column = 0; column < 120; column++)
				CreateStack(scene, 6, glm::vec3(static_cast<float>(column % 12) * 1.5f - 9.0f, 0.1f, static_cast<float>(column / 12) * 1.5f - 7.5f));
			scene.OnRuntimeStart();
			for (uint32_t step = 0; step < 40; step++)
			{
				StepScene(scene, 1);
				steps.push_back(GetPhysics(scene).GetStepInfo());
			}
			scene.OnRuntimeStop();
			return steps;
		};
		const std::vector<PhysicsStepInfo> serial = run(0);
		const std::vector<PhysicsStepInfo> parallel = run(4);
		REQUIRE(serial.size() == parallel.size());
		for (size_t step = 0; step < serial.size(); step++)
		{
			CAPTURE(step);
			CHECK(serial[step].MaxBodySpeed == parallel[step].MaxBodySpeed);
			CHECK(serial[step].SettlingBodies == parallel[step].SettlingBodies);
			CHECK(serial[step].RestingJitterMm == parallel[step].RestingJitterMm);
			CHECK(serial[step].AwakeBodies == parallel[step].AwakeBodies);
		}
	}
}

TEST_SUITE("TestHelpers")
{
	TEST_CASE("The physics-settle helper plays a settle scene and writes everything it measured")
	{
		const std::filesystem::path result = Tests::CreateTemporaryDirectory("PhysicsSettle") / "Tower.json";
		ProcessSpecification specification;
		specification.Executable = Tests::GetTestExecutablePath();
		specification.Arguments = { "--strata-test-helper=physics-settle", "Tower20", "1", FileSystem::ToUTF8(result), "--workers", "2", "--collision-steps", "1" };
		const Process::RunResult run = Process::Run(specification, std::chrono::minutes(5));
		INFO("Output: ", run.Output);
		REQUIRE(run.Started);
		CHECK_FALSE(run.TimedOut);
		REQUIRE(run.ExitCode == 0);
		const std::optional<std::string> text = FileSystem::ReadText(result);
		REQUIRE(text);
		const nlohmann::json summary = nlohmann::json::parse(*text, nullptr, false);
		REQUIRE(summary.is_object());
		CHECK(summary["scene"] == "Tower20");
		CHECK(summary["steps"] == 60);
		CHECK(summary["movingBodies"] == 20);
		CHECK(summary["workers"].get<uint32_t>() <= 2);
		for (const char* key : { "seconds", "secondsTo99Asleep", "secondsToAllAsleep", "stepMs", "libraryMs", "settleCpuMs", "busyStrataMs", "busyLibraryMs",
			"busyCollideMs", "busyRecycledShare", "meanRecycledShare", "maxSpeedAt5s", "restingJitterMm" })
		{
			CAPTURE(key);
			CHECK(summary[key].is_number());
		}
		CHECK(summary["stepMs"].get<double>() > 0.0);
		CHECK(summary["probes"]["tower.topDriftMm"].is_number());

		// What it refuses.
		specification.Arguments = { "--strata-test-helper=physics-settle", "Nothing", "1", FileSystem::ToUTF8(result) };
		CHECK(Process::Run(specification, std::chrono::minutes(1)).ExitCode == 1);
		specification.Arguments = { "--strata-test-helper=physics-settle", "Tower20", "1", FileSystem::ToUTF8(result), "--workers", "lots" };
		CHECK(Process::Run(specification, std::chrono::minutes(1)).ExitCode == 1);
	}
}
