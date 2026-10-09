#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"
#include "Strata/Physics/PhysicsRuntime.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/ComponentAccess.h"

#include <limits>

using namespace Strata;
using namespace Strata::Tests;

TEST_SUITE("Physics.Lifecycle")
{
	TEST_CASE("Jolt is initialized while a physics world exists")
	{
		REQUIRE_FALSE(PhysicsRuntime::IsInitialized());
		{
			Scene first;
			Scene second;
			first.OnRuntimeStart();
			CHECK(PhysicsRuntime::GetReferenceCount() == 1);
			second.OnRuntimeStart();
			CHECK(PhysicsRuntime::GetReferenceCount() == 2);
			first.OnRuntimeStop();
			CHECK(PhysicsRuntime::IsInitialized());
		}
		CHECK_FALSE(PhysicsRuntime::IsInitialized());

		// Standalone worlds work without a scene system, and Jolt can be initialized again after a shutdown.
		Scene scene;
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 2.0f, 0.0f));
		{
			PhysicsWorld world(scene);
			world.CreateAllBodies();
			CHECK(world.HasBody(box));
			world.Simulate(1.0f / 60.0f);
			world.Simulate(0.0f); // Ignored
			world.Simulate(std::numeric_limits<float>::quiet_NaN());
			CHECK(world.GetStats().StepCount == 1);
			CHECK(GetWorldPosition(scene, box).y < 2.0f);
		}
		CHECK_FALSE(PhysicsRuntime::IsInitialized());
	}

	TEST_CASE("Bodies are created for entities with colliders")
	{
		Scene scene;
		Entity staticBox = CreateStaticBox(scene, "Static", glm::vec3(0.0f), glm::vec3(1.0f));
		Entity dynamicBox = CreateDynamicBox(scene, "Dynamic", glm::vec3(5.0f, 0.0f, 0.0f));
		Entity colliderless = scene.CreateEntity("Colliderless");
		colliderless.AddComponent<RigidBodyComponent>();
		Entity plain = scene.CreateEntity("Plain");

		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK(physics.HasBody(staticBox));
		CHECK(physics.HasBody(dynamicBox));
		CHECK_FALSE(physics.HasBody(colliderless));
		CHECK_FALSE(physics.HasBody(plain));
		CHECK(physics.GetStats().BodyCount == 2);
		CHECK(CountLogMessages(logStart, "'Colliderless' has a Rigid Body but no collider") == 1);

		// The warning is issued once per entity, however often the entity changes.
		colliderless.MarkModified<RigidBodyComponent>();
		StepScene(scene, 1);
		CHECK(CountLogMessages(logStart, "'Colliderless' has a Rigid Body but no collider") == 1);
	}

	TEST_CASE("Adding and removing components at runtime creates and destroys bodies")
	{
		Scene scene;
		CreateGround(scene);
		Entity entity = scene.CreateEntity("Late");
		entity.GetTransform().Translation = glm::vec3(0.0f, 5.0f, 0.0f);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK_FALSE(physics.HasBody(entity));

		// A collider alone makes a static body.
		entity.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.5f);
		StepScene(scene, 10);
		CHECK(physics.HasBody(entity));
		CHECK(GetWorldPosition(scene, entity).y == doctest::Approx(5.0f));
		CHECK(physics.GetStats().StaticBodyCount == 2);

		// Adding a rigid body makes it dynamic.
		entity.AddComponent<RigidBodyComponent>();
		StepScene(scene, 30);
		CHECK(GetWorldPosition(scene, entity).y < 4.5f);
		CHECK(physics.GetStats().DynamicBodyCount == 1);

		// Removing the rigid body makes it static again, where it is.
		entity.RemoveComponent<RigidBodyComponent>();
		StepScene(scene, 1);
		const float frozenHeight = GetWorldPosition(scene, entity).y;
		StepScene(scene, 30);
		CHECK(GetWorldPosition(scene, entity).y == doctest::Approx(frozenHeight));
		CHECK(physics.GetStats().DynamicBodyCount == 0);

		// Without colliders there is no body.
		entity.RemoveComponent<BoxColliderComponent>();
		StepScene(scene, 1);
		CHECK_FALSE(physics.HasBody(entity));
		CHECK(physics.GetStats().BodyCount == 1);

		// Destroyed entities lose their bodies.
		Entity doomed = CreateDynamicBox(scene, "Doomed", glm::vec3(5.0f, 1.0f, 0.0f));
		StepScene(scene, 1);
		CHECK(physics.HasBody(doomed));
		scene.DestroyEntity(doomed);
		StepScene(scene, 1);
		CHECK(physics.GetStats().BodyCount == 1);
	}

	TEST_CASE("Property edits rebuild the body and keep its motion")
	{
		Scene scene;
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f));
		box.GetComponent<RigidBodyComponent>().LinearDamping = 0.0f;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK(physics.SetLinearVelocity(box, glm::vec3(2.0f, 0.0f, 0.0f)));
		StepScene(scene, 30);

		// Edits through the reflection layer (inspector, automation, scripts) notify the system.
		const ComponentInfo* info = ComponentRegistry::Find("RigidBody");
		REQUIRE(info);
		const PropertyInfo* mass = info->FindProperty("Mass");
		REQUIRE(mass);
		CHECK(ComponentAccess::SetProperty(box, *info, *mass, PropertyValue(8.0f)));
		CHECK(physics.GetLinearVelocity(box).x == doctest::Approx(2.0f));
		CHECK(physics.AddImpulse(box, glm::vec3(8.0f, 0.0f, 0.0f)));
		CHECK(physics.GetLinearVelocity(box).x == doctest::Approx(3.0f)); // The new mass applies

		const PropertyInfo* type = info->FindProperty("Type");
		REQUIRE(type);
		CHECK(ComponentAccess::SetProperty(box, *info, *type, PropertyValue(static_cast<int>(RigidBodyType::Static))));
		const float x = GetWorldPosition(scene, box).x;
		StepScene(scene, 10);
		CHECK(GetWorldPosition(scene, box).x == doctest::Approx(x));
		CHECK(glm::length(physics.GetLinearVelocity(box)) == 0.0f);

		// Collider edits through MarkModified.
		box.GetComponent<BoxColliderComponent>().HalfExtents = glm::vec3(2.0f);
		box.MarkModified<BoxColliderComponent>();
		std::optional<RaycastHit> hit = physics.Raycast(GetWorldPosition(scene, box) + glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
		REQUIRE(hit);
		CHECK(hit->Distance == doctest::Approx(8.0f));
	}

	TEST_CASE("Deactivated entities leave the simulation until reactivated")
	{
		Scene scene;
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		Entity parent = scene.CreateEntity("Parent");
		Entity child = CreateDynamicBox(scene, "Child", glm::vec3(0.0f));
		scene.SetParent(child, parent);
		child.GetComponent<RigidBodyComponent>().LinearDamping = 0.0f;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK(physics.SetLinearVelocity(child, glm::vec3(1.0f, 0.0f, 0.0f)));
		StepScene(scene, 30);
		const float x = GetWorldPosition(scene, child).x;
		CHECK(x == doctest::Approx(0.5f).epsilon(0.02));

		child.SetActive(false);
		CHECK_FALSE(physics.HasBody(child));
		CHECK_FALSE(physics.Raycast(glm::vec3(x, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f));
		CHECK(physics.GetStats().BodyCount == 0);
		StepScene(scene, 30);
		CHECK(GetWorldPosition(scene, child).x == doctest::Approx(x));

		child.SetActive(true);
		CHECK(physics.HasBody(child));
		CHECK(physics.GetLinearVelocity(child).x == doctest::Approx(1.0f)); // Motion resumes where it stopped
		StepScene(scene, 30);
		CHECK(GetWorldPosition(scene, child).x == doctest::Approx(x + 0.5f).epsilon(0.02));

		// Deactivating an ancestor deactivates the subtree.
		parent.SetActive(false);
		CHECK_FALSE(physics.HasBody(child));
		parent.SetActive(true);
		CHECK(physics.HasBody(child));

		// Moved while inactive: the body comes back at the new place.
		child.SetActive(false);
		StepScene(scene, 1);
		child.GetTransform().Translation = glm::vec3(0.0f, 20.0f, 0.0f);
		child.SetActive(true);
		std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(0.0f, 30.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == child);
	}

	TEST_CASE("Entities created inactive get their body when activated")
	{
		Scene scene;
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 5.0f, 0.0f));
		box.SetActive(false);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK_FALSE(physics.HasBody(box));
		StepScene(scene, 10);
		CHECK(GetWorldPosition(scene, box).y == doctest::Approx(5.0f));

		box.SetActive(true);
		StepScene(scene, 10);
		CHECK(physics.HasBody(box));
		CHECK(GetWorldPosition(scene, box).y < 5.0f);
	}

	TEST_CASE("The body API returns neutral values without a body")
	{
		Scene scene;
		Entity plain = scene.CreateEntity("Plain");
		Entity wall = CreateStaticBox(scene, "Wall", glm::vec3(0.0f), glm::vec3(1.0f));
		Scene other;
		Entity foreign = CreateDynamicBox(other, "Foreign", glm::vec3(0.0f));

		PhysicsSystem stopped(scene);
		CHECK_FALSE(stopped.IsRunning());
		CHECK(stopped.GetWorld() == nullptr);
		CHECK_FALSE(stopped.HasBody(wall));
		CHECK_FALSE(stopped.Raycast(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f));
		CHECK(stopped.GetStats().BodyCount == 0);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK(physics.IsRunning());
		const glm::vec3 one(1.0f);
		for (Entity entity : { plain, foreign, Entity() })
		{
			CHECK_FALSE(physics.HasBody(entity));
			CHECK(physics.GetLinearVelocity(entity) == glm::vec3(0.0f));
			CHECK(physics.GetAngularVelocity(entity) == glm::vec3(0.0f));
			CHECK_FALSE(physics.SetLinearVelocity(entity, one));
			CHECK_FALSE(physics.SetAngularVelocity(entity, one));
			CHECK_FALSE(physics.AddForce(entity, one));
			CHECK_FALSE(physics.AddForceAtPosition(entity, one, one));
			CHECK_FALSE(physics.AddImpulse(entity, one));
			CHECK_FALSE(physics.AddImpulseAtPosition(entity, one, one));
			CHECK_FALSE(physics.AddTorque(entity, one));
			CHECK_FALSE(physics.SetGravityScale(entity, 0.0f));
			CHECK_FALSE(physics.IsSleeping(entity));
			CHECK_FALSE(physics.WakeUp(entity));
			CHECK_FALSE(physics.Teleport(entity, one, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
		}

		// Static bodies have no motion to change.
		CHECK(physics.HasBody(wall));
		CHECK_FALSE(physics.SetLinearVelocity(wall, one));
		CHECK_FALSE(physics.AddForce(wall, one));
		CHECK_FALSE(physics.IsSleeping(wall));
		CHECK_FALSE(physics.WakeUp(wall));
		CHECK(physics.GetLinearVelocity(wall) == glm::vec3(0.0f));
	}

	TEST_CASE("Teleport moves the body and its entity")
	{
		Scene scene;
		Entity parent = scene.CreateEntity("Parent");
		parent.GetTransform().Translation = glm::vec3(100.0f, 0.0f, 0.0f);
		Entity box = scene.CreateChildEntity(parent, "Box");
		box.GetTransform().Scale = glm::vec3(2.0f);
		box.AddComponent<RigidBodyComponent>().GravityScale = 0.0f;
		box.AddComponent<BoxColliderComponent>();
		Entity wall = CreateStaticBox(scene, "Wall", glm::vec3(0.0f), glm::vec3(1.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const glm::quat rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(0.0f, 0.0f, 1.0f));
		CHECK(physics.SetLinearVelocity(box, glm::vec3(0.0f, 0.0f, 1.0f)));
		CHECK(physics.Teleport(box, glm::vec3(0.0f, 10.0f, 0.0f), rotation));
		CHECK(Math::IsNearlyEqual(GetWorldPosition(scene, box), glm::vec3(0.0f, 10.0f, 0.0f), 1.0e-4f));
		CHECK(Math::IsNearlyEqual(GetWorldRotation(scene, box), rotation, 1.0e-5f));
		CHECK(Math::IsNearlyEqual(box.GetComponent<TransformComponent>().Translation, glm::vec3(-100.0f, 10.0f, 0.0f), 1.0e-4f));
		CHECK(Math::IsNearlyEqual(box.GetComponent<TransformComponent>().Scale, glm::vec3(2.0f), 1.0e-4f));
		CHECK(physics.GetLinearVelocity(box).z == doctest::Approx(1.0f)); // Velocity is kept

		std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(0.0f, 20.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 30.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == box);

		// Static bodies can be teleported too; invalid input is rejected.
		CHECK(physics.Teleport(wall, glm::vec3(50.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
		CHECK(Math::IsNearlyEqual(GetWorldPosition(scene, wall), glm::vec3(50.0f, 0.0f, 0.0f), 1.0e-4f));
		CHECK_FALSE(physics.Teleport(wall, glm::vec3(std::numeric_limits<float>::quiet_NaN()), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
		CHECK_FALSE(physics.Teleport(wall, glm::vec3(0.0f), glm::quat(0.0f, 0.0f, 0.0f, 0.0f)));
		CHECK(Math::IsNearlyEqual(GetWorldPosition(scene, wall), glm::vec3(50.0f, 0.0f, 0.0f), 1.0e-4f));
	}

	TEST_CASE("Invalid component data never produces non-finite state")
	{
		Scene scene;
		CreateGround(scene);
		const float nan = std::numeric_limits<float>::quiet_NaN();

		Entity broken = CreateDynamicBox(scene, "Broken", glm::vec3(0.0f, 2.0f, 0.0f));
		broken.GetComponent<BoxColliderComponent>().HalfExtents = glm::vec3(nan, 0.5f, 0.5f);

		Entity flat = scene.CreateEntity("Flat");
		flat.GetTransform().Scale = glm::vec3(1.0f, 0.0f, 1.0f);
		flat.AddComponent<BoxColliderComponent>();

		Entity odd = CreateDynamicBox(scene, "Odd", glm::vec3(5.0f, 2.0f, 0.0f));
		RigidBodyComponent& oddBody = odd.GetComponent<RigidBodyComponent>();
		oddBody.Mass = nan;
		oddBody.LinearDamping = -1.0f;
		oddBody.GravityScale = std::numeric_limits<float>::infinity();
		oddBody.Restitution = 7.0f;
		oddBody.Layer = 40;
		odd.GetComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.0f);

		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK_FALSE(physics.HasBody(broken));
		CHECK_FALSE(physics.HasBody(flat));
		CHECK(physics.HasBody(odd));
		CHECK(CountLogMessages(logStart, "non-finite dimensions") == 1);
		CHECK(CountLogMessages(logStart, "'Flat' has a degenerate world transform") == 1);
		CHECK(CountLogMessages(logStart, "'Odd' uses collision layer 40") == 1);

		StepScene(scene, 120);
		CHECK(IsFinite(GetWorldPosition(scene, odd)));
		CHECK(GetWorldPosition(scene, odd).y > -0.1f); // Tiny, but still collides with the ground on layer 0
	}

	TEST_CASE("Stopping the runtime releases every body")
	{
		Scene scene;
		CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 2.0f, 0.0f));
		scene.OnRuntimeStart();
		CollisionRecorder recorder(GetPhysics(scene));
		StepScene(scene, 60);
		REQUIRE(recorder.Count(CollisionEventType::Begin) == 1);

		scene.OnRuntimeStop();
		CHECK_FALSE(PhysicsRuntime::IsInitialized());
		CHECK(recorder.Count(CollisionEventType::End) == 0); // The world is torn down without events

		// Signals are disconnected: editing components after the stop is harmless, and a restart rebuilds everything.
		box.RemoveComponent<BoxColliderComponent>();
		box.AddComponent<SphereColliderComponent>();
		scene.OnRuntimeStart();
		CHECK(GetPhysics(scene).HasBody(box));
	}
}

TEST_SUITE("Physics.Layers")
{
	TEST_CASE("Bodies on layers that do not collide pass through each other")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		ground.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		ground.GetComponent<RigidBodyComponent>().Layer = 1;
		Entity ghost = CreateDynamicBox(scene, "Ghost", glm::vec3(0.0f, 2.0f, 0.0f));
		ghost.GetComponent<RigidBodyComponent>().Layer = 2;
		Entity solid = CreateDynamicBox(scene, "Solid", glm::vec3(3.0f, 2.0f, 0.0f));
		solid.GetComponent<RigidBodyComponent>().Layer = 3;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK(physics.DoLayersCollide(1, 2));
		physics.SetLayersCollide(1, 2, false);
		CHECK_FALSE(physics.DoLayersCollide(1, 2));
		CHECK_FALSE(physics.DoLayersCollide(2, 1));
		CHECK(physics.DoLayersCollide(1, 3));
		CHECK_FALSE(physics.DoLayersCollide(1, 32));

		StepScene(scene, 90);
		CHECK(GetWorldPosition(scene, ghost).y < -2.0f);
		CHECK(std::abs(GetWorldPosition(scene, solid).y - 0.5f) < 0.03f);
	}

	TEST_CASE("The default settings configure the layer matrix of new worlds")
	{
		PhysicsSettings settings;
		settings.SetLayersCollide(4, 4, false);
		settings.SetLayersCollide(4, 0, false);
		CHECK_FALSE(settings.DoLayersCollide(4, 4));
		CHECK_FALSE(settings.DoLayersCollide(0, 4));
		CHECK(settings.DoLayersCollide(4, 5));

		// An asymmetric matrix disables the pair.
		settings.LayerCollisionMasks[6] &= ~ST_BIT(7);
		CHECK_FALSE(settings.DoLayersCollide(6, 7));
		CHECK_FALSE(settings.DoLayersCollide(7, 6));

		ScopedPhysicsSettings scopedSettings(settings);
		Scene scene;
		CreateGround(scene); // Layer 0
		Entity first = CreateDynamicBox(scene, "First", glm::vec3(0.0f, 1.0f, 0.0f));
		Entity second = CreateDynamicBox(scene, "Second", glm::vec3(0.0f, 3.0f, 0.0f));
		first.GetComponent<RigidBodyComponent>().Layer = 4;
		second.GetComponent<RigidBodyComponent>().Layer = 4;

		scene.OnRuntimeStart();
		CHECK_FALSE(GetPhysics(scene).DoLayersCollide(4, 4));
		StepScene(scene, 90);
		CHECK(GetWorldPosition(scene, first).y < -2.0f);
		CHECK(GetWorldPosition(scene, second).y < -2.0f);
	}
}
