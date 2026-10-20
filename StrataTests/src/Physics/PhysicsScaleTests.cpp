#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Physics/PhysicsComponents.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"
#include "Strata/Scene/SceneServices.h"
#include "TestHelpers.h"

#include <cmath>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A static floor whose top is the plane y = 0, large enough for `count` boxes 1.5 apart.
	Entity CreateFloor(Scene& scene, uint32_t count)
	{
		const float extent = 1.5f * static_cast<float>(static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))))) + 5.0f;
		return CreateStaticBox(scene, "Floor", glm::vec3(extent * 0.5f - 5.0f, -0.5f, extent * 0.5f - 5.0f), glm::vec3(extent, 0.5f, extent));
	}

	// `count` boxes in a square grid 1.5 apart (they do not touch each other), each sinking slightly into the floor, so every
	// box touches the floor from the first step on.
	std::vector<Entity> CreateRestingBoxes(Scene& scene, uint32_t count)
	{
		const uint32_t side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(count))));
		std::vector<Entity> boxes;
		boxes.reserve(count);
		for (uint32_t index = 0; index < count; index++)
		{
			const glm::vec3 position(1.5f * static_cast<float>(index % side), 0.49f, 1.5f * static_cast<float>(index / side));
			boxes.push_back(CreateDynamicBox(scene, "Box", position));
		}
		return boxes;
	}

	// Warnings of the physics module and of box3d logged since `logStart`.
	size_t CountPhysicsWarnings(uint64_t logStart)
	{
		return CountLogMessages(logStart, "Physics: ") + CountLogMessages(logStart, "box3d: ");
	}

}

