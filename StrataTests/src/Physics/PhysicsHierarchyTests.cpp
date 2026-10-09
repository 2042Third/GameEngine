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
