#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/Log.h"
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

	size_t CountDroppedContactWarnings(uint64_t logStart)
	{
		return CountLogMessages(logStart, "contacts were dropped") + CountLogMessages(logStart, "is full");
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

		// Every box touches the floor: one Enter each, and no buffer overflowed.
		const ScriptSystem& scripts = GetScriptSystem(scene);
		CHECK(GetField<int32_t>(scripts, floor, "ContactRecorder", "CollisionEnters") == 5000);
		CHECK(GetField<int32_t>(scripts, floor, "ContactRecorder", "CollisionExits") == 0);
		CHECK(CountDroppedContactWarnings(logStart) == 0);
		PhysicsStats stats = physics.GetStats();
		CHECK(stats.ContactOverflowSteps == 0);
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
		CHECK(CountDroppedContactWarnings(logStart) == 0);
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
