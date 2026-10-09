#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"

#include <limits>

using namespace Strata;
using namespace Strata::Tests;

TEST_SUITE("Physics.Queries")
{
	TEST_CASE("Raycast reports the closest hit")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity crate = CreateStaticBox(scene, "Crate", glm::vec3(3.0f, 1.0f, 0.0f), glm::vec3(1.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);

		std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 100.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == ground);
		CHECK(hit->EntityID == ground.GetUUID());
		CHECK(Math::IsNearlyEqual(hit->Point, glm::vec3(0.0f, 0.0f, 0.0f), 1.0e-4f));
		CHECK(Math::IsNearlyEqual(hit->Normal, glm::vec3(0.0f, 1.0f, 0.0f), 1.0e-4f));
		CHECK(hit->Distance == doctest::Approx(5.0f));

		// The direction does not need to be normalized; the closest of several bodies wins.
		hit = physics.Raycast(glm::vec3(-5.0f, 1.0f, 0.0f), glm::vec3(10.0f, 0.0f, 0.0f), 100.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == crate);
		CHECK(Math::IsNearlyEqual(hit->Point, glm::vec3(2.0f, 1.0f, 0.0f), 1.0e-4f));
		CHECK(Math::IsNearlyEqual(hit->Normal, glm::vec3(-1.0f, 0.0f, 0.0f), 1.0e-4f));
		CHECK(hit->Distance == doctest::Approx(7.0f));

		hit = physics.Raycast(glm::vec3(3.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 100.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == crate);
		CHECK(hit->Point.y == doctest::Approx(2.0f));
	}

	TEST_CASE("Raycast misses and rejects invalid rays")
	{
		Scene scene;
		CreateGround(scene);
		Entity crate = CreateStaticBox(scene, "Crate", glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(1.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const glm::vec3 origin(0.0f, 10.0f, 0.0f);
		const glm::vec3 down(0.0f, -1.0f, 0.0f);

		CHECK_FALSE(physics.Raycast(origin, glm::vec3(0.0f, 1.0f, 0.0f), 100.0f));
		CHECK_FALSE(physics.Raycast(origin, down, 3.9f)); // The crate's top is 4 units away
		CHECK(physics.Raycast(origin, down, 4.1f));
		CHECK(physics.Raycast(origin, down, std::numeric_limits<float>::infinity())); // Clamped, not rejected

		const float nan = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(physics.Raycast(origin, glm::vec3(0.0f), 100.0f));
		CHECK_FALSE(physics.Raycast(origin, glm::vec3(nan, -1.0f, 0.0f), 100.0f));
		CHECK_FALSE(physics.Raycast(glm::vec3(nan), down, 100.0f));
		CHECK_FALSE(physics.Raycast(origin, down, 0.0f));
		CHECK_FALSE(physics.Raycast(origin, down, -5.0f));
		CHECK_FALSE(physics.Raycast(origin, down, nan));
		CHECK(physics.RaycastAll(origin, glm::vec3(0.0f), 100.0f).empty());

		// A ray starting inside a collider does not hit it.
		std::optional<RaycastHit> fromInside = physics.Raycast(glm::vec3(0.0f, 5.0f, 0.0f), down, 100.0f);
		REQUIRE(fromInside);
		CHECK(fromInside->HitEntity != crate);
		CHECK(fromInside->Point.y == doctest::Approx(0.0f).epsilon(1.0e-4));
	}

	TEST_CASE("Raycasts skip triggers and an ignored entity unless asked")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity trigger = CreateStaticBox(scene, "Trigger", glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(1.0f));
		trigger.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		trigger.GetComponent<RigidBodyComponent>().IsTrigger = true;
		Entity player = CreateDynamicBox(scene, "Player", glm::vec3(0.0f, 6.0f, 0.0f));
		player.GetComponent<RigidBodyComponent>().GravityScale = 0.0f;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const glm::vec3 origin(0.0f, 10.0f, 0.0f);
		const glm::vec3 down(0.0f, -1.0f, 0.0f);

		std::optional<RaycastHit> hit = physics.Raycast(origin, down, 100.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == player);

		hit = physics.Raycast(origin, down, 100.0f, c_AllPhysicsLayers, player);
		REQUIRE(hit);
		CHECK(hit->HitEntity == ground);

		hit = physics.Raycast(origin, down, 100.0f, c_AllPhysicsLayers, player, true);
		REQUIRE(hit);
		CHECK(hit->HitEntity == trigger);
		CHECK(hit->Point.y == doctest::Approx(4.0f));

		CHECK(physics.RaycastAll(origin, down, 100.0f).size() == 2);
		CHECK(physics.RaycastAll(origin, down, 100.0f, c_AllPhysicsLayers, {}, true).size() == 3);
	}

	TEST_CASE("RaycastAll returns every entity hit, sorted by distance")
	{
		Scene scene;
		Entity farBox = CreateStaticBox(scene, "Far", glm::vec3(8.0f, 0.0f, 0.0f), glm::vec3(0.5f));
		Entity nearBox = CreateStaticBox(scene, "Near", glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.5f));
		Entity middle = scene.CreateEntity("Middle");
		middle.GetTransform().Translation = glm::vec3(5.0f, 0.0f, 0.0f);
		middle.AddComponent<SphereColliderComponent>();
		BoxColliderComponent& extra = middle.AddComponent<BoxColliderComponent>(); // Compound: still one hit for the entity
		extra.HalfExtents = glm::vec3(0.25f);
		extra.Offset = glm::vec3(0.75f, 0.0f, 0.0f);
		CreateStaticBox(scene, "Aside", glm::vec3(5.0f, 3.0f, 0.0f), glm::vec3(0.5f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const std::vector<RaycastHit> hits = physics.RaycastAll(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 20.0f);
		REQUIRE(hits.size() == 3);
		CHECK(hits[0].HitEntity == nearBox);
		CHECK(hits[1].HitEntity == middle);
		CHECK(hits[2].HitEntity == farBox);
		CHECK(hits[0].Distance == doctest::Approx(1.5f));
		CHECK(hits[1].Distance == doctest::Approx(4.5f));
		CHECK(hits[2].Distance == doctest::Approx(7.5f));
		for (const RaycastHit& hit : hits)
			CHECK(Math::IsNearlyEqual(hit.Normal, glm::vec3(-1.0f, 0.0f, 0.0f), 1.0e-4f));

		CHECK(physics.RaycastAll(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 6.0f).size() == 2);
		const std::vector<RaycastHit> withoutNear = physics.RaycastAll(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 20.0f, c_AllPhysicsLayers, nearBox);
		REQUIRE(withoutNear.size() == 2);
		CHECK(withoutNear[0].HitEntity == middle);
	}

	TEST_CASE("Overlap queries find the entities intersecting a sphere or box")
	{
		Scene scene;
		Entity center = CreateStaticBox(scene, "Center", glm::vec3(0.0f), glm::vec3(0.5f));
		Entity east = CreateStaticBox(scene, "East", glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.5f));
		Entity north = CreateStaticBox(scene, "North", glm::vec3(0.0f, 0.0f, -4.0f), glm::vec3(0.5f));
		Entity ball = CreateDynamicSphere(scene, "Ball", glm::vec3(0.0f, 2.0f, 0.0f), 0.5f);
		ball.GetComponent<RigidBodyComponent>().GravityScale = 0.0f;
		Entity trigger = CreateStaticBox(scene, "Trigger", glm::vec3(-2.0f, 0.0f, 0.0f), glm::vec3(0.5f));
		trigger.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		trigger.GetComponent<RigidBodyComponent>().IsTrigger = true;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const auto contains = [](const std::vector<Entity>& entities, Entity entity) { return std::find(entities.begin(), entities.end(), entity) != entities.end(); };

		std::vector<Entity> found = physics.OverlapSphere(glm::vec3(0.0f), 1.75f);
		CHECK(found.size() == 3);
		CHECK(contains(found, center));
		CHECK(contains(found, east));
		CHECK(contains(found, ball));
		CHECK_FALSE(contains(found, trigger)); // Excluded unless asked
		CHECK(physics.OverlapSphere(glm::vec3(0.0f), 1.75f, c_AllPhysicsLayers, true).size() == 4);
		CHECK(physics.OverlapSphere(glm::vec3(0.0f), 1.75f) == found); // Deterministic order

		// A long thin box along X, then rotated 90 degrees about Y to point along Z.
		found = physics.OverlapBox(glm::vec3(0.0f), glm::vec3(3.0f, 0.25f, 0.25f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		CHECK(found.size() == 2);
		CHECK(contains(found, center));
		CHECK(contains(found, east));
		found = physics.OverlapBox(glm::vec3(0.0f), glm::vec3(4.0f, 0.25f, 0.25f), glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
		CHECK(found.size() == 2);
		CHECK(contains(found, center));
		CHECK(contains(found, north));

		CHECK(physics.OverlapSphere(glm::vec3(0.0f, 50.0f, 0.0f), 1.0f).empty());
		CHECK(physics.OverlapSphere(glm::vec3(0.0f), 0.0f).empty());
		CHECK(physics.OverlapSphere(glm::vec3(0.0f), std::numeric_limits<float>::quiet_NaN()).empty());
		CHECK(physics.OverlapBox(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 1.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).empty());
		CHECK(physics.OverlapBox(glm::vec3(0.0f), glm::vec3(1.0f), glm::quat(0.0f, 0.0f, 0.0f, 0.0f)).empty());
	}

	TEST_CASE("Layer masks filter queries")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity platform = CreateStaticBox(scene, "Platform", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(1.0f, 0.25f, 1.0f));
		platform.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		platform.GetComponent<RigidBodyComponent>().Layer = 5;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const glm::vec3 origin(0.0f, 10.0f, 0.0f);
		const glm::vec3 down(0.0f, -1.0f, 0.0f);

		std::optional<RaycastHit> hit = physics.Raycast(origin, down, 100.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == platform);

		hit = physics.Raycast(origin, down, 100.0f, ~ST_BIT(5));
		REQUIRE(hit);
		CHECK(hit->HitEntity == ground);

		hit = physics.Raycast(origin, down, 100.0f, ST_BIT(5));
		REQUIRE(hit);
		CHECK(hit->HitEntity == platform);

		CHECK_FALSE(physics.Raycast(origin, down, 100.0f, ST_BIT(7)));
		CHECK_FALSE(physics.Raycast(origin, down, 100.0f, 0u));
		CHECK(physics.OverlapSphere(glm::vec3(0.0f, 1.0f, 0.0f), 2.0f, ST_BIT(5)) == std::vector<Entity> { platform });
		CHECK(physics.OverlapSphere(glm::vec3(0.0f, 1.0f, 0.0f), 2.0f, ST_BIT(0)) == std::vector<Entity> { ground });
	}

	TEST_CASE("Queries see component changes made earlier in the frame")
	{
		Scene scene;
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const glm::vec3 origin(0.0f, 10.0f, 0.0f);
		const glm::vec3 down(0.0f, -1.0f, 0.0f);
		CHECK_FALSE(physics.Raycast(origin, down, 100.0f));

		Entity ground = CreateGround(scene);
		std::optional<RaycastHit> hit = physics.Raycast(origin, down, 100.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == ground);

		scene.DestroyEntity(ground);
		CHECK_FALSE(physics.Raycast(origin, down, 100.0f));
	}
}
