#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A car: a dynamic chassis with a cabin (a collider-only child, rotated) and an antenna on the cabin (a collider-only
	// grandchild, scaled).
	struct Car
	{
		Entity Chassis;
		Entity Cabin;
		Entity Antenna;
	};

	Car CreateCar(Scene& scene, const glm::vec3& position)
	{
		Car car;
		car.Chassis = CreateDynamicBox(scene, "Chassis", position, glm::vec3(1.0f, 0.25f, 2.0f));
		car.Cabin = scene.CreateChildEntity(car.Chassis, "Cabin");
		car.Cabin.GetTransform().Translation = glm::vec3(0.0f, 0.75f, 0.5f);
		car.Cabin.GetTransform().Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		car.Cabin.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.5f);
		car.Antenna = scene.CreateChildEntity(car.Cabin, "Antenna");
		car.Antenna.GetTransform().Translation = glm::vec3(0.0f, 1.0f, 0.0f);
		car.Antenna.GetTransform().Scale = glm::vec3(2.0f);
		car.Antenna.AddComponent<SphereColliderComponent>().Radius = 0.1f;
		return car;
	}

	std::optional<RaycastHit> CastDown(PhysicsSystem& physics, const glm::vec3& position)
	{
		return physics.Raycast(glm::vec3(position.x, 20.0f, position.z), glm::vec3(0.0f, -1.0f, 0.0f), 40.0f);
	}

}

