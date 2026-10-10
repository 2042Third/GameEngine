#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"
#include "Strata/Physics/PhysicsRuntime.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Scene/ComponentAccess.h"

#include <limits>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Destroys entities from inside the scene update (destruction is deferred to the end of the frame) and records what
	// physics reports for them in between. Runs after the physics system, which is registered first.
	struct DeferredDestroySystem : public SceneSystem
	{
		explicit DeferredDestroySystem(Scene& scene)
			: TargetScene(scene)
		{
		}

		static void Reset()
		{
			Armed = false;
			Checked = false;
			QueryHitDoomed = QueryHitChild = QueryHitWall = true;
			DoomedSimulated = ChildSimulated = true;
		}

		void OnUpdate(Timestep) override
		{
			if (!Armed)
				return;
			TargetScene.DestroyEntity(TargetScene.FindEntityByName("Doomed"));
			TargetScene.DestroyEntity(TargetScene.FindEntityByName("DoomedWall"));
		}

		void OnLateUpdate(Timestep) override
		{
			if (!Armed)
				return;
			Armed = false;

			PhysicsSystem* physics = TargetScene.GetSystem<PhysicsSystem>();
			const auto hits = [&](float x, std::string_view name)
			{
				std::optional<RaycastHit> hit = physics->Raycast(glm::vec3(x, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);
				return hit && hit->HitEntity == TargetScene.FindEntityByName(name);
			};
			QueryHitDoomed = hits(0.0f, "Doomed");
			QueryHitChild = hits(3.0f, "DoomedChild");
			QueryHitWall = hits(-3.0f, "DoomedWall");
			DoomedSimulated = physics->HasBody(TargetScene.FindEntityByName("Doomed"));
			ChildSimulated = physics->HasBody(TargetScene.FindEntityByName("DoomedChild"));
			Checked = true;
		}

		Scene& TargetScene;
		static inline bool Armed = false;
		static inline bool Checked = false;
		static inline bool QueryHitDoomed = true;
		static inline bool QueryHitChild = true;
		static inline bool QueryHitWall = true;
		static inline bool DoomedSimulated = true;
		static inline bool ChildSimulated = true;
	};

}

