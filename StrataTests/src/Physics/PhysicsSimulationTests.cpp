#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"

#include <glm/gtc/matrix_transform.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	struct BodyPose
	{
		glm::vec3 Translation;
		glm::quat Rotation;
	};

	// A pile of mixed bodies dropped onto the ground; returns every body's final pose in hierarchy order.
	std::vector<BodyPose> SimulatePile()
	{
		Scene scene("Pile");
		CreateGround(scene);
		for (int index = 0; index < 24; index++)
		{
			const glm::vec3 position(static_cast<float>(index % 3) * 0.6f - 0.6f, 1.0f + static_cast<float>(index) * 0.7f, static_cast<float>((index / 3) % 2) * 0.4f);
			Entity entity = scene.CreateEntity("Body");
			entity.GetTransform().Translation = position;
			entity.GetTransform().Rotation = glm::angleAxis(0.3f * static_cast<float>(index), glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f)));
			RigidBodyComponent& rigidBody = entity.AddComponent<RigidBodyComponent>();
			rigidBody.Mass = 1.0f + static_cast<float>(index % 4);
			switch (index % 3)
			{
				case 0: entity.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.3f, 0.2f, 0.25f); break;
				case 1: entity.AddComponent<SphereColliderComponent>().Radius = 0.3f; break;
				case 2: entity.AddComponent<CapsuleColliderComponent>().HalfHeight = 0.2f; break;
			}
		}

		scene.OnRuntimeStart();
		StepScene(scene, 150);

		std::vector<BodyPose> poses;
		for (Entity entity : scene.GetEntitiesInHierarchyOrder())
		{
			const TransformComponent& transform = entity.GetComponent<TransformComponent>();
			poses.push_back({ transform.Translation, transform.Rotation });
		}
		return poses;
	}

	bool AreIdentical(const std::vector<BodyPose>& a, const std::vector<BodyPose>& b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t index = 0; index < a.size(); index++)
		{
			if (std::memcmp(&a[index].Translation, &b[index].Translation, sizeof(glm::vec3)) != 0 || std::memcmp(&a[index].Rotation, &b[index].Rotation, sizeof(glm::quat)) != 0)
				return false;
		}
		return true;
	}

	// Drops a sphere onto the ground and returns the highest point it reaches after its first bounce (0 if it never moves
	// upwards).
	float MeasureBounceHeight(float restitution)
	{
		Scene scene;
		CreateGround(scene);
		Entity ball = CreateDynamicSphere(scene, "Ball", glm::vec3(0.0f, 4.5f, 0.0f), 0.5f);
		RigidBodyComponent& rigidBody = ball.GetComponent<RigidBodyComponent>();
		rigidBody.Restitution = restitution;
		rigidBody.LinearDamping = 0.0f;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		bool bounced = false;
		float peak = 0.0f;
		for (int step = 0; step < 240; step++)
		{
			StepScene(scene, 1);
			if (physics.GetLinearVelocity(ball).y > 0.5f)
				bounced = true;
			if (bounced)
				peak = std::max(peak, GetWorldPosition(scene, ball).y);
		}
		return peak;
	}

}