TEST_SUITE("Physics.Scale")
{
	TEST_CASE("5,000 boxes on one floor reach the floor's script, and removing them costs constant work per contact")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity floor = CreateFloor(scene, 5000);
		AddScriptEntry(floor, "ContactRecorder");
		std::vector<Entity> boxes = CreateRestingBoxes(scene, 5000);

		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 2);

		// Every box touches the floor: one Enter each, and nothing was warned about.
		const ScriptSystem& scripts = GetScriptSystem(scene);
		CHECK(GetField<int32_t>(scripts, floor, "ContactRecorder", "CollisionEnters") == 5000);
		CHECK(GetField<int32_t>(scripts, floor, "ContactRecorder", "CollisionExits") == 0);
		CHECK(CountPhysicsWarnings(logStart) == 0);
		PhysicsStats stats = physics.GetStats();
		CHECK(stats.ContactPairCount == 5000);

		// Removing boxes ends their contacts with the floor, whose list holds thousands of pairs: each removal writes a few
		// entries of the pair lists, as many for the second half as for the first.
		const auto removeBoxes = [&](size_t first, size_t count)
		{
			const uint64_t before = physics.GetStats().PairIndexOperations;
			scene.DestroyEntities(std::vector<Entity>(boxes.begin() + static_cast<ptrdiff_t>(first), boxes.begin() + static_cast<ptrdiff_t>(first + count)));
			StepScene(scene, 1);
			return physics.GetStats().PairIndexOperations - before;
		};
		const uint64_t firstOperations = removeBoxes(0, 1250);
		const uint64_t secondOperations = removeBoxes(1250, 1250);
		INFO("Pair index operations: ", firstOperations, " for the first 1,250 removals, ", secondOperations, " for the next");
		CHECK(firstOperations <= 3 * 1250);
		CHECK(secondOperations <= 3 * 1250);
		CHECK(firstOperations >= 1250);
		CHECK(secondOperations >= 1250);
		CHECK(GetField<int32_t>(scripts, floor, "ContactRecorder", "CollisionExits") == 2500);
		CHECK(physics.GetStats().ContactPairCount == 2500);
		CHECK(CountPhysicsWarnings(logStart) == 0);
		CHECK_FALSE(engine->IsFaulted());
		scene.OnRuntimeStop();
	}

	TEST_CASE("Contacts are tracked only while someone wants them")
	{
		Scene scene;
		CreateFloor(scene, 16);
		std::vector<Entity> boxes = CreateRestingBoxes(scene, 16);

		// Playing without listeners of every entity and without watched entities (the scripting system listens, but no script
		// has contact callbacks): contacts cost nothing beyond Jolt's step.
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		ICollisionEventSource* contacts = scene.GetService<ICollisionEventSource>();
		REQUIRE(contacts != nullptr);
		StepScene(scene, 2);
		REQUIRE(physics.GetWorld() != nullptr);
		CHECK_FALSE(physics.GetWorld()->IsTrackingAllContacts());
		CHECK(physics.GetStats().ContactPairCount == 0);

		// Watching one box tracks its contact only, from the next step that reports it (the boxes are awake).
		std::vector<CollisionEvent> watchedEvents;
		const CollisionListenerID watcher = contacts->AddCollisionListener([&](const CollisionEvent& event) { watchedEvents.push_back(event); },
			CollisionListenerScope::WatchedEntities);
		contacts->WatchCollisions(boxes[3].GetUUID());
		StepScene(scene, 1);
		CHECK(physics.GetStats().ContactPairCount == 1);
		REQUIRE(watchedEvents.size() == 1);
		CHECK(watchedEvents[0].Type == CollisionEventType::Begin);
		CHECK(watchedEvents[0].Involves(boxes[3].GetUUID()));

		// A listener of every entity tracks every contact; the watched one hears only its own.
		CollisionRecorder recorder(physics);
		CHECK(physics.GetWorld()->IsTrackingAllContacts());
		StepScene(scene, 1);
		CHECK(physics.GetStats().ContactPairCount == 16);
		CHECK(recorder.Count(CollisionEventType::Begin) == 15); // The watched contact began before
		CHECK(watchedEvents.size() == 1);

		// Once nobody wants them, the contacts of awake bodies end at the next step: every Begin got its End.
		CHECK(physics.RemoveCollisionListener(recorder.GetListenerID()));
		CHECK_FALSE(physics.GetWorld()->IsTrackingAllContacts());
		contacts->UnwatchCollisions(boxes[3].GetUUID());
		for (Entity box : boxes)
			CHECK(physics.WakeUp(box));
		StepScene(scene, 1);
		CHECK(physics.GetStats().ContactPairCount == 0);
		REQUIRE(watchedEvents.size() == 1); // Not watched any more: the End of its contact concerns nobody watching
		CHECK(contacts->RemoveCollisionListener(watcher));
		scene.OnRuntimeStop();
	}

	TEST_CASE("Bodies created in one frame join the simulation in one batch per activation mode")
	{
		Scene scene;
		CreateFloor(scene, 100);
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 1);
		const uint64_t batches = physics.GetStats().AddBatchCount;

		// A spawn of 100 dynamic boxes and 10 static props: two batches (the static ones are not activated).
		std::vector<Entity> boxes;
		for (uint32_t index = 0; index < 100; index++)
			boxes.push_back(CreateDynamicBox(scene, "Spawned", glm::vec3(1.5f * static_cast<float>(index % 10), 3.0f, 1.5f * static_cast<float>(index / 10))));
		for (uint32_t index = 0; index < 10; index++)
			CreateStaticBox(scene, "Prop", glm::vec3(1.5f * static_cast<float>(index), 0.5f, -5.0f), glm::vec3(0.5f));
		CHECK(physics.HasBody(boxes.front()));
		PhysicsStats stats = physics.GetStats();
		CHECK(stats.AddBatchCount == batches + 2);
		CHECK(stats.SharedShapeCount == 2); // The floor's box and one box of half a meter for the 110 others
		CHECK(stats.DynamicBodyCount == 100);
		CHECK(stats.StaticBodyCount == 11);
		CHECK(stats.ActiveBodyCount == 100);

		// They simulate like any other body: the boxes land on the floor.
		StepScene(scene, 120);
		for (Entity box : boxes)
			CHECK(std::abs(GetWorldPosition(scene, box).y - 0.5f) < 0.05f);

		// Destroying bodies and creating others in one frame reuses the slots in a full world, without an error.
		Scene full("Full");
		full.GetSettings().PhysicsCapacity.MaxBodies = 4;
		full.GetSettings().Gravity = glm::vec3(0.0f);
		std::vector<Entity> first;
		for (uint32_t index = 0; index < 4; index++)
			first.push_back(CreateDynamicBox(full, "First", glm::vec3(2.0f * static_cast<float>(index), 0.0f, 0.0f)));
		full.OnRuntimeStart();
		PhysicsSystem& fullPhysics = GetPhysics(full);
		REQUIRE(fullPhysics.GetStats().BodyCount == 4);
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		full.DestroyEntities(std::vector<Entity>(first.begin(), first.begin() + 2));
		Entity replacement = CreateDynamicBox(full, "Replacement", glm::vec3(0.0f, 5.0f, 0.0f));
		Entity other = CreateDynamicBox(full, "Other", glm::vec3(0.0f, 10.0f, 0.0f));
		CHECK(fullPhysics.HasBody(replacement));
		CHECK(fullPhysics.HasBody(other));
		CHECK(fullPhysics.GetStats().BodyCount == 4);
		CHECK(fullPhysics.GetStats().BodyOverflowCount == 0);
		CHECK(CountLogMessages(logStart, "cannot create a body") == 0);
		full.OnRuntimeStop();
		scene.OnRuntimeStop();
	}

	TEST_CASE("Poses written back in parallel chunks equal those written one by one")
	{
		// Root bodies, dynamic bodies below a static parent (written in parallel against the parent's world transform) and
		// dynamic bodies below dynamic ones (written afterwards, parents first): the same poses with and without workers.
		const auto simulate = [](uint32_t workers)
		{
			Scope<ScopedJobSystem> jobs = workers > 0 ? CreateScope<ScopedJobSystem>(workers) : nullptr;
			Scene scene;
			CreateFloor(scene, 1600);
			for (uint32_t index = 0; index < 1500; index++)
				CreateDynamicBox(scene, "Box " + std::to_string(index), glm::vec3(1.5f * static_cast<float>(index % 40), 2.0f, 1.5f * static_cast<float>(index / 40)));

			Entity platform = CreateStaticBox(scene, "Platform", glm::vec3(-20.0f, 1.0f, 0.0f), glm::vec3(5.0f, 0.25f, 5.0f));
			platform.GetTransform().Rotation = glm::angleAxis(glm::radians(10.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			platform.GetTransform().Scale = glm::vec3(2.0f);
			for (uint32_t index = 0; index < 20; index++)
			{
				Entity child = scene.CreateChildEntity(platform, "Child " + std::to_string(index));
				child.GetTransform().Translation = glm::vec3(static_cast<float>(index % 5) - 2.0f, 1.0f, static_cast<float>(index / 5) - 2.0f);
				child.AddComponent<RigidBodyComponent>();
				child.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.2f);
			}
			for (uint32_t index = 0; index < 10; index++)
			{
				Entity parent = CreateDynamicBox(scene, "Carrier " + std::to_string(index), glm::vec3(1.5f * static_cast<float>(index), 6.0f, -10.0f));
				Entity rider = scene.CreateChildEntity(parent, "Rider " + std::to_string(index));
				rider.GetTransform().Translation = glm::vec3(0.0f, 1.5f, 0.0f);
				rider.AddComponent<RigidBodyComponent>();
				rider.AddComponent<SphereColliderComponent>().Radius = 0.3f;
			}

			scene.OnRuntimeStart();
			StepScene(scene, 40); // Landed, still awake
			CHECK(GetPhysics(scene).GetStats().WrittenBodyCount > 1000);
			std::string error;
			CHECK_MESSAGE(scene.ValidateWorldTransforms(&error), error);
			std::vector<glm::mat4> poses;
			for (const Entity entity : scene.GetEntitiesInHierarchyOrder())
				poses.push_back(scene.GetWorldTransform(entity));
			scene.OnRuntimeStop();
			return poses;
		};

		const std::vector<glm::mat4> serial = simulate(0);
		const std::vector<glm::mat4> parallel = simulate(4);
		REQUIRE(serial.size() == parallel.size());
		size_t different = 0;
		for (size_t index = 0; index < serial.size(); index++)
		{
			if (serial[index] != parallel[index])
				different++;
		}
		CHECK(different == 0);
	}

	TEST_CASE("Putting the moved bodies in order costs what moved, not the world's capacity")
	{
		// A world with room for the most bodies a scene may ask for (2^21 slots): the write-back reads the slots of the groups
		// of 64 that hold a moved body, not all of them.
		Scene scene;
		scene.GetSettings().PhysicsCapacity.MaxBodies = PhysicsCapacitySettings::c_MaxBodiesLimit;
		CreateFloor(scene, 400);
		CreateDynamicBox(scene, "Falling", glm::vec3(0.0f, 100.0f, 0.0f));
		scene.OnRuntimeStart();
		StepScene(scene, 2);
		PhysicsStats stats = GetPhysics(scene).GetStats();
		REQUIRE(stats.MaxBodies == PhysicsCapacitySettings::c_MaxBodiesLimit);
		CHECK(stats.WrittenBodyCount == 1);
		CHECK(stats.OrderedSlotCount == 64);

		// 201 falling bodies in the slots of the first 202 bodies: four groups.
		for (uint32_t index = 0; index < 200; index++)
			CreateDynamicBox(scene, "Box " + std::to_string(index), glm::vec3(1.5f * static_cast<float>(index % 20), 50.0f, 1.5f * static_cast<float>(index / 20)));
		StepScene(scene, 1);
		stats = GetPhysics(scene).GetStats();
		CHECK(stats.WrittenBodyCount == 201);
		CHECK(stats.OrderedSlotCount == 4 * 64);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Contact events do not depend on how many threads report them")
	{
		// Reports from Jolt's job threads go into per-thread buffers, merged after the step: the events must come out the
		// same, in the same order, with any number of workers.
		const auto record = [](uint32_t workers)
		{
			Scope<ScopedJobSystem> jobs = workers > 0 ? CreateScope<ScopedJobSystem>(workers) : nullptr;
			Scene scene;
			CreateFloor(scene, 400);
			std::vector<Entity> boxes;
			for (uint32_t index = 0; index < 400; index++)
			{
				// Stacked in pairs: each lower box touches the floor and the box above.
				const uint32_t column = index / 2;
				const glm::vec3 position(1.5f * static_cast<float>(column % 20), 0.5f + static_cast<float>(index % 2) * 1.05f, 1.5f * static_cast<float>(column / 20));
				boxes.push_back(CreateDynamicBox(scene, "Box " + std::to_string(index), position));
			}
			scene.OnRuntimeStart();
			CollisionRecorder recorder(GetPhysics(scene));
			StepScene(scene, 30);
			std::vector<std::string> events;
			for (const CollisionEvent& event : recorder.GetEvents())
				events.push_back(fmt::format("{} {} {}", event.Type == CollisionEventType::Begin ? "Begin" : "End", event.A.GetName(), event.B.GetName()));
			scene.OnRuntimeStop();
			return events;
		};

		const std::vector<std::string> singleThreaded = record(0);
		CHECK(singleThreaded.size() >= 400);
		CHECK(record(4) == singleThreaded);
	}
}