TEST_SUITE("Physics.Lifecycle")
{
	TEST_CASE("The physics system is a built-in scene system that also runs in simulate mode")
	{
		const std::vector<SceneSystemDescriptor>& descriptors = SceneSystemRegistry::GetAll();
		auto it = std::find_if(descriptors.begin(), descriptors.end(), [](const SceneSystemDescriptor& descriptor) { return descriptor.Name == "Physics"; });
		REQUIRE(it != descriptors.end());
		CHECK(it->RunsInSimulateMode);

		Scene scene;
		const Scope<SceneSystem> system = it->Create(scene);
		CHECK(dynamic_cast<PhysicsSystem*>(system.get()) != nullptr);
	}

	TEST_CASE("Jolt is initialized while a physics world exists")
	{
		REQUIRE_FALSE(PhysicsRuntime::IsInitialized());
		{
			Scene first;
			Scene second;
			CreateGround(first);
			CreateGround(second);
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
		scene.MarkTransformChanged(child); // No on_update: the reactivation alone brings the body to the new place
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
			CHECK_FALSE(physics.AddAngularImpulse(entity, one));
			CHECK_FALSE(physics.SetGravityScale(entity, 0.0f));
			CHECK_FALSE(physics.IsSleeping(entity));
			CHECK_FALSE(physics.WakeUp(entity));
			CHECK_FALSE(physics.Teleport(entity, one, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
		}

		// Static bodies have no motion to change.
		CHECK(physics.HasBody(wall));
		CHECK_FALSE(physics.SetLinearVelocity(wall, one));
		CHECK_FALSE(physics.AddForce(wall, one));
		CHECK_FALSE(physics.AddAngularImpulse(wall, one));
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

	TEST_CASE("Scenes without physics components create the simulation only when needed")
	{
		Scene scene;
		scene.CreateEntity("Empty");
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK(physics.IsRunning());
		CHECK(physics.GetWorld() == nullptr);
		CHECK_FALSE(PhysicsRuntime::IsInitialized());
		StepScene(scene, 5);
		CHECK(physics.GetWorld() == nullptr);
		CHECK_FALSE(physics.Raycast(glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f));

		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 5.0f, 0.0f));
		StepScene(scene, 10);
		CHECK(physics.GetWorld() != nullptr);
		CHECK(physics.HasBody(box));
		CHECK(GetWorldPosition(scene, box).y < 5.0f);
	}

	TEST_CASE("Entities whose body cannot be built yet get it once possible")
	{
		const Ref<const PhysicsMeshData> cube = []()
		{
			Ref<PhysicsMeshData> mesh = CreateRef<PhysicsMeshData>();
			for (int index = 0; index < 8; index++)
				mesh->Positions.emplace_back((index & 1) ? 0.5f : -0.5f, (index & 2) ? 0.5f : -0.5f, (index & 4) ? 0.5f : -0.5f);
			return Ref<const PhysicsMeshData>(mesh);
		}();
		bool meshLoaded = false;
		Ref<FunctionMeshProvider> meshes = CreateRef<FunctionMeshProvider>([&](AssetHandle) -> Ref<const PhysicsMeshData>
		{
			return meshLoaded ? cube : nullptr;
		});
		ScopedMeshProvider provider(meshes);

		Scene scene;
		CreateGround(scene);
		Entity flat = CreateDynamicBox(scene, "Flat", glm::vec3(0.0f, 3.0f, 0.0f));
		flat.GetTransform().Scale = glm::vec3(0.0f);
		Entity rock = scene.CreateEntity("Rock");
		rock.GetTransform().Translation = glm::vec3(10.0f, 0.5f, 0.0f);
		rock.AddComponent<MeshColliderComponent>().Mesh = UUID(0x5001);

		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK_FALSE(physics.HasBody(flat));
		CHECK_FALSE(physics.HasBody(rock));
		CHECK(physics.GetStats().PendingBodyCount == 2);

		// Retried once the cause can be gone (the transform changes, the provider reports new mesh data): steps in between
		// neither repeat the warnings nor ask for the mesh again.
		const uint32_t requests = meshes->GetRequestCount();
		CHECK(requests >= 1);
		StepScene(scene, 5);
		CHECK_FALSE(physics.HasBody(rock));
		CHECK(meshes->GetRequestCount() == requests);
		CHECK(CountLogMessages(logStart, "'Flat' has a degenerate world transform") == 1);
		CHECK(CountLogMessages(logStart, "'Rock' waits for mesh") == 1);
		CHECK(Math::IsNearlyEqual(flat.GetComponent<TransformComponent>().Scale, glm::vec3(0.0f)));

		// Records waiting for a valid transform are retried when a transform change is signaled.
		flat.GetTransform().Scale = glm::vec3(1.0f);
		flat.MarkModified<TransformComponent>();
		meshLoaded = true;
		meshes->Changed();
		StepScene(scene, 1);
		CHECK(physics.HasBody(flat));
		CHECK(physics.HasBody(rock));
		CHECK(physics.GetStats().PendingBodyCount == 0);
		std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(10.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == rock);

		StepScene(scene, 150);
		CHECK(std::abs(GetWorldPosition(scene, flat).y - 0.5f) < 0.03f);
	}

	TEST_CASE("Rebuilding a body in a full world keeps the body and its motion")
	{
		PhysicsSettings settings;
		settings.MaxBodies = 2;
		ScopedPhysicsSettings scopedSettings(settings);

		Scene scene;
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		Entity mover = CreateDynamicBox(scene, "Mover", glm::vec3(0.0f));
		mover.GetComponent<RigidBodyComponent>().LinearDamping = 0.0f;
		Entity other = CreateDynamicBox(scene, "Other", glm::vec3(0.0f, 10.0f, 0.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(physics.GetStats().BodyCount == 2);
		CHECK(physics.SetLinearVelocity(mover, glm::vec3(1.0f, 0.0f, 0.0f)));
		CHECK(physics.SetAngularVelocity(mover, glm::vec3(0.0f, 0.5f, 0.0f)));
		StepScene(scene, 30);
		const glm::vec3 angularVelocity = physics.GetAngularVelocity(mover);

		// Rebuilds need a second body while the world is full: the old one makes room instead of being lost.
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		mover.GetComponent<RigidBodyComponent>().Friction = 0.25f;
		mover.MarkModified<RigidBodyComponent>();
		CHECK(physics.HasBody(mover));
		CHECK(physics.GetLinearVelocity(mover).x == doctest::Approx(1.0f));
		CHECK(Math::IsNearlyEqual(physics.GetAngularVelocity(mover), angularVelocity, 1.0e-5f));
		mover.GetComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.25f);
		mover.MarkModified<BoxColliderComponent>();
		CHECK(physics.HasBody(mover));
		CHECK(physics.GetLinearVelocity(mover).x == doctest::Approx(1.0f));
		CHECK(physics.GetStats().BodyCount == 2);
		CHECK(physics.GetStats().PendingBodyCount == 0);
		CHECK(CountLogMessages(logStart, "limit of 2 bodies") == 0);

		const float x = GetWorldPosition(scene, mover).x;
		StepScene(scene, 30);
		CHECK(GetWorldPosition(scene, mover).x == doctest::Approx(x + 0.5f).epsilon(0.01));
		// The new, smaller box (spinning about Y, so its top stays level).
		std::optional<RaycastHit> hit = physics.Raycast(GetWorldPosition(scene, mover) + glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == mover);
		CHECK(hit->Distance == doctest::Approx(4.75f).epsilon(1.0e-3));

		// New bodies wait for a free slot.
		Entity late = CreateDynamicBox(scene, "Late", glm::vec3(0.0f, -10.0f, 0.0f));
		CHECK_FALSE(physics.HasBody(late));
		CHECK(physics.GetStats().PendingBodyCount == 1);
		CHECK(CountLogMessages(logStart, "'Late': the world's limit of 2 bodies is reached") == 1);
		scene.DestroyEntity(other);
		StepScene(scene, 1);
		CHECK(physics.HasBody(late));
		CHECK(physics.HasBody(mover));
	}

	TEST_CASE("A dynamic body whose mesh is briefly unavailable resumes its motion")
	{
		Ref<PhysicsMeshData> cube = CreateRef<PhysicsMeshData>();
		for (int index = 0; index < 8; index++)
			cube->Positions.emplace_back((index & 1) ? 0.5f : -0.5f, (index & 2) ? 0.5f : -0.5f, (index & 4) ? 0.5f : -0.5f);
		const AssetHandle firstMesh = UUID(0x6001);
		const AssetHandle secondMesh = UUID(0x6002);
		bool secondLoaded = false;
		Ref<FunctionMeshProvider> meshes = CreateRef<FunctionMeshProvider>([&](AssetHandle mesh) -> Ref<const PhysicsMeshData>
		{
			return mesh == firstMesh || (mesh == secondMesh && secondLoaded) ? cube : nullptr;
		});
		ScopedMeshProvider provider(meshes);

		Scene scene;
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		Entity rock = scene.CreateEntity("Rock");
		rock.AddComponent<RigidBodyComponent>().LinearDamping = 0.0f;
		rock.AddComponent<MeshColliderComponent>().Mesh = firstMesh;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(physics.HasBody(rock));
		CHECK(physics.SetLinearVelocity(rock, glm::vec3(0.0f, 0.0f, 2.0f)));
		StepScene(scene, 30);

		// Switched to a mesh that is still loading: the body waits outside the simulation.
		rock.GetComponent<MeshColliderComponent>().Mesh = secondMesh;
		rock.MarkModified<MeshColliderComponent>();
		CHECK_FALSE(physics.HasBody(rock));
		const float z = GetWorldPosition(scene, rock).z;
		CHECK(z == doctest::Approx(1.0f).epsilon(0.01));
		StepScene(scene, 10);
		CHECK(GetWorldPosition(scene, rock).z == doctest::Approx(z));

		secondLoaded = true;
		meshes->Changed();
		StepScene(scene, 1);
		REQUIRE(physics.HasBody(rock));
		CHECK(physics.GetLinearVelocity(rock).z == doctest::Approx(2.0f));
		StepScene(scene, 29);
		CHECK(GetWorldPosition(scene, rock).z == doctest::Approx(z + 1.0f).epsilon(0.01));
	}

	TEST_CASE("A body whose transform becomes degenerate leaves the simulation without restoring its scale")
	{
		Scene scene;
		CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 5.0f, 0.0f));
		box.GetComponent<RigidBodyComponent>().LinearDamping = 0.0f;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 10);
		const float speed = physics.GetLinearVelocity(box).y;
		REQUIRE(speed < -1.0f);

		// Noticed without an on_update signal (the body is awake); only the scene is told.
		box.GetTransform().Scale = glm::vec3(0.0f);
		scene.MarkTransformChanged(box);
		StepScene(scene, 1);
		CHECK_FALSE(physics.HasBody(box));
		const glm::vec3 frozen = box.GetComponent<TransformComponent>().Translation;
		StepScene(scene, 10);
		const TransformComponent& transform = box.GetComponent<TransformComponent>();
		CHECK(transform.Scale == glm::vec3(0.0f));
		CHECK(transform.Translation == frozen);

		// Back with a valid scale (signaled: the suspended body is not polled) it continues with the velocity it had.
		box.GetTransform().Scale = glm::vec3(1.0f);
		box.MarkModified<TransformComponent>();
		StepScene(scene, 1);
		CHECK(physics.HasBody(box));
		CHECK(physics.GetLinearVelocity(box).y < speed);
		StepScene(scene, 120);
		CHECK(std::abs(GetWorldPosition(scene, box).y - 0.5f) < 0.03f);
	}

	TEST_CASE("Bodies waiting for a valid transform are not polled")
	{
		Scene scene;
		CreateGround(scene);
		Entity flat = CreateDynamicBox(scene, "Flat", glm::vec3(0.0f, 3.0f, 0.0f));
		flat.GetTransform().Scale = glm::vec3(0.0f);
		Entity holder = scene.CreateEntity("Holder");
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(5.0f, 3.0f, 0.0f));
		REQUIRE(scene.SetParent(box, holder));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 1);
		// The falling box gets a degenerate transform (noticed without an on_update signal: it is awake; only the scene is
		// told), then its parent is deactivated.
		box.GetTransform().Scale = glm::vec3(0.0f);
		scene.MarkTransformChanged(box);
		StepScene(scene, 1);
		REQUIRE_FALSE(physics.HasBody(box));
		holder.SetActive(false);
		StepScene(scene, 1);

		// Neither the record that cannot be built nor the suspended body costs anything per step.
		StepScene(scene, 5);
		CHECK(physics.GetStats().SyncedBodyCount == 0);
		CHECK(physics.GetStats().PendingBodyCount == 1);

		// A signaled transform change retries the build.
		flat.GetTransform().Scale = glm::vec3(1.0f);
		scene.MarkTransformChanged(flat); // The scene knows, physics is not signaled
		StepScene(scene, 1);
		CHECK_FALSE(physics.HasBody(flat)); // Not signaled yet
		flat.MarkModified<TransformComponent>();
		StepScene(scene, 1);
		CHECK(physics.HasBody(flat));

		// Reactivation brings the suspended body back once its transform is valid.
		box.GetTransform().Scale = glm::vec3(1.0f);
		scene.MarkTransformChanged(box);
		holder.SetActive(true);
		CHECK(physics.HasBody(box));
		StepScene(scene, 120);
		CHECK(std::abs(GetWorldPosition(scene, box).y - 0.5f) < 0.03f);
		CHECK(std::abs(GetWorldPosition(scene, flat).y - 0.5f) < 0.03f);
	}

	TEST_CASE("Dynamic bodies below a parent scaled to nearly zero wait until it is restored")
	{
		// Powers of two keep the arithmetic exact: compensating children keep bit-identical world transforms.
		const float tiny = std::ldexp(1.0f, -20);
		const float huge = std::ldexp(1.0f, 20);

		Scene scene;
		CreateGround(scene);
		Entity parent = scene.CreateEntity("Shrunk");
		parent.GetTransform().Scale = glm::vec3(tiny);
		// The children compensate: their world transforms are ordinary, but the parent cannot be inverted.
		Entity box = scene.CreateChildEntity(parent, "Box");
		box.GetTransform().Translation = glm::vec3(0.0f, 3.0f * huge, 0.0f);
		box.GetTransform().Scale = glm::vec3(huge);
		box.AddComponent<RigidBodyComponent>().LinearDamping = 0.0f;
		box.AddComponent<BoxColliderComponent>();
		// Static bodies never write their pose back, so the parent does not matter to them.
		Entity shelf = scene.CreateChildEntity(parent, "Shelf");
		shelf.GetTransform().Translation = glm::vec3(5.0f * huge, huge, 0.0f);
		shelf.GetTransform().Scale = glm::vec3(huge);
		shelf.AddComponent<BoxColliderComponent>();
		REQUIRE(GetWorldPosition(scene, box) == glm::vec3(0.0f, 3.0f, 0.0f));

		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		const auto warnings = [&]() { return CountLogMessages(logStart, "the parent of 'Box' is scaled to (nearly) zero"); };
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK_FALSE(physics.HasBody(box));
		CHECK(physics.HasBody(shelf));
		CHECK(physics.GetStats().PendingBodyCount == 1);
		StepScene(scene, 10);
		CHECK_FALSE(physics.HasBody(box));
		CHECK(box.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 3.0f * huge, 0.0f));
		CHECK(warnings() == 1);

		// Rescales the parent keeping the box's world pose (exactly). The edit is signaled: bodies waiting for a valid
		// transform are not polled.
		const auto setParentScale = [&](float scale)
		{
			const glm::vec3 boxWorld = GetWorldPosition(scene, box);
			parent.GetTransform().Scale = glm::vec3(scale);
			box.GetTransform().Translation = boxWorld / scale;
			box.GetTransform().Scale = glm::vec3(1.0f / scale);
			parent.MarkModified<TransformComponent>();
		};

		// Restored (the box's world transform does not change): the body is built and falls.
		setParentScale(1.0f);
		StepScene(scene, 1);
		REQUIRE(physics.HasBody(box));
		StepScene(scene, 30);
		const float speed = physics.GetLinearVelocity(box).y;
		CHECK(speed < -4.0f);

		// Shrunk while falling and moved: the body leaves the simulation before the step (warned once more, since the
		// problem was solved in between) and comes back with its motion when the parent is restored.
		setParentScale(tiny);
		box.GetTransform().Translation.x += huge;
		StepScene(scene, 1);
		CHECK_FALSE(physics.HasBody(box));
		glm::vec3 frozen = box.GetComponent<TransformComponent>().Translation;
		StepScene(scene, 10);
		CHECK(box.GetComponent<TransformComponent>().Translation == frozen);
		CHECK(warnings() == 2);

		setParentScale(1.0f);
		StepScene(scene, 1);
		REQUIRE(physics.HasBody(box));
		CHECK(physics.GetLinearVelocity(box).y < speed);
		CHECK(GetWorldPosition(scene, box).x == doctest::Approx(1.0f));

		// Shrunk without moving: only writing the pose back fails. The body returns to its entity's pose and leaves the
		// simulation until the parent is restored.
		setParentScale(tiny);
		frozen = box.GetComponent<TransformComponent>().Translation;
		StepScene(scene, 1);
		CHECK_FALSE(physics.HasBody(box));
		StepScene(scene, 10);
		CHECK(box.GetComponent<TransformComponent>().Translation == frozen);
		CHECK(warnings() == 3);
		CHECK(CountLogMessages(logStart, "Cannot set the world transform") == 0);

		setParentScale(1.0f);
		StepScene(scene, 1);
		REQUIRE(physics.HasBody(box));
		StepScene(scene, 120);
		CHECK(std::abs(GetWorldPosition(scene, box).y - 0.5f) < 0.03f);
		CHECK(GetWorldPosition(scene, box).x == doctest::Approx(1.0f).epsilon(1.0e-3));
	}

	TEST_CASE("Entities pending destruction leave the simulation and queries right away")
	{
		DeferredDestroySystem::Reset();
		REQUIRE(SceneSystemRegistry::Register({ "TestDeferredDestroy", false, [](Scene& scene) { return CreateScope<DeferredDestroySystem>(scene); } }));
		{
			Scene scene;
			Entity doomed = CreateDynamicBox(scene, "Doomed", glm::vec3(0.0f, 5.0f, 0.0f));
			doomed.GetComponent<RigidBodyComponent>().GravityScale = 0.0f;
			Entity child = CreateDynamicBox(scene, "DoomedChild", glm::vec3(3.0f, 5.0f, 0.0f));
			child.GetComponent<RigidBodyComponent>().GravityScale = 0.0f;
			scene.SetParent(child, doomed);
			Entity wall = CreateStaticBox(scene, "DoomedWall", glm::vec3(-3.0f, 5.0f, 0.0f), glm::vec3(0.5f));

			scene.OnRuntimeStart();
			PhysicsSystem& physics = GetPhysics(scene);
			CHECK(physics.HasBody(doomed));
			CHECK(physics.HasBody(child));
			CHECK(physics.HasBody(wall));

			DeferredDestroySystem::Armed = true;
			StepScene(scene, 1);
			CHECK(DeferredDestroySystem::Checked);
			CHECK_FALSE(DeferredDestroySystem::QueryHitDoomed);
			CHECK_FALSE(DeferredDestroySystem::QueryHitChild);
			CHECK_FALSE(DeferredDestroySystem::QueryHitWall);
			CHECK_FALSE(DeferredDestroySystem::DoomedSimulated);
			CHECK_FALSE(DeferredDestroySystem::ChildSimulated);
			CHECK_FALSE(doomed.IsValid());
			CHECK_FALSE(wall.IsValid());
			StepScene(scene, 1);
			CHECK(physics.GetStats().BodyCount == 0);
		}
		CHECK(SceneSystemRegistry::Unregister("TestDeferredDestroy"));
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

	TEST_CASE("A scene's layer matrix can be configured before it runs")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		ground.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		ground.GetComponent<RigidBodyComponent>().Layer = 3;
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 1.0f, 0.0f));
		box.GetComponent<RigidBodyComponent>().Layer = 4;

		PhysicsSystem physics(scene);
		physics.SetLayersCollide(3, 4, false);
		CHECK_FALSE(physics.DoLayersCollide(3, 4));
		CHECK_FALSE(physics.DoLayersCollide(4, 3));
		CHECK(physics.DoLayersCollide(3, 3));
		CHECK(PhysicsSystem::GetDefaultSettings().DoLayersCollide(3, 4)); // Only this scene is affected

		physics.OnRuntimeStart();
		REQUIRE(physics.GetWorld() != nullptr);
		CHECK_FALSE(physics.GetWorld()->DoLayersCollide(3, 4));
		for (int step = 0; step < 90; step++)
			physics.OnFixedUpdate(1.0f / 60.0f);
		CHECK(GetWorldPosition(scene, box).y < -2.0f);
	}

	TEST_CASE("Changing the layer matrix wakes sleeping bodies")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		ground.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		ground.GetComponent<RigidBodyComponent>().Layer = 1;
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f));
		box.GetComponent<RigidBodyComponent>().Layer = 2;

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics);
		StepScene(scene, 90);
		REQUIRE(physics.IsSleeping(box));
		REQUIRE(recorder.Count(CollisionEventType::Begin) == 1);

		physics.SetLayersCollide(1, 2, false);
		StepScene(scene, 60);
		CHECK(GetWorldPosition(scene, box).y < -0.5f);
		CHECK(recorder.Count(CollisionEventType::End, ground.GetUUID(), box.GetUUID()) == 1);
	}
}
