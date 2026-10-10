#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Physics/PhysicsWorld.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"
#include "TestHelpers.h"

#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_MiB = 1024u * 1024u;

	// Forgets what earlier worlds needed (process-wide, by scene name) before and after a test, so tests do not size each
	// other's worlds.
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

}

TEST_SUITE("Physics.Capacity")
{
	TEST_CASE("Automatic capacities follow the bodies a scene starts with")
	{
		const PhysicsSettings automatic;
		CHECK(automatic.MaxBodies == 0);
		CHECK(automatic.MaxBodyPairs == 0);
		CHECK(automatic.MaxContactConstraints == 0);
		CHECK(automatic.TempAllocatorSize == 0);

		// Small scenes get the minimums.
		const PhysicsSettings small = PhysicsWorld::ResolveCapacities(automatic, 10);
		CHECK(small.MaxBodies == 65536);
		CHECK(small.MaxBodyPairs == 4 * 65536);
		CHECK(small.MaxContactConstraints == 10240);
		CHECK(small.TempAllocatorSize >= 10 * c_MiB);

		// Large ones: the next power of two of twice the bodies, four pairs per body slot, two contacts per body, and the
		// scratch memory for them.
		const PhysicsSettings large = PhysicsWorld::ResolveCapacities(automatic, 70'000);
		CHECK(large.MaxBodies == 262'144);
		CHECK(large.MaxBodyPairs == 4 * 262'144);
		CHECK(large.MaxContactConstraints == 140'000);
		CHECK(large.TempAllocatorSize > small.TempAllocatorSize);

		// Given capacities stay.
		PhysicsSettings given;
		given.MaxBodies = 100;
		given.MaxBodyPairs = 200;
		given.MaxContactConstraints = 300;
		given.TempAllocatorSize = c_MiB;
		const PhysicsSettings kept = PhysicsWorld::ResolveCapacities(given, 70'000, { 500'000, 500'000, 500'000 });
		CHECK(kept.MaxBodies == 100);
		CHECK(kept.MaxBodyPairs == 200);
		CHECK(kept.MaxContactConstraints == 300);
		CHECK(kept.TempAllocatorSize == c_MiB);

		// What an earlier world needed raises the automatic ones.
		const PhysicsSettings grown = PhysicsWorld::ResolveCapacities(automatic, 10, { 100'000, 5'000'000, 400'000 });
		CHECK(grown.MaxBodies == 262'144);
		CHECK(grown.MaxBodyPairs == 5'000'000);
		CHECK(grown.MaxContactConstraints == 400'000);

		// Nothing exceeds the limits a scene may ask for.
		const PhysicsSettings huge = PhysicsWorld::ResolveCapacities(automatic, 20'000'000);
		CHECK(huge.MaxBodies == PhysicsCapacitySettings::c_MaxBodiesLimit);
		CHECK(huge.MaxBodyPairs == 4 * PhysicsCapacitySettings::c_MaxBodiesLimit);
		CHECK(huge.MaxContactConstraints == PhysicsCapacitySettings::c_MaxContactConstraintsLimit);
		CHECK(PhysicsWorld::ResolveCapacities(automatic, 10, { 0, UINT32_MAX, 0 }).MaxBodyPairs == PhysicsCapacitySettings::c_MaxBodyPairsLimit);
	}

	TEST_CASE("A scene's capacities size its world, before the default settings and automatic sizing")
	{
		ScopedCapacityDemands demands;
		Scene scene("Capacities");
		CreateGround(scene);
		CreateBoxRow(scene, 3);
		scene.GetSettings().PhysicsCapacity = { 16, 0, 64 };

		scene.OnRuntimeStart();
		PhysicsStats stats = GetPhysics(scene).GetStats();
		CHECK(stats.MaxBodies == 16);
		CHECK(stats.MaxBodyPairs == 4 * 16);
		CHECK(stats.MaxContactConstraints == 64);
		CHECK(stats.BodyCount == 4);
		scene.OnRuntimeStop();

		// Without scene capacities, the default settings give what they set and the rest is automatic.
		scene.GetSettings().PhysicsCapacity = {};
		PhysicsSettings defaults;
		defaults.MaxBodyPairs = 1000;
		ScopedPhysicsSettings scopedDefaults(defaults);
		scene.OnRuntimeStart();
		stats = GetPhysics(scene).GetStats();
		CHECK(stats.MaxBodies == 65536);
		CHECK(stats.MaxBodyPairs == 1000);
		CHECK(stats.MaxContactConstraints == 10240);
		CHECK(stats.Demand.IsEmpty());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Bodies that find the world full are reported in one error, counted, and remembered for the scene's next world")
	{
		ScopedCapacityDemands demands;
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
		CHECK(CountLogMessages(logStart, "'Box 4' and 5 more entities: the world's limit of 4 bodies is reached") == 1);
		CHECK(CountLogMessages(logStart, "raise the scene's PhysicsCapacity.MaxBodies") == 1);

		// Waiting entities get the slots of removed bodies; while some still wait, nothing is logged again.
		StepScene(scene, 5);
		scene.DestroyEntities(std::vector<Entity>(boxes.begin(), boxes.begin() + 3));
		StepScene(scene, 2);
		stats = physics.GetStats();
		CHECK(stats.BodyCount == 4);
		CHECK(stats.BodyOverflowCount == 3);
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
		CHECK(PhysicsSystem::GetRememberedCapacityDemand("Crowded").IsEmpty());
		scene.OnRuntimeStop();
		CHECK(PhysicsSystem::GetRememberedCapacityDemand("Crowded").Bodies == 10);
		CHECK(PhysicsSystem::GetRememberedCapacityDemand("Other").IsEmpty());
	}

	TEST_CASE("Steps that overflow the contact buffers are counted, warned about once and remembered")
	{
		ScopedCapacityDemands demands;
		Scene scene("Pile");
		scene.GetSettings().PhysicsCapacity.MaxContactConstraints = 4;
		CreateGround(scene);
		CreateBoxRow(scene, 10, 0.45f);

		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		StepScene(scene, 10);
		const PhysicsStats stats = GetPhysics(scene).GetStats();
		CHECK(stats.ContactOverflowSteps > 0);
		CHECK(stats.Demand.ContactConstraints == 8);
		// One warning per kind of buffer that overflowed (the contact constraints and their manifold cache share the capacity).
		const size_t warnings = CountLogMessages(logStart, "contacts were dropped");
		CHECK(warnings >= 1);
		CHECK(warnings <= 2);
		CHECK(CountLogMessages(logStart, "raise the scene's PhysicsCapacity.MaxContactConstraints") == warnings);
		StepScene(scene, 10);
		CHECK(GetPhysics(scene).GetStats().ContactOverflowSteps >= stats.ContactOverflowSteps);
		CHECK(CountLogMessages(logStart, "contacts were dropped") == warnings);
		scene.OnRuntimeStop();
		CHECK(PhysicsSystem::GetRememberedCapacityDemand("Pile").ContactConstraints == 8);

		// With automatic capacities the next world has room for every contact.
		scene.GetSettings().PhysicsCapacity = {};
		const uint64_t secondStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		StepScene(scene, 10);
		CHECK(GetPhysics(scene).GetStats().ContactOverflowSteps == 0);
		CHECK(CountLogMessages(secondStart, "contacts were dropped") == 0);
		scene.OnRuntimeStop();
	}
}