TEST_SUITE("Physics.Hierarchy")
{
	TEST_CASE("Colliders of descendants without a rigid body belong to the ancestor's body")
	{
		Scene scene;
		CreateGround(scene);
		Car car = CreateCar(scene, glm::vec3(0.0f, 1.0f, 0.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK(physics.HasBody(car.Chassis));
		CHECK_FALSE(physics.HasBody(car.Cabin));
		CHECK_FALSE(physics.HasBody(car.Antenna));
		CHECK(physics.GetBodyEntity(car.Cabin) == car.Chassis);
		CHECK(physics.GetBodyEntity(car.Antenna) == car.Chassis);
		CHECK(physics.GetStats().BodyCount == 2); // Ground and car

		// The cabin and the antenna are part of the car's shape (hits report the body's entity).
		std::optional<RaycastHit> cabinHit = CastDown(physics, glm::vec3(0.0f, 0.0f, 0.2f));
		REQUIRE(cabinHit);
		CHECK(cabinHit->HitEntity == car.Chassis);
		CHECK(cabinHit->Point.y == doctest::Approx(1.0f + 0.75f + 0.5f).epsilon(1.0e-3)); // Cabin top
		std::optional<RaycastHit> antennaHit = CastDown(physics, glm::vec3(0.0f, 0.0f, 0.5f));
		REQUIRE(antennaHit);
		CHECK(antennaHit->Point.y == doctest::Approx(1.0f + 0.75f + 1.0f + 0.2f).epsilon(1.0e-3));

		// The car falls as one body and is not pushed around by its own cabin.
		StepScene(scene, 150);
		const glm::vec3 position = GetWorldPosition(scene, car.Chassis);
		CHECK(std::abs(position.y - 0.25f) < 0.03f);
		CHECK(std::abs(position.x) < 1.0e-3f);
		CHECK(std::abs(position.z) < 1.0e-3f);
		CHECK(Math::IsNearlyEqual(GetWorldRotation(scene, car.Chassis), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0e-3f));
		CHECK(Math::IsNearlyEqual(GetWorldPosition(scene, car.Cabin), position + glm::vec3(0.0f, 0.75f, 0.5f), 1.0e-3f));
		CHECK(physics.GetStats().ContactPairCount == 1); // Only the car touching the ground
	}

	TEST_CASE("Descendants with their own rigid body and collider entities without one are separate bodies")
	{
		Scene scene;
		Entity truck = CreateDynamicBox(scene, "Truck", glm::vec3(0.0f, 5.0f, 0.0f));
		Entity trailer = CreateDynamicBox(scene, "Trailer", glm::vec3(0.0f, 5.0f, 5.0f));
		scene.SetParent(trailer, truck);
		Entity cargo = scene.CreateChildEntity(trailer, "Cargo");
		cargo.AddComponent<SphereColliderComponent>().Offset = glm::vec3(0.0f, 1.0f, 0.0f);

		Entity post = CreateStaticBox(scene, "Post", glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(0.25f, 2.0f, 0.25f));
		Entity sign = scene.CreateChildEntity(post, "Sign");
		sign.GetTransform().Translation = glm::vec3(0.0f, 2.5f, 0.0f);
		sign.AddComponent<BoxColliderComponent>();

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK(physics.GetBodyEntity(trailer) == trailer);
		CHECK(physics.GetBodyEntity(cargo) == trailer);
		CHECK(physics.HasBody(post));
		CHECK(physics.HasBody(sign));
		CHECK(physics.GetBodyEntity(sign) == sign);
		const PhysicsStats stats = physics.GetStats();
		CHECK(stats.DynamicBodyCount == 2);
		CHECK(stats.StaticBodyCount == 2);
	}

	TEST_CASE("Rigid bodies added to or removed from descendants regroup the colliders")
	{
		Scene scene;
		Car car = CreateCar(scene, glm::vec3(0.0f, 5.0f, 0.0f));
		car.Chassis.GetComponent<RigidBodyComponent>().GravityScale = 0.0f;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(physics.GetBodyEntity(car.Antenna) == car.Chassis);

		// The cabin becomes a body of its own, taking the antenna with it.
		car.Cabin.AddComponent<RigidBodyComponent>().GravityScale = 0.0f;
		CHECK(physics.HasBody(car.Cabin));
		CHECK(physics.GetBodyEntity(car.Antenna) == car.Cabin);
		CHECK(physics.GetStats().DynamicBodyCount == 2);

		// And merges back when it loses it.
		car.Cabin.RemoveComponent<RigidBodyComponent>();
		CHECK_FALSE(physics.HasBody(car.Cabin));
		CHECK(physics.GetBodyEntity(car.Cabin) == car.Chassis);
		CHECK(physics.GetBodyEntity(car.Antenna) == car.Chassis);
		CHECK(physics.GetStats().DynamicBodyCount == 1);

		// New collider children join the body.
		Entity bumper = scene.CreateChildEntity(car.Chassis, "Bumper");
		bumper.GetTransform().Translation = glm::vec3(0.0f, 0.0f, 2.5f);
		bumper.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(1.0f, 0.2f, 0.2f);
		CHECK(physics.GetBodyEntity(bumper) == car.Chassis);
		std::optional<RaycastHit> bumperHit = CastDown(physics, glm::vec3(0.0f, 0.0f, 2.6f));
		REQUIRE(bumperHit);
		CHECK(bumperHit->HitEntity == car.Chassis);
		CHECK(bumperHit->Point.y == doctest::Approx(5.2f).epsilon(1.0e-3));

		// Without the chassis' rigid body every collider entity is a static body of its own.
		car.Chassis.RemoveComponent<RigidBodyComponent>();
		CHECK(physics.GetBodyEntity(car.Cabin) == car.Cabin);
		CHECK(physics.GetBodyEntity(car.Antenna) == car.Antenna);
		CHECK(physics.GetBodyEntity(bumper) == bumper);
		const PhysicsStats stats = physics.GetStats();
		CHECK(stats.DynamicBodyCount == 0);
		CHECK(stats.StaticBodyCount == 4);
	}

	TEST_CASE("Moving or deactivating a merged collider changes its body's shape")
	{
		Scene scene;
		Car car = CreateCar(scene, glm::vec3(0.0f, 5.0f, 0.0f));
		car.Chassis.GetComponent<RigidBodyComponent>().GravityScale = 0.0f;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const glm::vec3 cabinTop(0.0f, 0.0f, 0.2f);
		REQUIRE(CastDown(physics, cabinTop));
		CHECK(CastDown(physics, cabinTop)->Point.y == doctest::Approx(6.25f).epsilon(1.0e-3));

		// Moved through a signaled transform edit: the cabin (and the antenna on it) moved within the body.
		car.Cabin.GetTransform().Translation.y = 1.75f;
		car.Cabin.MarkModified<TransformComponent>();
		CHECK(CastDown(physics, cabinTop)->Point.y == doctest::Approx(7.25f).epsilon(1.0e-3));

		car.Cabin.SetActive(false);
		std::optional<RaycastHit> withoutCabin = CastDown(physics, cabinTop);
		REQUIRE(withoutCabin);
		CHECK(withoutCabin->Point.y == doctest::Approx(5.25f).epsilon(1.0e-3)); // Chassis top
		CHECK(physics.HasBody(car.Chassis));

		car.Cabin.SetActive(true);
		CHECK(CastDown(physics, cabinTop)->Point.y == doctest::Approx(7.25f).epsilon(1.0e-3));
	}

	TEST_CASE("Reparenting colliders at runtime moves them between bodies")
	{
		Scene scene;
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		Entity player = CreateDynamicBox(scene, "Player", glm::vec3(0.0f, 5.0f, 0.0f));
		Entity enemy = CreateDynamicBox(scene, "Enemy", glm::vec3(10.0f, 5.0f, 0.0f));
		for (Entity body : { player, enemy })
			body.GetComponent<RigidBodyComponent>().LinearDamping = 0.0f;
		// A collider-only item lying against the player's +X face: a static body of its own.
		Entity sword = CreateStaticBox(scene, "Sword", glm::vec3(1.0f, 5.0f, 0.0f), glm::vec3(0.5f, 0.1f, 0.1f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics);
		StepScene(scene, 1);
		REQUIRE(physics.GetBodyEntity(sword) == sword);
		REQUIRE(recorder.Count(CollisionEventType::Begin, player.GetUUID(), sword.GetUUID()) == 1);

		// Picked up: the sword becomes part of the player's shape and stops touching it.
		CHECK(scene.SetParent(sword, player));
		CHECK(physics.GetBodyEntity(sword) == player);
		CHECK_FALSE(physics.HasBody(sword));
		CHECK(physics.GetStats().StaticBodyCount == 0);
		std::optional<RaycastHit> hit = CastDown(physics, glm::vec3(1.3f, 0.0f, 0.0f));
		REQUIRE(hit);
		CHECK(hit->HitEntity == player);
		CHECK(hit->Point.y == doctest::Approx(5.1f).epsilon(1.0e-3));
		StepScene(scene, 1);
		CHECK(recorder.Count(CollisionEventType::End, player.GetUUID(), sword.GetUUID()) == 1);
		CHECK(physics.GetStats().ContactPairCount == 0);

		// The player carries it along instead of running into it.
		CHECK(physics.SetLinearVelocity(player, glm::vec3(2.0f, 0.0f, 0.0f)));
		StepScene(scene, 30);
		const float playerX = GetWorldPosition(scene, player).x;
		CHECK(playerX == doctest::Approx(1.0f).epsilon(0.01));
		CHECK(GetWorldPosition(scene, sword).x == doctest::Approx(playerX + 1.0f).epsilon(0.01));
		hit = CastDown(physics, glm::vec3(playerX + 1.3f, 0.0f, 0.0f));
		REQUIRE(hit);
		CHECK(hit->HitEntity == player);
		CHECK(physics.GetStats().ContactPairCount == 0);

		// Dropped: a static body of its own again where it was let go, no longer part of the player's shape.
		CHECK(physics.SetLinearVelocity(player, glm::vec3(0.0f, 0.0f, -2.0f)));
		CHECK(scene.SetParent(sword, Entity()));
		CHECK(physics.GetBodyEntity(sword) == sword);
		CHECK(physics.HasBody(sword));
		CHECK(physics.GetStats().StaticBodyCount == 1);
		StepScene(scene, 30);
		const glm::vec3 droppedAt = GetWorldPosition(scene, sword);
		CHECK(droppedAt.x == doctest::Approx(playerX + 1.0f).epsilon(0.01));
		CHECK(std::abs(droppedAt.z) < 1.0e-4f);
		const glm::vec3 playerPosition = GetWorldPosition(scene, player);
		CHECK(playerPosition.z == doctest::Approx(-1.0f).epsilon(0.02));
		hit = CastDown(physics, droppedAt);
		REQUIRE(hit);
		CHECK(hit->HitEntity == sword);
		CHECK_FALSE(CastDown(physics, playerPosition + glm::vec3(1.3f, 0.0f, 0.0f)));
		// It touched the player when dropped, and stopped touching when the player walked away.
		CHECK(recorder.Count(CollisionEventType::Begin, player.GetUUID(), sword.GetUUID()) == 2);
		CHECK(recorder.Count(CollisionEventType::End, player.GetUUID(), sword.GetUUID()) == 2);

		// Handed from one body to another.
		sword.GetTransform().Translation = glm::vec3(1.0f, 0.0f, 0.0f);
		CHECK(scene.SetParent(sword, enemy, false));
		CHECK(physics.GetBodyEntity(sword) == enemy);
		CHECK(physics.GetStats().StaticBodyCount == 0);
		hit = CastDown(physics, glm::vec3(11.3f, 0.0f, 0.0f));
		REQUIRE(hit);
		CHECK(hit->HitEntity == enemy);
		CHECK(scene.SetParent(sword, player, false));
		CHECK(physics.GetBodyEntity(sword) == player);
		CHECK_FALSE(CastDown(physics, glm::vec3(11.3f, 0.0f, 0.0f)));
		hit = CastDown(physics, playerPosition + glm::vec3(1.3f, 0.0f, 0.0f));
		REQUIRE(hit);
		CHECK(hit->HitEntity == player);

		// A rigid body moved under another one stays a body of its own.
		CHECK(scene.SetParent(enemy, player));
		CHECK(physics.GetBodyEntity(enemy) == enemy);
		CHECK(physics.HasBody(enemy));
		CHECK(physics.GetStats().DynamicBodyCount == 2);
		StepScene(scene, 1);
		CHECK(recorder.Count(CollisionEventType::Begin) == recorder.Count(CollisionEventType::End));
	}

	TEST_CASE("Reparenting rebuilds only the bodies whose colliders change")
	{
		Scene scene;
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		Car car = CreateCar(scene, glm::vec3(0.0f, 5.0f, 0.0f));
		Entity mount = scene.CreateChildEntity(car.Chassis, "Mount");
		mount.GetTransform().Translation = glm::vec3(0.0f, 0.5f, -1.5f);
		Entity truck = CreateDynamicBox(scene, "Truck", glm::vec3(10.0f, 5.0f, 0.0f));
		Entity post = CreateStaticBox(scene, "Post", glm::vec3(20.0f, 1.0f, 0.0f), glm::vec3(0.25f, 1.0f, 0.25f));
		Entity garage = scene.CreateEntity("Garage");

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const auto builds = [&]()
		{
			physics.HasBody(car.Chassis); // Applies the pending changes
			return physics.GetStats().BuildCount;
		};
		const uint64_t initialBuilds = builds();
		const auto antennaTop = [&]()
		{
			std::optional<RaycastHit> hit = CastDown(physics, GetWorldPosition(scene, car.Antenna));
			return hit && hit->HitEntity == car.Chassis ? hit->Point.y : -1.0f;
		};
		const float antennaHeight = antennaTop();
		REQUIRE(antennaHeight > 6.0f);

		// Bodies moved under other entities, keeping their pose, stay as they are (a rigid body below another one stays a
		// body of its own).
		CHECK(scene.SetParent(car.Chassis, garage));
		CHECK(scene.SetParent(post, garage));
		CHECK(scene.SetParent(truck, car.Chassis));
		CHECK(builds() == initialBuilds);
		CHECK(physics.GetBodyEntity(car.Antenna) == car.Chassis);
		CHECK(physics.GetBodyEntity(truck) == truck);
		CHECK(antennaTop() == doctest::Approx(antennaHeight).epsilon(1.0e-4));

		// A merged collider moved within its body without changing its pose does not change the body either...
		CHECK(scene.SetParent(car.Antenna, mount));
		CHECK(builds() == initialBuilds);
		CHECK(antennaTop() == doctest::Approx(antennaHeight).epsilon(1.0e-4));
		// ...but moved relative to the body (the local transform kept, now relative to the chassis), it does.
		CHECK(scene.SetParent(car.Antenna, car.Chassis, false));
		CHECK(builds() == initialBuilds + 1);
		CHECK(antennaTop() == doctest::Approx(5.0f + 1.25f + 0.2f).epsilon(1.0e-3)); // 1.25 above the mount before

		// A collider handed to another body rebuilds both bodies.
		CHECK(scene.SetParent(car.Cabin, truck));
		CHECK(builds() == initialBuilds + 3);
		CHECK(physics.GetBodyEntity(car.Cabin) == truck);

		// Activity still follows the hierarchy: below an inactive entity a body leaves the simulation, without a rebuild.
		Entity storage = scene.CreateEntity("Storage");
		storage.SetActive(false);
		CHECK(scene.SetParent(post, storage));
		CHECK_FALSE(physics.HasBody(post));
		CHECK(scene.SetParent(post, garage));
		CHECK(physics.HasBody(post));
		CHECK(builds() == initialBuilds + 3);
		StepScene(scene, 1);
		std::optional<RaycastHit> postHit = CastDown(physics, GetWorldPosition(scene, post));
		REQUIRE(postHit);
		CHECK(postHit->HitEntity == post);
	}

	TEST_CASE("Bodies below a body moved without a signal follow it in the same step")
	{
		Scene scene;
		CreateGround(scene);
		// A kinematic platform carrying a static railing and a kinematic door, bodies of their own.
		Entity platform = CreateDynamicBox(scene, "Platform", glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(2.0f, 0.25f, 2.0f));
		platform.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		Entity railing = CreateStaticBox(scene, "Railing", glm::vec3(1.5f, 1.0f, 0.0f), glm::vec3(0.1f, 0.5f, 2.0f));
		railing.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		REQUIRE(scene.SetParent(railing, platform, false)); // World (1.5, 2, 0)
		Entity door = CreateStaticBox(scene, "Door", glm::vec3(-1.5f, 1.0f, 0.0f), glm::vec3(0.1f, 0.5f, 1.0f));
		door.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		REQUIRE(scene.SetParent(door, platform, false)); // World (-1.5, 2, 0)

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 90);
		REQUIRE(physics.IsSleeping(platform));
		REQUIRE(physics.IsSleeping(door));

		// Driven by direct writes every frame, which the awake platform notices without a signal.
		CHECK(physics.WakeUp(platform));
		for (int frame = 0; frame < 30; frame++)
		{
			platform.GetTransform().Translation.y += 0.1f;
			StepScene(scene, 1);
		}
		REQUIRE(GetWorldPosition(scene, platform).y == doctest::Approx(4.0f));
		for (Entity rider : { railing, door })
		{
			const glm::vec3 position = GetWorldPosition(scene, rider);
			std::optional<RaycastHit> hit = CastDown(physics, position);
			REQUIRE(hit);
			CHECK(hit->HitEntity == rider);
			CHECK(hit->Point.y == doctest::Approx(position.y + 0.5f).epsilon(1.0e-3));
		}
		// Nothing is left behind where the riders started.
		std::optional<RaycastHit> below = physics.Raycast(glm::vec3(1.5f, 3.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(below);
		CHECK(below->HitEntity != railing);
	}

	TEST_CASE("Sleeping dynamic children move along with a parent moved without a signal")
	{
		Scene scene;
		CreateGround(scene);
		Entity parent = CreateDynamicBox(scene, "Parent", glm::vec3(0.0f, 5.0f, 0.0f));
		parent.GetComponent<RigidBodyComponent>().GravityScale = 0.0f;
		Entity child = CreateDynamicBox(scene, "Child", glm::vec3(10.0f, 0.5f, 0.0f));
		REQUIRE(scene.SetParent(child, parent));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 90);
		REQUIRE(physics.IsSleeping(child));

		// The awake parent is moved by a direct write: the child moves along, as after a signaled edit.
		CHECK(physics.WakeUp(parent));
		parent.GetTransform().Translation.x += 3.0f;
		StepScene(scene, 1);
		CHECK(GetWorldPosition(scene, parent).x == doctest::Approx(3.0f).epsilon(1.0e-3));
		CHECK(GetWorldPosition(scene, child).x == doctest::Approx(13.0f).epsilon(1.0e-3));
		std::optional<RaycastHit> hit = CastDown(physics, glm::vec3(13.0f, 0.0f, 0.0f));
		REQUIRE(hit);
		CHECK(hit->HitEntity == child);
		StepScene(scene, 30);
		CHECK(GetWorldPosition(scene, child).x == doctest::Approx(13.0f).epsilon(1.0e-3));
	}

	TEST_CASE("Bodies rebuilt while inactive keep their merged colliders")
	{
		Scene scene;
		Car car = CreateCar(scene, glm::vec3(0.0f, 5.0f, 0.0f));
		car.Chassis.GetComponent<RigidBodyComponent>().GravityScale = 0.0f;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		car.Chassis.SetActive(false);
		car.Chassis.GetComponent<RigidBodyComponent>().Mass = 3.0f;
		car.Chassis.MarkModified<RigidBodyComponent>();
		CHECK_FALSE(physics.HasBody(car.Chassis));

		car.Chassis.SetActive(true);
		std::optional<RaycastHit> cabinHit = CastDown(physics, glm::vec3(0.0f, 0.0f, 0.2f));
		REQUIRE(cabinHit);
		CHECK(cabinHit->HitEntity == car.Chassis);
		CHECK(cabinHit->Point.y == doctest::Approx(6.25f).epsilon(1.0e-3));
	}

	TEST_CASE("Dynamic descendants keep their world pose when their parent's body moves")
	{
		Scene scene;
		CreateGround(scene);
		Entity parent = CreateDynamicBox(scene, "Parent", glm::vec3(0.0f, 5.0f, 0.0f));
		RigidBodyComponent& parentBody = parent.GetComponent<RigidBodyComponent>();
		parentBody.GravityScale = 0.0f;
		parentBody.LinearDamping = 0.0f;
		Entity child = CreateDynamicBox(scene, "Child", glm::vec3(10.0f, 0.5f, 0.0f));
		scene.SetParent(child, parent);
		// Static bodies follow their moving parent instead.
		Entity sign = CreateStaticBox(scene, "Sign", glm::vec3(0.0f, 7.0f, 0.0f), glm::vec3(0.5f));
		sign.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		scene.SetParent(sign, parent);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 60);
		REQUIRE(physics.IsSleeping(child));
		const glm::vec3 childPosition = GetWorldPosition(scene, child);

		CHECK(physics.SetLinearVelocity(parent, glm::vec3(0.0f, 0.0f, 2.0f)));
		StepScene(scene, 60);
		CHECK(GetWorldPosition(scene, parent).z == doctest::Approx(2.0f).epsilon(0.02));
		CHECK(Math::IsNearlyEqual(GetWorldPosition(scene, child), childPosition, 1.0e-4f));
		CHECK(physics.IsSleeping(child));

		StepScene(scene, 1);
		std::optional<RaycastHit> signHit = CastDown(physics, glm::vec3(0.0f, 0.0f, GetWorldPosition(scene, parent).z));
		REQUIRE(signHit);
		CHECK(signHit->HitEntity == sign);
		CHECK(signHit->Point.y == doctest::Approx(7.5f).epsilon(1.0e-3));
	}
}