TEST_SUITE("Physics.Simulation")
{
	TEST_CASE("A dynamic box comes to rest on a static ground box")
	{
		Scene scene;
		CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 3.0f, 0.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(physics.HasBody(box));

		StepScene(scene, 180);
		const glm::vec3 position = GetWorldPosition(scene, box);
		CHECK(std::abs(position.y - 0.5f) < 0.03f);
		CHECK(std::abs(position.x) < 1.0e-3f);
		CHECK(std::abs(position.z) < 1.0e-3f);
		CHECK(Math::IsNearlyEqual(GetWorldRotation(scene, box), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0e-4f));
		CHECK(glm::length(physics.GetLinearVelocity(box)) < 0.01f);
		CHECK(physics.IsSleeping(box));

		const PhysicsStats stats = physics.GetStats();
		CHECK(stats.BodyCount == 2);
		CHECK(stats.StaticBodyCount == 1);
		CHECK(stats.DynamicBodyCount == 1);
		CHECK(stats.ActiveBodyCount == 0);
		CHECK(stats.ContactPairCount == 1);
		CHECK(stats.StepCount == 180);
		CHECK(stats.PendingBodyCount == 0);
	}

	TEST_CASE("Restitution makes a sphere bounce")
	{
		// Falling 4 m hits the ground at ~8.9 m/s; with restitution 0.8 the ball rebounds to ~2.6 m above the contact.
		const float bouncyPeak = MeasureBounceHeight(0.8f);
		CHECK(bouncyPeak > 2.5f);
		CHECK(bouncyPeak < 4.5f); // Never gains energy

		const float deadPeak = MeasureBounceHeight(0.0f);
		CHECK(deadPeak < 0.6f);
	}

	TEST_CASE("Gravity scale scales the gravity a body receives")
	{
		Scene scene;
		Entity floating = CreateDynamicBox(scene, "Floating", glm::vec3(0.0f, 5.0f, 0.0f));
		Entity falling = CreateDynamicBox(scene, "Falling", glm::vec3(5.0f, 5.0f, 0.0f));
		Entity half = CreateDynamicBox(scene, "Half", glm::vec3(10.0f, 5.0f, 0.0f));
		floating.GetComponent<RigidBodyComponent>().GravityScale = 0.0f;
		half.GetComponent<RigidBodyComponent>().GravityScale = 0.5f;
		for (Entity entity : { floating, falling, half })
			entity.GetComponent<RigidBodyComponent>().LinearDamping = 0.0f;

		scene.OnRuntimeStart();
		StepScene(scene, 60);

		CHECK(GetWorldPosition(scene, floating).y == doctest::Approx(5.0f));
		const float fallDistance = 5.0f - GetWorldPosition(scene, falling).y;
		const float halfDistance = 5.0f - GetWorldPosition(scene, half).y;
		CHECK(fallDistance == doctest::Approx(0.5f * 9.81f).epsilon(0.02)); // Free fall for one second
		CHECK(halfDistance == doctest::Approx(0.5f * fallDistance).epsilon(0.01));

		// Turning gravity off at runtime keeps the current velocity (no damping) and updates the component.
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK(physics.SetGravityScale(falling, 0.0f));
		CHECK(falling.GetComponent<RigidBodyComponent>().GravityScale == 0.0f);
		const glm::vec3 velocity = physics.GetLinearVelocity(falling);
		StepScene(scene, 10);
		CHECK(Math::IsNearlyEqual(physics.GetLinearVelocity(falling), velocity, 1.0e-4f));
		CHECK_FALSE(physics.SetGravityScale(falling, std::numeric_limits<float>::quiet_NaN()));
	}

	TEST_CASE("Mass and damping shape the response to impulses and forces")
	{
		Scene scene;
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		Entity light = CreateDynamicBox(scene, "Light", glm::vec3(0.0f, 0.0f, 0.0f));
		Entity heavy = CreateDynamicBox(scene, "Heavy", glm::vec3(0.0f, 5.0f, 0.0f));
		Entity damped = CreateDynamicBox(scene, "Damped", glm::vec3(0.0f, 10.0f, 0.0f));
		Entity pushed = CreateDynamicBox(scene, "Pushed", glm::vec3(0.0f, 15.0f, 0.0f));
		heavy.GetComponent<RigidBodyComponent>().Mass = 4.0f;
		pushed.GetComponent<RigidBodyComponent>().Mass = 2.0f;
		for (Entity entity : { light, heavy, pushed })
			entity.GetComponent<RigidBodyComponent>().LinearDamping = 0.0f;
		damped.GetComponent<RigidBodyComponent>().LinearDamping = 2.0f;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);

		// Impulses change the velocity immediately by impulse / mass.
		CHECK(physics.AddImpulse(light, glm::vec3(4.0f, 0.0f, 0.0f)));
		CHECK(physics.AddImpulse(heavy, glm::vec3(4.0f, 0.0f, 0.0f)));
		CHECK(physics.GetLinearVelocity(light).x == doctest::Approx(4.0f));
		CHECK(physics.GetLinearVelocity(heavy).x == doctest::Approx(1.0f));

		// A force acts over one step: dv = F / m * dt.
		CHECK(physics.AddForce(pushed, glm::vec3(12.0f, 0.0f, 0.0f)));
		CHECK(physics.SetLinearVelocity(damped, glm::vec3(4.0f, 0.0f, 0.0f)));
		StepScene(scene, 1);
		CHECK(physics.GetLinearVelocity(pushed).x == doctest::Approx(12.0f / 2.0f / 60.0f));

		StepScene(scene, 59);
		CHECK(physics.GetLinearVelocity(light).x == doctest::Approx(4.0f));
		CHECK(GetWorldPosition(scene, light).x == doctest::Approx(4.0f).epsilon(0.02));
		const float dampedSpeed = physics.GetLinearVelocity(damped).x;
		CHECK(dampedSpeed > 0.0f);
		CHECK(dampedSpeed < 1.0f); // ~4 * (1 - 2/60)^60 = 0.52
		CHECK(GetWorldPosition(scene, damped).x < 0.6f * GetWorldPosition(scene, light).x);

		// Torque and off-center impulses spin bodies.
		CHECK(physics.AddTorque(light, glm::vec3(0.0f, 50.0f, 0.0f)));
		StepScene(scene, 1);
		CHECK(physics.GetAngularVelocity(light).y > 0.1f);
		CHECK(physics.AddImpulseAtPosition(heavy, glm::vec3(0.0f, 0.0f, 1.0f), GetWorldPosition(scene, heavy) + glm::vec3(0.5f, 0.0f, 0.0f)));
		CHECK(glm::length(physics.GetAngularVelocity(heavy)) > 0.1f);
		CHECK(physics.AddForceAtPosition(pushed, glm::vec3(0.0f, 0.0f, 100.0f), GetWorldPosition(scene, pushed) + glm::vec3(0.5f, 0.0f, 0.0f)));
		StepScene(scene, 1);
		CHECK(glm::length(physics.GetAngularVelocity(pushed)) > 0.1f);

		// An angular impulse changes the angular velocity immediately by the inverse inertia times the impulse: for a unit cube
		// of mass 1 the inertia is m * (1 + 1) / 12 = 1/6 around every axis.
		CHECK(physics.SetAngularVelocity(damped, glm::vec3(0.0f)));
		CHECK(physics.AddAngularImpulse(damped, glm::vec3(0.0f, 0.5f, 0.0f)));
		CHECK(physics.GetAngularVelocity(damped).y == doctest::Approx(3.0f).epsilon(0.01));
		CHECK_FALSE(physics.AddAngularImpulse(damped, glm::vec3(std::numeric_limits<float>::quiet_NaN())));
	}

	TEST_CASE("Locked rotation axes stay fixed")
	{
		Scene scene;
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		Entity free = CreateDynamicBox(scene, "Free", glm::vec3(0.0f, 0.0f, 0.0f));
		Entity locked = CreateDynamicBox(scene, "Locked", glm::vec3(5.0f, 0.0f, 0.0f));
		Entity lockedY = CreateDynamicBox(scene, "LockedY", glm::vec3(10.0f, 0.0f, 0.0f));
		RigidBodyComponent& lockedBody = locked.GetComponent<RigidBodyComponent>();
		lockedBody.LockRotationX = lockedBody.LockRotationY = lockedBody.LockRotationZ = true;
		lockedY.GetComponent<RigidBodyComponent>().LockRotationY = true;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		for (Entity entity : { free, locked })
			CHECK(physics.AddImpulseAtPosition(entity, glm::vec3(0.0f, 1.0f, 1.0f), GetWorldPosition(scene, entity) + glm::vec3(0.5f, 0.5f, 0.0f)));
		CHECK(physics.SetAngularVelocity(lockedY, glm::vec3(2.0f, 2.0f, 0.0f)));
		StepScene(scene, 30);

		const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
		CHECK_FALSE(Math::IsNearlyEqual(GetWorldRotation(scene, free), identity, 1.0e-3f));
		CHECK(Math::IsNearlyEqual(GetWorldRotation(scene, locked), identity, 1.0e-6f));
		CHECK(glm::length(physics.GetAngularVelocity(locked)) < 1.0e-6f);
		CHECK(glm::length(physics.GetLinearVelocity(locked)) > 0.1f); // Translation is still free

		// Only the X component of the requested spin survives: the body turns about X alone.
		CHECK(std::abs(physics.GetAngularVelocity(lockedY).y) < 1.0e-6f);
		const glm::quat rotation = GetWorldRotation(scene, lockedY);
		CHECK(std::abs(rotation.x) > 0.1f);
		CHECK(std::abs(rotation.y) < 1.0e-4f);
		CHECK(std::abs(rotation.z) < 1.0e-4f);
	}

	TEST_CASE("A kinematic body moved by its transform pushes dynamic bodies")
	{
		Scene scene;
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		Entity pusher = CreateDynamicBox(scene, "Pusher", glm::vec3(-3.0f, 0.0f, 0.0f));
		pusher.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
		Entity crate = CreateDynamicBox(scene, "Crate", glm::vec3(0.0f, 0.0f, 0.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const float speed = 3.0f;
		for (int frame = 0; frame < 90; frame++)
		{
			pusher.GetTransform().Translation.x += speed / 60.0f;
			StepScene(scene, 1);
		}

		CHECK(physics.GetLinearVelocity(pusher).x == doctest::Approx(speed).epsilon(0.01));
		CHECK(GetWorldPosition(scene, pusher).x == doctest::Approx(1.5f).epsilon(1.0e-3));
		CHECK(GetWorldPosition(scene, crate).x > GetWorldPosition(scene, pusher).x + 0.95f); // Pushed ahead, not penetrated
		CHECK(physics.GetLinearVelocity(crate).x >= speed * 0.95f);

		// Once its entity stops moving, the kinematic body stops too.
		StepScene(scene, 2);
		CHECK(glm::length(physics.GetLinearVelocity(pusher)) < 1.0e-5f);
		CHECK(GetWorldPosition(scene, pusher).x == doctest::Approx(1.5f).epsilon(1.0e-3));

		// Velocities of kinematic bodies follow their transform; setting them directly is rejected.
		CHECK_FALSE(physics.SetLinearVelocity(pusher, glm::vec3(1.0f, 0.0f, 0.0f)));
		CHECK_FALSE(physics.AddImpulse(pusher, glm::vec3(1.0f, 0.0f, 0.0f)));
	}

	TEST_CASE("Continuous collision keeps fast projectiles from tunneling through thin walls")
	{
		// At 240 m/s the projectile advances 4 m per step: it is at x = 4 before and x = 8 after the wall at x = 6.
		const auto fire = [](bool continuous)
		{
			Scene scene;
			scene.GetSettings().Gravity = glm::vec3(0.0f);
			CreateStaticBox(scene, "Wall", glm::vec3(6.0f, 0.0f, 0.0f), glm::vec3(0.05f, 5.0f, 5.0f));
			Entity projectile = CreateDynamicSphere(scene, "Projectile", glm::vec3(0.0f, 0.0f, 0.0f), 0.1f);
			RigidBodyComponent& rigidBody = projectile.GetComponent<RigidBodyComponent>();
			rigidBody.ContinuousCollision = continuous;
			rigidBody.LinearDamping = 0.0f;

			scene.OnRuntimeStart();
			GetPhysics(scene).SetLinearVelocity(projectile, glm::vec3(240.0f, 0.0f, 0.0f));
			StepScene(scene, 5);
			return GetWorldPosition(scene, projectile).x;
		};

		CHECK(fire(false) > 6.05f); // Discrete collision detection misses the wall
		CHECK(fire(true) < 5.95f);  // The linear cast stops it in front of the wall
	}

	TEST_CASE("Collision sub-steps catch fast bodies that one collision step misses")
	{
		// At 240 m/s a projectile advances 4 m per step: one collision step per step tests it at x = 4 and x = 8, both clear
		// of the wall between x = 5.7 and 6.9; four collision steps test it every meter, and at x = 6 it is in the wall.
		const auto fire = [](uint32_t collisionSteps)
		{
			PhysicsSettings settings;
			settings.CollisionSteps = collisionSteps;
			ScopedPhysicsSettings scopedSettings(settings);

			Scene scene;
			scene.GetSettings().Gravity = glm::vec3(0.0f);
			CreateStaticBox(scene, "Wall", glm::vec3(6.3f, 0.0f, 0.0f), glm::vec3(0.6f, 5.0f, 5.0f));
			Entity projectile = CreateDynamicSphere(scene, "Projectile", glm::vec3(0.0f), 0.1f);
			projectile.GetComponent<RigidBodyComponent>().LinearDamping = 0.0f;

			scene.OnRuntimeStart();
			PhysicsSystem& physics = GetPhysics(scene);
			REQUIRE(physics.GetWorld() != nullptr);
			CHECK(physics.GetWorld()->GetSettings().CollisionSteps == collisionSteps);
			CHECK(physics.SetLinearVelocity(projectile, glm::vec3(240.0f, 0.0f, 0.0f)));
			StepScene(scene, 5);
			CHECK(physics.GetStats().StepCount == 5); // Collision steps subdivide a step; they are not steps of their own
			return GetWorldPosition(scene, projectile).x;
		};

		CHECK(fire(1) > 6.9f);
		CHECK(fire(4) < 5.7f);
	}

	TEST_CASE("Parented dynamic bodies write their world transform through the hierarchy")
	{
		Scene scene;
		CreateGround(scene);
		Entity parent = scene.CreateEntity("Parent");
		TransformComponent& parentTransform = parent.GetTransform();
		parentTransform.Translation = glm::vec3(10.0f, 0.0f, 0.0f);
		parentTransform.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		parentTransform.Scale = glm::vec3(2.0f);
		const glm::mat4 parentWorld = scene.GetWorldTransform(parent);

		Entity child = scene.CreateChildEntity(parent, "Child");
		child.GetTransform().Translation = glm::vec3(0.0f, 2.0f, 0.0f); // World (10, 4, 0)
		child.AddComponent<RigidBodyComponent>();
		child.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.25f); // World half extents 0.5

		scene.OnRuntimeStart();
		StepScene(scene, 180);

		const glm::vec3 worldPosition = GetWorldPosition(scene, child);
		CHECK(worldPosition.x == doctest::Approx(10.0f).epsilon(1.0e-3));
		CHECK(std::abs(worldPosition.y - 0.5f) < 0.03f);
		CHECK(std::abs(worldPosition.z) < 1.0e-3f);

		// The parent is untouched and the child's local transform expresses the simulated pose under it.
		CHECK(scene.GetWorldTransform(parent) == parentWorld);
		const TransformComponent& childTransform = child.GetComponent<TransformComponent>();
		CHECK(std::abs(childTransform.Translation.y - 0.25f) < 0.015f);
		CHECK(Math::IsNearlyEqual(childTransform.Scale, glm::vec3(1.0f), 1.0e-4f));
		CHECK(Math::IsNearlyEqual(childTransform.Rotation, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0e-4f));
	}

	TEST_CASE("Mirrored dynamic bodies keep their authored scale")
	{
		Scene scene;
		CreateGround(scene);
		// Mirrored on Y and spinning about Y.
		Entity mirrored = CreateDynamicBox(scene, "Mirrored", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(1.0f, 0.5f, 0.5f));
		mirrored.GetTransform().Scale = glm::vec3(1.0f, -1.0f, 1.0f);
		// Mirrored by its parent instead, with a scale of its own.
		Entity parent = scene.CreateEntity("MirroringParent");
		parent.GetTransform().Translation = glm::vec3(10.0f, 0.0f, 0.0f);
		parent.GetTransform().Scale = glm::vec3(-1.0f, 1.0f, 1.0f);
		Entity child = CreateDynamicBox(scene, "Child", glm::vec3(0.0f, 2.0f, 0.0f));
		child.GetTransform().Scale = glm::vec3(2.0f, 1.0f, 1.0f);
		REQUIRE(scene.SetParent(child, parent, false));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK(physics.SetAngularVelocity(mirrored, glm::vec3(0.0f, 1.0f, 0.0f)));
		StepScene(scene, 1);

		// Only the translation and rotation of the components change, as if the entity had been moved by hand.
		const TransformComponent& transform = mirrored.GetComponent<TransformComponent>();
		CHECK(Math::IsNearlyEqual(transform.Scale, glm::vec3(1.0f, -1.0f, 1.0f), 1.0e-5f));
		CHECK(Math::IsNearlyEqual(transform.Rotation, glm::angleAxis(1.0f / 60.0f, glm::vec3(0.0f, 1.0f, 0.0f)), 1.0e-4f));
		CHECK(glm::determinant(glm::mat3(scene.GetWorldTransform(mirrored))) < 0.0f);

		StepScene(scene, 150);
		CHECK(Math::IsNearlyEqual(transform.Scale, glm::vec3(1.0f, -1.0f, 1.0f), 1.0e-5f));
		CHECK(std::abs(transform.Translation.y - 0.5f) < 0.03f);
		CHECK(std::abs(transform.Rotation.x) < 1.0e-3f); // Still a rotation about Y only
		CHECK(std::abs(transform.Rotation.z) < 1.0e-3f);
		std::optional<RaycastHit> top = physics.Raycast(glm::vec3(transform.Translation.x, 5.0f, transform.Translation.z), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(top);
		CHECK(top->HitEntity == mirrored);
		CHECK(top->Point.y == doctest::Approx(1.0f).epsilon(0.03));

		const TransformComponent& childTransform = child.GetComponent<TransformComponent>();
		CHECK(Math::IsNearlyEqual(childTransform.Scale, glm::vec3(2.0f, 1.0f, 1.0f), 1.0e-5f));
		CHECK(Math::IsNearlyEqual(childTransform.Rotation, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0e-4f));
		CHECK(std::abs(GetWorldPosition(scene, child).y - 0.5f) < 0.03f);

		// Teleporting keeps the authored scale as well.
		CHECK(physics.Teleport(mirrored, glm::vec3(5.0f, 3.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
		CHECK(Math::IsNearlyEqual(transform.Scale, glm::vec3(1.0f, -1.0f, 1.0f), 1.0e-5f));
		CHECK(Math::IsNearlyEqual(GetWorldPosition(scene, mirrored), glm::vec3(5.0f, 3.0f, 0.0f), 1.0e-4f));
	}

	TEST_CASE("Scale tweens of small mirrored bodies are kept and rebuild their shape")
	{
		Scene scene;
		CreateGround(scene);
		// A centimeter-scale prop mirrored on Y: its box collider is 1 m across in the world.
		Entity prop = CreateDynamicBox(scene, "Prop", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(50.0f));
		prop.GetTransform().Scale = glm::vec3(0.01f, -0.01f, 0.01f);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 30);

		// Grown by half a percent every frame through the signaled path: every edit survives the write-back.
		for (int frame = 0; frame < 20; frame++)
		{
			prop.GetTransform().Scale += glm::vec3(5.0e-5f, -5.0e-5f, 5.0e-5f);
			const glm::vec3 expected = prop.GetTransform().Scale;
			prop.MarkModified<TransformComponent>();
			StepScene(scene, 1);
			CHECK(Math::IsNearlyEqual(prop.GetComponent<TransformComponent>().Scale, expected, 1.0e-7f));
		}
		const TransformComponent& transform = prop.GetComponent<TransformComponent>();
		CHECK(transform.Scale.x == doctest::Approx(0.011f));
		CHECK(transform.Scale.y == doctest::Approx(-0.011f)); // Still mirrored on Y
		CHECK(Math::IsNearlyEqual(transform.Rotation, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 1.0e-3f));

		// The collider grew with it: 0.011 * 50 = 0.55 half extent.
		StepScene(scene, 60);
		CHECK(std::abs(GetWorldPosition(scene, prop).y - 0.55f) < 0.03f);
		std::optional<RaycastHit> top = physics.Raycast(GetWorldPosition(scene, prop) + glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(top);
		CHECK(top->HitEntity == prop);
		CHECK(top->Point.y == doctest::Approx(GetWorldPosition(scene, prop).y + 0.55f).epsilon(1.0e-3));
	}

	TEST_CASE("Several colliders on one entity form a compound shape")
	{
		Scene scene;
		CreateGround(scene);

		Entity statue = scene.CreateEntity("Statue");
		statue.GetTransform().Translation = glm::vec3(0.0f, 5.0f, 0.0f);
		BoxColliderComponent& box = statue.AddComponent<BoxColliderComponent>();
		box.Offset = glm::vec3(-2.0f, 0.0f, 0.0f);
		SphereColliderComponent& sphere = statue.AddComponent<SphereColliderComponent>();
		sphere.Offset = glm::vec3(2.0f, 0.0f, 0.0f);

		// A plate with a ball on top.
		Entity lamp = scene.CreateEntity("Lamp");
		lamp.GetTransform().Translation = glm::vec3(10.0f, 2.0f, 0.0f);
		lamp.AddComponent<RigidBodyComponent>();
		lamp.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(1.0f, 0.25f, 1.0f);
		SphereColliderComponent& ball = lamp.AddComponent<SphereColliderComponent>();
		ball.Radius = 0.25f;
		ball.Offset = glm::vec3(0.0f, 0.5f, 0.0f);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const glm::vec3 down(0.0f, -1.0f, 0.0f);
		std::optional<RaycastHit> boxHit = physics.Raycast(glm::vec3(-2.0f, 10.0f, 0.0f), down, 20.0f);
		std::optional<RaycastHit> sphereHit = physics.Raycast(glm::vec3(2.0f, 10.0f, 0.0f), down, 20.0f);
		std::optional<RaycastHit> gapHit = physics.Raycast(glm::vec3(0.0f, 10.0f, 0.0f), down, 20.0f);
		REQUIRE(boxHit);
		REQUIRE(sphereHit);
		REQUIRE(gapHit);
		CHECK(boxHit->HitEntity == statue);
		CHECK(boxHit->Point.y == doctest::Approx(5.5f).epsilon(1.0e-3));
		CHECK(sphereHit->HitEntity == statue);
		CHECK(sphereHit->Point.y == doctest::Approx(5.5f).epsilon(1.0e-3));
		CHECK(gapHit->HitEntity != statue); // Between the parts the ray reaches the ground

		StepScene(scene, 180);
		const glm::vec3 lampPosition = GetWorldPosition(scene, lamp);
		CHECK(std::abs(lampPosition.y - 0.25f) < 0.03f);
		std::optional<RaycastHit> ballHit = physics.Raycast(lampPosition + glm::vec3(0.0f, 5.0f, 0.0f), down, 10.0f);
		std::optional<RaycastHit> plateHit = physics.Raycast(lampPosition + glm::vec3(0.8f, 5.0f, 0.0f), down, 10.0f);
		REQUIRE(ballHit);
		REQUIRE(plateHit);
		CHECK(ballHit->HitEntity == lamp);
		CHECK(ballHit->Point.y == doctest::Approx(lampPosition.y + 0.75f).epsilon(1.0e-3));
		CHECK(plateHit->HitEntity == lamp);
		CHECK(plateHit->Point.y == doctest::Approx(lampPosition.y + 0.25f).epsilon(1.0e-3));
	}

	TEST_CASE("Entity scale is applied to colliders")
	{
		Scene scene;
		CreateGround(scene);

		// Box half extents 0.5 scaled by (4, 1, 2): world half extents (2, 0.5, 1).
		Entity slab = scene.CreateEntity("Slab");
		slab.GetTransform().Translation = glm::vec3(0.0f, 10.0f, 0.0f);
		slab.GetTransform().Scale = glm::vec3(4.0f, 1.0f, 2.0f);
		slab.AddComponent<BoxColliderComponent>();

		// Spheres scale by their largest axis, capsules' radii by max(|X|, |Z|) and their height by |Y|.
		Entity ellipsoid = scene.CreateEntity("Ellipsoid");
		ellipsoid.GetTransform().Translation = glm::vec3(20.0f, 10.0f, 0.0f);
		ellipsoid.GetTransform().Scale = glm::vec3(1.0f, 3.0f, 1.0f);
		ellipsoid.AddComponent<SphereColliderComponent>();
		Entity capsule = scene.CreateEntity("Capsule");
		capsule.GetTransform().Translation = glm::vec3(30.0f, 10.0f, 0.0f);
		capsule.GetTransform().Scale = glm::vec3(2.0f, 0.5f, 1.0f);
		CapsuleColliderComponent& capsuleCollider = capsule.AddComponent<CapsuleColliderComponent>();
		capsuleCollider.HalfHeight = 1.0f;

		Entity flat = CreateDynamicBox(scene, "Flat", glm::vec3(-10.0f, 2.0f, 0.0f));
		flat.GetTransform().Scale = glm::vec3(1.0f, 0.5f, 1.0f);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const glm::vec3 down(0.0f, -1.0f, 0.0f);
		const auto hitsSlab = [&](float x, float z)
		{
			std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(x, 20.0f, z), down, 15.0f);
			return hit && hit->HitEntity == slab && std::abs(hit->Point.y - 10.5f) < 1.0e-3f;
		};
		CHECK(hitsSlab(1.9f, 0.9f));
		CHECK(hitsSlab(-1.9f, -0.9f));
		CHECK_FALSE(hitsSlab(2.1f, 0.0f));
		CHECK_FALSE(hitsSlab(0.0f, 1.1f));

		std::optional<RaycastHit> sideHit = physics.Raycast(glm::vec3(-20.0f, 10.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 30.0f);
		REQUIRE(sideHit);
		CHECK(sideHit->HitEntity == slab);
		CHECK(sideHit->Point.x == doctest::Approx(-2.0f).epsilon(1.0e-3));
		CHECK(Math::IsNearlyEqual(sideHit->Normal, glm::vec3(-1.0f, 0.0f, 0.0f), 1.0e-4f));

		std::optional<RaycastHit> ellipsoidHit = physics.Raycast(glm::vec3(20.0f, 20.0f, 0.0f), down, 15.0f);
		REQUIRE(ellipsoidHit);
		CHECK(ellipsoidHit->Point.y == doctest::Approx(11.5f).epsilon(1.0e-3));
		std::optional<RaycastHit> capsuleTop = physics.Raycast(glm::vec3(30.0f, 20.0f, 0.0f), down, 15.0f);
		REQUIRE(capsuleTop);
		CHECK(capsuleTop->HitEntity == capsule);
		CHECK(capsuleTop->Point.y == doctest::Approx(11.5f).epsilon(1.0e-3)); // Half height 0.5 + radius 1
		std::optional<RaycastHit> capsuleFlank = physics.Raycast(glm::vec3(25.0f, 10.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), 10.0f);
		REQUIRE(capsuleFlank);
		CHECK(capsuleFlank->HitEntity == capsule);
		CHECK(capsuleFlank->Point.x == doctest::Approx(29.0f).epsilon(1.0e-3));

		StepScene(scene, 120);
		CHECK(std::abs(GetWorldPosition(scene, flat).y - 0.25f) < 0.03f);
		CHECK(Math::IsNearlyEqual(flat.GetComponent<TransformComponent>().Scale, glm::vec3(1.0f, 0.5f, 1.0f), 1.0e-4f));

		// Changing the scale at runtime rebuilds the shape (static bodies follow signaled transform changes).
		slab.GetTransform().Scale = glm::vec3(2.0f, 1.0f, 2.0f);
		slab.MarkModified<TransformComponent>();
		StepScene(scene, 1);
		CHECK_FALSE(hitsSlab(1.9f, 0.0f));
		CHECK(hitsSlab(0.9f, 0.0f));
	}

	TEST_CASE("Static bodies follow their transform and wake bodies resting on them")
	{
		Scene scene;
		Entity platform = CreateStaticBox(scene, "Platform", glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(3.0f, 0.5f, 3.0f));
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 1.0f, 0.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 120);
		REQUIRE(physics.IsSleeping(box));

		// Like dragging the platform in the editor while simulating (edits go through ComponentAccess, which signals the
		// change; static bodies are not polled every step).
		platform.GetTransform().Translation.y = -2.5f;
		platform.MarkModified<TransformComponent>();
		StepScene(scene, 120);
		CHECK(std::abs(GetWorldPosition(scene, box).y + 1.5f) < 0.03f);

		// Moving an ancestor moves static descendants as well.
		Entity holder = scene.CreateEntity("Holder");
		Entity shelf = CreateStaticBox(scene, "Shelf", glm::vec3(20.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.1f, 1.0f));
		scene.SetParent(shelf, holder);
		StepScene(scene, 1);
		holder.GetTransform().Translation.y = 5.0f;
		holder.MarkModified<TransformComponent>();
		StepScene(scene, 1);
		std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(20.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == shelf);
		CHECK(hit->Point.y == doctest::Approx(5.1f).epsilon(1.0e-3));
	}

	TEST_CASE("Sleeping and static bodies follow Scene::SetWorldTransform")
	{
		Scene scene;
		CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f));
		Entity post = CreateStaticBox(scene, "Post", glm::vec3(5.0f, 1.0f, 0.0f), glm::vec3(0.25f, 1.0f, 0.25f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 90);
		REQUIRE(physics.IsSleeping(box));

		// Neither body is polled, so the moves reach physics through the signal SetWorldTransform emits.
		CHECK(scene.SetWorldTransform(box, glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.5f, 0.0f))));
		CHECK(scene.SetWorldTransform(post, glm::translate(glm::mat4(1.0f), glm::vec3(-5.0f, 1.0f, 0.0f))));
		StepScene(scene, 1);
		const glm::vec3 down(0.0f, -1.0f, 0.0f);
		std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(10.0f, 5.0f, 0.0f), down, 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == box);
		hit = physics.Raycast(glm::vec3(-5.0f, 5.0f, 0.0f), down, 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == post);
		hit = physics.Raycast(glm::vec3(5.0f, 5.0f, 0.0f), down, 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity != post);
	}

	TEST_CASE("Gravity follows the scene settings and wakes sleeping bodies")
	{
		Scene scene;
		CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 90);
		REQUIRE(physics.IsSleeping(box));
		CHECK(physics.GetGravity() == glm::vec3(0.0f, -9.81f, 0.0f));

		CHECK(physics.SetGravity(glm::vec3(0.0f, 5.0f, 0.0f)));
		CHECK(scene.GetSettings().Gravity == glm::vec3(0.0f, 5.0f, 0.0f));
		StepScene(scene, 30);
		CHECK(GetWorldPosition(scene, box).y > 1.0f);

		// Editing the scene settings directly takes effect at the next step.
		scene.GetSettings().Gravity = glm::vec3(0.0f, -20.0f, 0.0f);
		StepScene(scene, 1);
		CHECK(physics.GetGravity() == glm::vec3(0.0f, -20.0f, 0.0f));

		CHECK_FALSE(physics.SetGravity(glm::vec3(std::numeric_limits<float>::infinity(), 0.0f, 0.0f)));
		scene.GetSettings().Gravity = glm::vec3(std::numeric_limits<float>::quiet_NaN());
		StepScene(scene, 1);
		CHECK(physics.GetGravity() == glm::vec3(0.0f, -20.0f, 0.0f));
		CHECK(IsFinite(GetWorldPosition(scene, box)));
	}

	TEST_CASE("Simulate mode creates the physics system")
	{
		Scene scene;
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 5.0f, 0.0f));
		scene.OnRuntimeStart(SceneRuntimeMode::Simulate);
		REQUIRE(scene.GetSystem<PhysicsSystem>() != nullptr);
		StepScene(scene, 10);
		CHECK(GetWorldPosition(scene, box).y < 5.0f);
		scene.OnRuntimeStop();
		CHECK(scene.GetSystem<PhysicsSystem>() == nullptr);
	}

	TEST_CASE("Identical runs produce identical results, with or without worker threads")
	{
		const std::vector<BodyPose> first = SimulatePile();
		const std::vector<BodyPose> second = SimulatePile();
		CHECK(first.size() == 25);
		CHECK(AreIdentical(first, second));

		// Bodies settled into a pile instead of falling through each other or the ground.
		for (size_t index = 1; index < first.size(); index++)
		{
			CHECK(IsFinite(first[index].Translation));
			CHECK(first[index].Translation.y > 0.15f);
		}

		// Jolt's jobs run on Strata's worker pool when it is initialized; the results must not change.
		ScopedJobSystem jobSystem(3);
		const std::vector<BodyPose> threaded = SimulatePile();
		CHECK(AreIdentical(first, threaded));
	}

	TEST_CASE("Moving a body wakes the bodies resting on it")
	{
		enum class Move
		{
			Teleport,
			TransformEdit,
			Kinematic
		};

		// Two stacked boxes fall asleep; then the bottom one is moved away from under the top one, which must fall.
		const auto topHeightAfterMove = [](Move move)
		{
			Scene scene;
			CreateGround(scene);
			Entity bottom = CreateDynamicBox(scene, "Bottom", glm::vec3(0.0f, 0.5f, 0.0f));
			if (move == Move::Kinematic)
				bottom.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;
			Entity top = CreateDynamicBox(scene, "Top", glm::vec3(0.0f, 1.5f, 0.0f));

			scene.OnRuntimeStart();
			PhysicsSystem& physics = GetPhysics(scene);
			StepScene(scene, 120);
			REQUIRE(physics.IsSleeping(top));

			// The bottom box sleeps too, so transform edits are signaled (sleeping bodies only follow signaled changes).
			REQUIRE(physics.IsSleeping(bottom));
			switch (move)
			{
				case Move::Teleport: CHECK(physics.Teleport(bottom, glm::vec3(5.0f, 0.5f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f))); break;
				case Move::TransformEdit: bottom.GetTransform().Translation.x = 5.0f; break;
				case Move::Kinematic: bottom.GetTransform().Translation.y = -5.0f; break; // Drops away below the ground
			}
			bottom.MarkModified<TransformComponent>();
			StepScene(scene, 90);
			return GetWorldPosition(scene, top).y;
		};

		CHECK(std::abs(topHeightAfterMove(Move::Teleport) - 0.5f) < 0.03f);
		CHECK(std::abs(topHeightAfterMove(Move::TransformEdit) - 0.5f) < 0.03f);
		CHECK(std::abs(topHeightAfterMove(Move::Kinematic) - 0.5f) < 0.03f);
	}

	TEST_CASE("Per-step work grows with the awake bodies, not with the sleeping ones")
	{
		Scene scene;
		CreateGround(scene);
		// A grid of boxes resting on the ground (they fall asleep and keep touching it) and a ball outside the grid.
		std::vector<Entity> boxes;
		for (int x = 0; x < 20; x++)
		{
			for (int z = 0; z < 20; z++)
				boxes.push_back(CreateDynamicBox(scene, "Box", glm::vec3(static_cast<float>(x) * 2.0f - 19.0f, 0.5f, static_cast<float>(z) * 2.0f - 19.0f)));
		}
		Entity ball = CreateDynamicSphere(scene, "Ball", glm::vec3(30.0f, 0.5f, 0.0f), 0.5f);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics);
		StepScene(scene, 90);
		PhysicsStats stats = physics.GetStats();
		REQUIRE(stats.ActiveBodyCount == 0);
		CHECK(stats.ContactPairCount == 401);

		// Everything sleeps: nothing is visited.
		StepScene(scene, 1);
		stats = physics.GetStats();
		CHECK(stats.SyncedBodyCount == 0);
		CHECK(stats.CheckedPairCount == 0);
		CHECK(stats.WrittenBodyCount == 0);

		// One body in motion: only it and its contact are visited.
		CHECK(physics.SetLinearVelocity(ball, glm::vec3(2.0f, 0.0f, 0.0f)));
		StepScene(scene, 1);
		stats = physics.GetStats();
		CHECK(stats.SyncedBodyCount == 1);
		CHECK(stats.CheckedPairCount == 1);
		CHECK(stats.WrittenBodyCount == 1);
		CHECK(stats.ContactPairCount == 401);

		// A signaled move of a sleeping body adds that body (lifted off the ground, it stops touching it and falls back).
		Entity lifted = boxes.front();
		lifted.GetTransform().Translation.y = 1.5f;
		lifted.MarkModified<TransformComponent>();
		StepScene(scene, 1);
		stats = physics.GetStats();
		CHECK(stats.SyncedBodyCount == 2);
		CHECK(stats.CheckedPairCount == 2);
		CHECK(stats.WrittenBodyCount == 2);
		CHECK(stats.ContactPairCount == 400);
		CHECK(recorder.Count(CollisionEventType::End) == 1);

		// Unsignaled edits of sleeping bodies are not looked for: the body stays until the change is signaled.
		Entity edited = boxes.back();
		const glm::vec3 restingPlace = GetWorldPosition(scene, edited);
		edited.GetTransform().Translation.z += 0.75f;
		StepScene(scene, 1);
		CHECK(physics.GetStats().SyncedBodyCount == 2);
		std::optional<RaycastHit> hit = physics.Raycast(restingPlace + glm::vec3(0.0f, 0.0f, -0.4f) + glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == edited);
		edited.MarkModified<TransformComponent>();
		StepScene(scene, 1);
		CHECK(physics.GetStats().SyncedBodyCount == 3);
		hit = physics.Raycast(restingPlace + glm::vec3(0.0f, 5.0f, -0.4f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		CHECK((!hit || hit->HitEntity != edited));

		// Once everything sleeps again, steps visit nothing, while all contacts are kept.
		CHECK(physics.SetLinearVelocity(ball, glm::vec3(0.0f)));
		CHECK(physics.SetAngularVelocity(ball, glm::vec3(0.0f)));
		StepScene(scene, 90);
		stats = physics.GetStats();
		REQUIRE(stats.ActiveBodyCount == 0);
		CHECK(stats.SyncedBodyCount == 0);
		CHECK(stats.CheckedPairCount == 0);
		CHECK(stats.WrittenBodyCount == 0);
		CHECK(stats.ContactPairCount == 401);
		CHECK(recorder.Count(CollisionEventType::Begin) == recorder.Count(CollisionEventType::End) + 401);
	}

	TEST_CASE("Kinematic bodies reach their target when a frame runs several fixed steps")
	{
		Scene scene;
		scene.GetSettings().FixedTimestep = 1.0f / 120.0f;
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		Entity pusher = CreateDynamicBox(scene, "Pusher", glm::vec3(0.0f));
		pusher.GetComponent<RigidBodyComponent>().Type = RigidBodyType::Kinematic;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		for (int frame = 1; frame <= 30; frame++)
		{
			pusher.GetTransform().Translation.x = 0.1f * static_cast<float>(frame);
			scene.OnUpdateRuntime(1.0f / 60.0f); // Two fixed steps: the first moves the body, the second holds it there

			// The body sits exactly where its entity is, at rest, after every frame.
			const float x = pusher.GetComponent<TransformComponent>().Translation.x;
			std::optional<RaycastHit> inside = physics.Raycast(glm::vec3(x + 0.45f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
			std::optional<RaycastHit> outside = physics.Raycast(glm::vec3(x + 0.55f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
			CHECK(inside.has_value());
			CHECK_FALSE(outside.has_value());
			CHECK(glm::length(physics.GetLinearVelocity(pusher)) < 1.0e-4f);
		}
		CHECK(physics.GetStats().StepCount == 60);
	}

	TEST_CASE("Steps complete while every worker thread is busy")
	{
		// Physics jobs handed to the worker pool must never be required for a step to finish, and jobs the stepping thread
		// ran itself must not stay alive in the pool's queue (they would exhaust Jolt's job pool within a few dozen steps).
		ScopedJobSystem jobSystem(2);
		std::atomic<bool> release = false;
		std::atomic<uint32_t> blocked = 0;
		std::vector<JobHandle> blockers;
		for (uint32_t index = 0; index < JobSystem::GetWorkerThreadCount(); index++)
		{
			blockers.push_back(JobSystem::Submit([&]()
			{
				blocked++;
				while (!release)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}));
		}
		while (blocked < JobSystem::GetWorkerThreadCount())
			std::this_thread::yield();

		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		{
			Scene scene;
			CreateGround(scene);
			for (int index = 0; index < 8; index++)
				CreateDynamicSphere(scene, "Ball", glm::vec3(static_cast<float>(index) * 0.3f, 1.0f + static_cast<float>(index), 0.0f), 0.25f);
			Entity probe = CreateDynamicBox(scene, "Probe", glm::vec3(10.0f, 3.0f, 0.0f));

			scene.OnRuntimeStart();
			StepScene(scene, 200);
			CHECK(std::abs(GetWorldPosition(scene, probe).y - 0.5f) < 0.03f);
		} // Destroying the world while the workers are still busy must not wait for them either

		release = true;
		JobSystem::WaitAll(blockers);
		CHECK(CountLogMessages(logStart, "job pool is exhausted") == 0);
	}

	TEST_CASE("A thousand bodies simulate without errors on the job system")
	{
		// Without a JobSystem every job runs on the stepping thread.
		uint64_t singleThreadedJobs = 0;
		{
			Scene scene;
			CreateGround(scene);
			CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 1.0f, 0.0f));
			scene.OnRuntimeStart();
			StepScene(scene, 5);
			const PhysicsStats stats = GetPhysics(scene).GetStats();
			singleThreadedJobs = stats.JobCount;
			CHECK(stats.JobCount > 0);
			CHECK(stats.WorkerJobCount == 0);
		}

		// With one, the step's jobs are handed to the workers; how many they run before the stepping thread takes the rest
		// depends on thread timing, so only the total is checked.
		ScopedJobSystem jobSystem(3);
		Scene scene("Stress");
		CreateGround(scene);
		std::vector<Entity> bodies;
		for (int x = 0; x < 10; x++)
		{
			for (int y = 0; y < 10; y++)
			{
				for (int z = 0; z < 10; z++)
				{
					// 0.1 apart, so that the bottom layer lands within a few steps and the layers start colliding.
					const glm::vec3 position(static_cast<float>(x) - 5.0f, 0.45f + static_cast<float>(y), static_cast<float>(z) - 5.0f);
					bodies.push_back((x + y + z) % 2 == 0 ? CreateDynamicBox(scene, "Box", position, glm::vec3(0.4f)) : CreateDynamicSphere(scene, "Ball", position, 0.4f));
				}
			}
		}

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 15);

		const PhysicsStats stats = physics.GetStats();
		CHECK(stats.BodyCount == 1001);
		CHECK(stats.DynamicBodyCount == 1000);
		CHECK(stats.StepCount == 15);
		CHECK(stats.ContactPairCount >= 100); // At least the bottom layer rests on the ground
		CHECK(stats.JobCount > singleThreadedJobs);
		CHECK(stats.WorkerJobCount <= stats.JobCount);
		size_t valid = 0;
		for (Entity body : bodies)
		{
			const glm::vec3 position = GetWorldPosition(scene, body);
			if (IsFinite(position) && position.y > 0.3f)
				valid++;
		}
		CHECK(valid == bodies.size());
	}
}
