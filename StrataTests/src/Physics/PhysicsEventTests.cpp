#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"
#include "Strata/Core/Hash.h"

#include <unordered_map>

using namespace Strata;
using namespace Strata::Tests;

TEST_SUITE("Physics.Events")
{
	TEST_CASE("Trigger volumes report balanced Begin and End events")
	{
		Scene scene;
		Entity trigger = CreateStaticBox(scene, "Trigger", glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f));
		trigger.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		trigger.GetComponent<RigidBodyComponent>().IsTrigger = true;
		Entity ball = CreateDynamicSphere(scene, "Ball", glm::vec3(0.0f, 3.0f, 0.0f), 0.25f);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics);

		StepScene(scene, 90);
		CHECK(GetWorldPosition(scene, ball).y < -2.0f); // Triggers do not stop bodies
		CHECK(recorder.GetEvents().size() == 2);
		CHECK(recorder.Count(CollisionEventType::Begin, trigger.GetUUID(), ball.GetUUID()) == 1);
		CHECK(recorder.Count(CollisionEventType::End, trigger.GetUUID(), ball.GetUUID()) == 1);
		REQUIRE(recorder.GetEvents().size() == 2);

		const CollisionEvent& begin = recorder.GetEvents()[0];
		const CollisionEvent& end = recorder.GetEvents()[1];
		CHECK(begin.Type == CollisionEventType::Begin);
		CHECK(end.Type == CollisionEventType::End);
		CHECK(begin.IsTrigger);
		CHECK(end.IsTrigger);
		CHECK(begin.Involves(trigger.GetUUID()));
		CHECK(begin.GetOther(trigger.GetUUID()) == ball.GetUUID());
		CHECK(begin.A.IsValid());
		CHECK(begin.B.IsValid());
		CHECK((begin.A == ball || begin.B == ball));
		CHECK(begin.Point.y < 1.5f);
		CHECK(begin.Point.y > 0.0f);
		CHECK(physics.GetStats().ContactPairCount == 0);
	}

	TEST_CASE("Solid collisions report the touching entities, contact point and normal")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 2.0f, 0.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics);
		StepScene(scene, 120);

		CHECK(recorder.Count(CollisionEventType::Begin, ground.GetUUID(), box.GetUUID()) == 1);
		CHECK(recorder.Count(CollisionEventType::End) == 0);
		const CollisionEvent* begin = recorder.Find(CollisionEventType::Begin, ground.GetUUID(), box.GetUUID());
		REQUIRE(begin);
		CHECK_FALSE(begin->IsTrigger);
		CHECK(std::abs(begin->Point.y) < 0.05f);
		CHECK(std::abs(begin->Point.x) < 0.51f);
		// The normal points from A towards B: up if A is the ground, down if A is the box.
		const glm::vec3 expectedNormal = begin->AID == ground.GetUUID() ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, -1.0f, 0.0f);
		CHECK(Math::IsNearlyEqual(begin->Normal, expectedNormal, 1.0e-3f));
		CHECK(physics.GetStats().ContactPairCount == 1);
	}

	TEST_CASE("Sleeping bodies keep their contacts")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 1.0f, 0.0f));
		Entity sensor = CreateStaticBox(scene, "Sensor", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(2.0f));
		sensor.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		sensor.GetComponent<RigidBodyComponent>().IsTrigger = true;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics);
		StepScene(scene, 120);
		REQUIRE(physics.IsSleeping(box));
		CHECK(recorder.Count(CollisionEventType::Begin) == 2); // Ground and sensor
		CHECK(recorder.Count(CollisionEventType::End) == 0);

		// Waking the body up re-reports its contacts to the simulation, which must not repeat Begin events.
		CHECK(physics.WakeUp(box));
		CHECK_FALSE(physics.IsSleeping(box));
		StepScene(scene, 120);
		CHECK(recorder.Count(CollisionEventType::Begin) == 2);
		CHECK(recorder.Count(CollisionEventType::End) == 0);

		// Lifting it out of both ends both contacts.
		CHECK(physics.Teleport(box, glm::vec3(0.0f, 10.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
		StepScene(scene, 1);
		CHECK(recorder.Count(CollisionEventType::End, ground.GetUUID(), box.GetUUID()) == 1);
		CHECK(recorder.Count(CollisionEventType::End, sensor.GetUUID(), box.GetUUID()) == 1);
	}

	TEST_CASE("Destroying a touching entity ends its contacts")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f));
		Entity crate = CreateDynamicBox(scene, "Crate", glm::vec3(5.0f, 0.5f, 0.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics);
		StepScene(scene, 60);
		REQUIRE(recorder.Count(CollisionEventType::Begin) == 2);

		// Destroyed between updates: the End event arrives with the next update, with the entity's UUID but no handle.
		const UUID boxID = box.GetUUID();
		scene.DestroyEntity(box);
		StepScene(scene, 1);
		REQUIRE(recorder.Count(CollisionEventType::End, ground.GetUUID(), boxID) == 1);
		const CollisionEvent* end = recorder.Find(CollisionEventType::End, ground.GetUUID(), boxID);
		REQUIRE(end);
		CHECK((end->AID == boxID ? !end->A.IsValid() : !end->B.IsValid()));
		CHECK((end->AID == ground.GetUUID() ? end->A == ground : end->B == ground));

		// Destroying the ground wakes the crate resting on it, which falls.
		REQUIRE(physics.IsSleeping(crate));
		const UUID groundID = ground.GetUUID();
		scene.DestroyEntity(ground);
		StepScene(scene, 30);
		CHECK(recorder.Count(CollisionEventType::End, groundID, crate.GetUUID()) == 1);
		CHECK(GetWorldPosition(scene, crate).y < 0.0f);
		CHECK(recorder.Count(CollisionEventType::Begin) == recorder.Count(CollisionEventType::End));
	}

	TEST_CASE("Removing a collider or deactivating an entity ends its contacts")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity first = CreateDynamicBox(scene, "First", glm::vec3(0.0f, 0.5f, 0.0f));
		Entity second = CreateDynamicBox(scene, "Second", glm::vec3(3.0f, 0.5f, 0.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics);
		StepScene(scene, 10);
		REQUIRE(recorder.Count(CollisionEventType::Begin) == 2);

		first.RemoveComponent<BoxColliderComponent>();
		second.SetActive(false);
		StepScene(scene, 1);
		CHECK(recorder.Count(CollisionEventType::End, ground.GetUUID(), first.GetUUID()) == 1);
		CHECK(recorder.Count(CollisionEventType::End, ground.GetUUID(), second.GetUUID()) == 1);

		// Reactivated, the body touches the ground again.
		second.SetActive(true);
		StepScene(scene, 2);
		CHECK(recorder.Count(CollisionEventType::Begin, ground.GetUUID(), second.GetUUID()) == 2);
	}

	TEST_CASE("Static triggers detect kinematic bodies moving through them")
	{
		Scene scene;
		Entity trigger = CreateStaticBox(scene, "Trigger", glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(1.0f));
		trigger.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		trigger.GetComponent<RigidBodyComponent>().IsTrigger = true;
		Entity mover = CreateDynamicBox(scene, "Mover", glm::vec3(0.0f));
		mover.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics);
		for (int frame = 0; frame < 60; frame++)
		{
			mover.GetTransform().Translation.x += 0.1f;
			StepScene(scene, 1);
		}

		CHECK(recorder.Count(CollisionEventType::Begin, trigger.GetUUID(), mover.GetUUID()) == 1);
		CHECK(recorder.Count(CollisionEventType::End, trigger.GetUUID(), mover.GetUUID()) == 1);
		const CollisionEvent* begin = recorder.Find(CollisionEventType::Begin, trigger.GetUUID(), mover.GetUUID());
		REQUIRE(begin);
		CHECK(begin->IsTrigger);
		CHECK(recorder.GetEvents().front().Type == CollisionEventType::Begin);
	}

	TEST_CASE("Kinematic triggers detect static colliders")
	{
		Scene scene;
		Entity wall = CreateStaticBox(scene, "Wall", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.5f));
		Entity probe = CreateDynamicBox(scene, "Probe", glm::vec3(0.0f, 0.0f, 0.0f));
		RigidBodyComponent& rigidBody = probe.GetComponent<RigidBodyComponent>();
		rigidBody.Type = RigidBodyType::Kinematic;
		rigidBody.IsTrigger = true;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics);
		for (int frame = 0; frame < 60; frame++)
		{
			probe.GetTransform().Translation.x += 0.1f;
			StepScene(scene, 1);
		}

		CHECK(recorder.Count(CollisionEventType::Begin, wall.GetUUID(), probe.GetUUID()) == 1);
		const CollisionEvent* begin = recorder.Find(CollisionEventType::Begin, wall.GetUUID(), probe.GetUUID());
		REQUIRE(begin);
		CHECK(begin->IsTrigger);
	}

	TEST_CASE("Listeners can be removed, also while events are dispatched")
	{
		Scene scene;
		CreateGround(scene);
		CreateDynamicBox(scene, "First", glm::vec3(0.0f, 0.6f, 0.0f));
		CreateDynamicBox(scene, "Second", glm::vec3(3.0f, 0.6f, 0.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);

		int selfRemovingCalls = 0;
		int laterCalls = 0;
		CollisionListenerID selfRemoving = c_InvalidCollisionListener;
		CollisionListenerID later = c_InvalidCollisionListener;
		selfRemoving = physics.AddCollisionListener([&](const CollisionEvent&)
		{
			selfRemovingCalls++;
			CHECK(physics.RemoveCollisionListener(selfRemoving));
			CHECK(physics.RemoveCollisionListener(later)); // Not called for the rest of this dispatch either
		});
		later = physics.AddCollisionListener([&](const CollisionEvent&) { laterCalls++; });
		CollisionRecorder recorder(physics);

		CHECK(physics.AddCollisionListener(CollisionCallback()) == c_InvalidCollisionListener);
		CHECK_FALSE(physics.RemoveCollisionListener(c_InvalidCollisionListener));

		StepScene(scene, 30);
		CHECK(recorder.Count(CollisionEventType::Begin) == 2); // Both Begin events reached the remaining listener
		CHECK(selfRemovingCalls == 1);
		CHECK(laterCalls == 0);
		CHECK_FALSE(physics.RemoveCollisionListener(selfRemoving));
	}

	TEST_CASE("Listeners may destroy entities while handling events")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity pickup = CreateStaticBox(scene, "Pickup", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.5f));
		pickup.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		pickup.GetComponent<RigidBodyComponent>().IsTrigger = true;
		Entity player = CreateDynamicSphere(scene, "Player", glm::vec3(0.0f, 4.0f, 0.0f), 0.25f);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const UUID pickupID = pickup.GetUUID();
		physics.AddCollisionListener([&](const CollisionEvent& event)
		{
			if (event.Type == CollisionEventType::Begin && event.Involves(pickupID))
				scene.DestroyEntity(scene.GetEntityByUUID(pickupID));
		});
		CollisionRecorder recorder(physics);

		StepScene(scene, 120);
		CHECK_FALSE(scene.GetEntityByUUID(pickupID).IsValid());
		CHECK(recorder.Count(CollisionEventType::Begin, pickupID, player.GetUUID()) == 1);
		CHECK(recorder.Count(CollisionEventType::End, pickupID, player.GetUUID()) == 1);
		CHECK(recorder.Count(CollisionEventType::Begin, ground.GetUUID(), player.GetUUID()) == 1);
	}

	TEST_CASE("Begin and End stay balanced in a busy scene")
	{
		Scene scene;
		CreateGround(scene);
		std::vector<Entity> bodies;
		for (int index = 0; index < 20; index++)
			bodies.push_back(CreateDynamicBox(scene, "Box", glm::vec3(static_cast<float>(index % 2) * 0.3f, 0.6f + static_cast<float>(index) * 1.1f, 0.0f), glm::vec3(0.5f)));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics);
		StepScene(scene, 120);

		const size_t begun = recorder.Count(CollisionEventType::Begin);
		const size_t ended = recorder.Count(CollisionEventType::End);
		CHECK(begun >= 20);
		CHECK(begun - ended == physics.GetStats().ContactPairCount);

		// Every pair begins before it ends, and never begins twice while touching.
		std::unordered_map<uint64_t, int> touching;
		for (const CollisionEvent& event : recorder.GetEvents())
		{
			const uint64_t key = Hash::Combine(std::min<uint64_t>(event.AID, event.BID), std::max<uint64_t>(event.AID, event.BID));
			int& count = touching[key];
			count += event.Type == CollisionEventType::Begin ? 1 : -1;
			CHECK(count >= 0);
			CHECK(count <= 1);
		}

		for (Entity body : bodies)
			scene.DestroyEntity(body);
		StepScene(scene, 1);
		CHECK(recorder.Count(CollisionEventType::Begin) == recorder.Count(CollisionEventType::End));
		CHECK(physics.GetStats().ContactPairCount == 0);
	}
}
