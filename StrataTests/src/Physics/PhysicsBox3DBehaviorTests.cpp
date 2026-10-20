#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Physics/PhysicsComponents.h"
#include "Strata/Physics/PhysicsMeshShapes.h"
#include "Strata/Physics/PhysicsRuntime.h"
#include "TestHelpers.h"

#include <box3d/box3d.h>

#include <cmath>
#include <vector>

// How Strata's physics behaves on box3d where box3d differs from what a physics engine might do by itself: worlds and their
// limit, contacts folded into entity pairs, triggers, hull limits, mesh lifetimes and shapes appearing under bodies.

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint64_t c_SphereMesh = 0x2001;
	constexpr uint64_t c_DenseFloorMesh = 0x2002;
	constexpr uint64_t c_PlateMesh = 0x2003;

	// The points and triangles of a sphere like the engine's built-in one (48 segments, 24 rings): its hull has more faces
	// and edges than box3d's hulls may have at 128 vertices.
	Ref<const PhysicsMeshData> CreateSphereMesh(float radius)
	{
		constexpr uint32_t c_Segments = 48;
		constexpr uint32_t c_Rings = 24;
		Ref<PhysicsMeshData> mesh = CreateRef<PhysicsMeshData>();
		for (uint32_t ring = 0; ring <= c_Rings; ring++)
		{
			const float phi = glm::pi<float>() * static_cast<float>(ring) / static_cast<float>(c_Rings);
			for (uint32_t segment = 0; segment <= c_Segments; segment++)
			{
				const float theta = glm::two_pi<float>() * static_cast<float>(segment) / static_cast<float>(c_Segments);
				mesh->Positions.emplace_back(radius * std::sin(phi) * std::cos(theta), radius * std::cos(phi), radius * std::sin(phi) * std::sin(theta));
			}
		}
		for (uint32_t ring = 0; ring < c_Rings; ring++)
		{
			for (uint32_t segment = 0; segment < c_Segments; segment++)
			{
				const uint32_t a = ring * (c_Segments + 1) + segment;
				const uint32_t b = a + c_Segments + 1;
				mesh->Indices.insert(mesh->Indices.end(), { a, a + 1, b, a + 1, b + 1, b });
			}
		}
		return mesh;
	}

	// A flat floor at y = 0 of `cells` x `cells` squares of `cellSize`, centered on the origin, facing up.
	Ref<const PhysicsMeshData> CreateGridMesh(uint32_t cells, float cellSize)
	{
		Ref<PhysicsMeshData> mesh = CreateRef<PhysicsMeshData>();
		const float half = 0.5f * static_cast<float>(cells) * cellSize;
		for (uint32_t z = 0; z <= cells; z++)
		{
			for (uint32_t x = 0; x <= cells; x++)
				mesh->Positions.emplace_back(static_cast<float>(x) * cellSize - half, 0.0f, static_cast<float>(z) * cellSize - half);
		}
		for (uint32_t z = 0; z < cells; z++)
		{
			for (uint32_t x = 0; x < cells; x++)
			{
				const uint32_t a = z * (cells + 1) + x;
				const uint32_t b = a + cells + 1;
				// Counter-clockwise seen from above.
				mesh->Indices.insert(mesh->Indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
			}
		}
		return mesh;
	}

	Ref<FunctionMeshProvider> CreateMeshProvider()
	{
		const Ref<const PhysicsMeshData> sphere = CreateSphereMesh(0.5f);
		const Ref<const PhysicsMeshData> floor = CreateGridMesh(100, 0.1f);
		const Ref<const PhysicsMeshData> plate = CreateGridMesh(4, 0.5f);
		return CreateRef<FunctionMeshProvider>([sphere, floor, plate](AssetHandle mesh) -> Ref<const PhysicsMeshData>
		{
			switch (static_cast<uint64_t>(mesh))
			{
				case c_SphereMesh: return sphere;
				case c_DenseFloorMesh: return floor;
				case c_PlateMesh: return plate;
				default: return nullptr;
			}
		});
	}

	Entity CreateMeshEntity(Scene& scene, const std::string& name, const glm::vec3& position, uint64_t mesh, bool convex)
	{
		Entity entity = scene.CreateEntity(name);
		entity.GetTransform().Translation = position;
		MeshColliderComponent& collider = entity.AddComponent<MeshColliderComponent>();
		collider.Mesh = UUID(mesh);
		collider.Convex = convex;
		return entity;
	}

	Entity CreateStaticTrigger(Scene& scene, const std::string& name, const glm::vec3& position, const glm::vec3& halfExtents)
	{
		Entity trigger = CreateStaticBox(scene, name, position, halfExtents);
		RigidBodyComponent& body = trigger.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Static;
		body.IsTrigger = true;
		return trigger;
	}

	size_t CountPhysicsWarnings(uint64_t logStart)
	{
		return CountLogMessages(logStart, "Physics: ") + CountLogMessages(logStart, "box3d: ");
	}

}

TEST_SUITE("Physics.Box3D")
{
	TEST_CASE("A scene's first world gets physics, and one that finds no free box3d world plays without physics, with one error")
	{
		// box3d holds a fixed number of worlds at once (B3_MAX_WORLDS); its b3GetMaxWorldCount is the most there ever were.
		REQUIRE(b3GetWorldCount() == 0);
		{
			Scene first("First");
			CreateGround(first);
			first.OnRuntimeStart();
			CHECK(GetPhysics(first).GetWorld() != nullptr);
			first.OnRuntimeStop();
		}

		PhysicsRuntimeReference runtime;
		std::vector<b3WorldId> held;
		while (b3GetWorldCount() < B3_MAX_WORLDS)
		{
			const b3WorldDef definition = b3DefaultWorldDef();
			held.push_back(b3CreateWorld(&definition));
		}
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		Scene crowded("Crowded");
		CreateGround(crowded);
		Entity box = CreateDynamicBox(crowded, "Box", glm::vec3(0.0f, 2.0f, 0.0f));
		crowded.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(crowded);
		CHECK(physics.GetWorld() == nullptr);
		CHECK_FALSE(physics.HasBody(box));
		StepScene(crowded, 3);
		CHECK(CountLogMessages(logStart, "Physics: scene 'Crowded' plays without physics") == 1);
		CHECK(GetWorldPosition(crowded, box).y == 2.0f);
		crowded.OnRuntimeStop();
		for (const b3WorldId world : held)
			b3DestroyWorld(world);

		// With a world free again, a scene gets physics.
		crowded.OnRuntimeStart();
		CHECK(GetPhysics(crowded).GetWorld() != nullptr);
		CHECK(GetPhysics(crowded).HasBody(box));
		crowded.OnRuntimeStop();
	}

	TEST_CASE("Bodies beyond 1e5 m from the origin wait like bodies with a degenerate transform")
	{
		Scene scene;
		Entity box = CreateDynamicBox(scene, "Far", glm::vec3(2.0e5f, 0.0f, 0.0f));
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK_FALSE(physics.HasBody(box));
		CHECK(physics.GetStats().PendingBodyCount == 1);
		CHECK(CountLogMessages(logStart, "'Far' is more than 100000 m from the origin") == 1);

		// Back within range (a signaled move), it gets its body.
		box.GetTransform().Translation = glm::vec3(10.0f, 0.0f, 0.0f);
		box.MarkModified<TransformComponent>();
		StepScene(scene, 1);
		CHECK(physics.HasBody(box));
		scene.OnRuntimeStop();
	}
}

TEST_SUITE("Physics.Queries")
{
	TEST_CASE("Bodies on a layer that collides with nothing are still hit by queries")
	{
		Scene scene;
		Entity loner = CreateStaticBox(scene, "Loner", glm::vec3(0.0f), glm::vec3(1.0f));
		loner.AddComponent<RigidBodyComponent>().Type = RigidBodyType::Static;
		loner.GetComponent<RigidBodyComponent>().Layer = 5;
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		for (uint32_t layer = 0; layer < c_PhysicsLayerCount; layer++)
			physics.SetLayersCollide(5, layer, false);

		const std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(hit.has_value());
		CHECK(hit->HitEntity == loner);
		CHECK(physics.Raycast(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f, ST_BIT(5)).has_value());
		CHECK_FALSE(physics.Raycast(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f, ~ST_BIT(5)).has_value());
		CHECK(physics.OverlapSphere(glm::vec3(0.0f), 0.5f).size() == 1);
		scene.OnRuntimeStop();
	}
}

TEST_SUITE("Physics.Events")
{
	TEST_CASE("A watch begun while bodies touch reports Begin at the next step and End when they part")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f));
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		ICollisionEventSource* contacts = scene.GetService<ICollisionEventSource>();
		REQUIRE(contacts != nullptr);
		StepScene(scene, 120);
		REQUIRE(physics.IsSleeping(box)); // Touching for a long time, asleep: box3d reports nothing new

		std::vector<CollisionEvent> events;
		const CollisionListenerID listener = contacts->AddCollisionListener([&](const CollisionEvent& event) { events.push_back(event); },
			CollisionListenerScope::WatchedEntities);
		contacts->WatchCollisions(box.GetUUID());
		StepScene(scene, 1);
		REQUIRE(events.size() == 1);
		CHECK(events[0].Type == CollisionEventType::Begin);
		CHECK(events[0].Involves(ground.GetUUID()));

		CHECK(physics.Teleport(box, glm::vec3(0.0f, 5.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
		StepScene(scene, 2);
		REQUIRE(events.size() == 2);
		CHECK(events[1].Type == CollisionEventType::End);
		CHECK(contacts->RemoveCollisionListener(listener));
		scene.OnRuntimeStop();
	}

	TEST_CASE("A static trigger touching static colliders and other static triggers reports nothing")
	{
		Scene scene;
		Entity floor = CreateGround(scene);
		Entity zone = CreateStaticTrigger(scene, "Zone", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(2.0f, 1.0f, 2.0f));
		Entity overlap = CreateStaticTrigger(scene, "Overlap", glm::vec3(1.0f, 0.5f, 0.0f), glm::vec3(2.0f, 1.0f, 2.0f));
		Entity wall = CreateStaticBox(scene, "Wall", glm::vec3(2.0f, 1.0f, 0.0f), glm::vec3(0.2f, 1.0f, 2.0f));
		Entity ball = CreateDynamicSphere(scene, "Ball", glm::vec3(-1.0f, 5.0f, 0.0f), 0.25f);
		scene.OnRuntimeStart();
		CollisionRecorder recorder(GetPhysics(scene));
		StepScene(scene, 90);
		for (const Entity trigger : { zone, overlap })
		{
			CHECK(recorder.Count(CollisionEventType::Begin, trigger.GetUUID(), floor.GetUUID()) == 0);
			CHECK(recorder.Count(CollisionEventType::Begin, trigger.GetUUID(), wall.GetUUID()) == 0);
		}
		CHECK(recorder.Count(CollisionEventType::Begin, zone.GetUUID(), overlap.GetUUID()) == 0);
		// What moves into them is reported.
		CHECK(recorder.Count(CollisionEventType::Begin, zone.GetUUID(), ball.GetUUID()) == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A body of several shapes touching another body begins one contact")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity bench = scene.CreateEntity("Bench");
		bench.GetTransform().Translation = glm::vec3(0.0f, 0.4f, 0.0f);
		bench.AddComponent<RigidBodyComponent>();
		for (const float x : { -0.8f, 0.0f, 0.8f })
		{
			Entity leg = scene.CreateChildEntity(bench, "Leg");
			leg.GetTransform().Translation = glm::vec3(x, 0.0f, 0.0f);
			leg.AddComponent<BoxColliderComponent>().HalfExtents = glm::vec3(0.2f, 0.4f, 0.2f);
		}
		scene.OnRuntimeStart();
		CollisionRecorder recorder(GetPhysics(scene));
		StepScene(scene, 60);
		CHECK(recorder.Count(CollisionEventType::Begin, bench.GetUUID(), ground.GetUUID()) == 1);
		CHECK(recorder.Count(CollisionEventType::End) == 0);
		CHECK(GetPhysics(scene).GetStats().ContactPairCount == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Rebuilding a touching body reports neither End nor Begin")
	{
		Scene scene;
		Entity ground = CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f));
		scene.OnRuntimeStart();
		CollisionRecorder recorder(GetPhysics(scene));
		StepScene(scene, 30);
		REQUIRE(recorder.Count(CollisionEventType::Begin, box.GetUUID(), ground.GetUUID()) == 1);
		const uint64_t builds = GetPhysics(scene).GetStats().BuildCount;

		// A property edit rebuilds the box (new shapes in box3d), and an edit of the ground rebuilds the ground.
		box.GetComponent<RigidBodyComponent>().Friction = 0.9f;
		box.MarkModified<RigidBodyComponent>();
		StepScene(scene, 5);
		ground.GetComponent<BoxColliderComponent>().HalfExtents.x += 1.0f;
		ground.MarkModified<BoxColliderComponent>();
		StepScene(scene, 5);
		CHECK(GetPhysics(scene).GetStats().BuildCount == builds + 2);
		CHECK(recorder.Count(CollisionEventType::Begin) == 1);
		CHECK(recorder.Count(CollisionEventType::End) == 0);
		CHECK(GetPhysics(scene).GetStats().ContactPairCount == 1);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Trigger events' points lie where the two overlap")
	{
		Scene scene;
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		Entity zone = CreateStaticTrigger(scene, "Zone", glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f));
		Entity ball = CreateDynamicSphere(scene, "Ball", glm::vec3(-3.0f, 1.5f, 0.0f), 0.25f);
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics);
		REQUIRE(physics.SetLinearVelocity(ball, glm::vec3(2.0f, 0.0f, 0.0f)));
		StepScene(scene, 60);
		const CollisionEvent* begin = recorder.Find(CollisionEventType::Begin, zone.GetUUID(), ball.GetUUID());
		REQUIRE(begin != nullptr);
		CHECK(begin->IsTrigger);
		// Inside the zone (x from -1 to 1, y from 0 to 2) and the ball's bounds (y from 1.25 to 1.75).
		CHECK(begin->Point.x >= -1.0f);
		CHECK(begin->Point.x <= 1.0f);
		CHECK(begin->Point.y >= 1.25f);
		CHECK(begin->Point.y <= 1.75f);
		CHECK(glm::length(begin->Normal) == doctest::Approx(1.0f));
		scene.OnRuntimeStop();
	}

	TEST_CASE("A watch begun while a body is inside a trigger reports Begin at the next step")
	{
		Scene scene;
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		Entity zone = CreateStaticTrigger(scene, "Zone", glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(1.0f));
		Entity resident = CreateDynamicBox(scene, "Resident", glm::vec3(0.5f, 1.0f, 0.5f), glm::vec3(0.2f));
		scene.OnRuntimeStart();
		ICollisionEventSource* contacts = scene.GetService<ICollisionEventSource>();
		REQUIRE(contacts != nullptr);
		// Nobody listens while the box enters: box3d reports its visit once, and it is dropped.
		StepScene(scene, 30);
		CHECK(GetPhysics(scene).GetStats().ContactPairCount == 0);

		std::vector<CollisionEvent> events;
		const CollisionListenerID listener = contacts->AddCollisionListener([&](const CollisionEvent& event) { events.push_back(event); },
			CollisionListenerScope::WatchedEntities);
		contacts->WatchCollisions(resident.GetUUID());
		StepScene(scene, 1);
		REQUIRE(events.size() == 1);
		CHECK(events[0].Type == CollisionEventType::Begin);
		CHECK(events[0].IsTrigger);
		CHECK(events[0].Involves(zone.GetUUID()));
		CHECK(contacts->RemoveCollisionListener(listener));
		scene.OnRuntimeStop();
	}
}

TEST_SUITE("Physics.MeshColliders")
{
	TEST_CASE("A convex collider of a round mesh is reduced, not refused")
	{
		// The built-in sphere's 1,225 points need a hull of more faces and edges than box3d's 128 at 128 vertices.
		ScopedMeshProvider provider(CreateMeshProvider());
		Scene scene;
		scene.GetSettings().Gravity = glm::vec3(0.0f);
		Entity ball = CreateMeshEntity(scene, "Ball", glm::vec3(0.0f), c_SphereMesh, true);
		ball.AddComponent<RigidBodyComponent>();
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(WaitUntil([&]()
		{
			StepScene(scene, 1);
			return physics.HasBody(ball);
		}, std::chrono::seconds(10)));
		CHECK(CountPhysicsWarnings(logStart) == 0);
		const std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(hit.has_value());
		CHECK(hit->HitEntity == ball);
		CHECK(std::abs(hit->Point.y - 0.5f) < 0.02f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A 1 cm box collider keeps its size")
	{
		Scene scene;
		CreateStaticBox(scene, "Pebble", glm::vec3(0.0f, 0.005f, 0.0f), glm::vec3(0.005f));
		scene.OnRuntimeStart();
		const std::optional<RaycastHit> hit = GetPhysics(scene).Raycast(glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 2.0f);
		REQUIRE(hit.has_value());
		CHECK(std::abs(hit->Point.y - 0.01f) < 1.0e-4f);
		const std::optional<RaycastHit> side = GetPhysics(scene).Raycast(glm::vec3(1.0f, 0.005f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), 2.0f);
		REQUIRE(side.has_value());
		CHECK(std::abs(side->Point.x - 0.005f) < 1.0e-4f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A large crate rests on a dense mesh floor")
	{
		// A 4 m crate on a floor of 10 cm squares lies over 3,200 of its triangles, more than box3d collides one convex shape
		// with at once (B3_MAX_MESH_CONTACT_TRIANGLES).
		ScopedMeshProvider provider(CreateMeshProvider());
		Scene scene;
		CreateMeshEntity(scene, "Floor", glm::vec3(0.0f), c_DenseFloorMesh, false);
		Entity crate = CreateDynamicBox(scene, "Crate", glm::vec3(0.0f, 2.2f, 0.0f), glm::vec3(2.0f));
		scene.OnRuntimeStart();
		REQUIRE(WaitUntil([&]()
		{
			StepScene(scene, 1);
			return GetPhysics(scene).GetStats().StaticBodyCount == 1;
		}, std::chrono::seconds(20)));
		StepScene(scene, 120);
		CHECK(std::abs(GetWorldPosition(scene, crate).y - 2.0f) < 0.03f);
		scene.OnRuntimeStop();
	}

	TEST_CASE("A triangle mesh collider away from its trigger body's origin is unusable, with a warning")
	{
		ScopedMeshProvider provider(CreateMeshProvider());
		Scene scene;
		Entity zone = scene.CreateEntity("Zone");
		RigidBodyComponent& body = zone.AddComponent<RigidBodyComponent>();
		body.Type = RigidBodyType::Static;
		body.IsTrigger = true;
		Entity plate = CreateMeshEntity(scene, "Plate", glm::vec3(0.0f), c_PlateMesh, false);
		scene.SetParent(plate, zone, false);
		plate.GetTransform().Translation = glm::vec3(1.0f, 0.0f, 0.0f);
		plate.MarkModified<TransformComponent>();
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		StepScene(scene, 2);
		CHECK(CountLogMessages(logStart, "a triangle mesh in a trigger must be at its body's origin") == 1);
		CHECK_FALSE(GetPhysics(scene).HasBody(zone));
		scene.OnRuntimeStop();
	}
}

TEST_SUITE("Physics.AssetMeshes")
{
	TEST_CASE("Unloading a mesh while a world uses its collider keeps the shape until the world stops")
	{
		Ref<const PhysicsMeshData> floor = CreateGridMesh(4, 2.0f);
		Ref<FunctionMeshProvider> provider = CreateRef<FunctionMeshProvider>([&floor](AssetHandle mesh) -> Ref<const PhysicsMeshData>
		{
			return static_cast<uint64_t>(mesh) == c_DenseFloorMesh ? floor : nullptr;
		});
		ScopedMeshProvider scopedProvider(provider);
		Scene scene;
		CreateMeshEntity(scene, "Floor", glm::vec3(0.0f), c_DenseFloorMesh, false);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 2.0f, 0.0f));
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(WaitUntil([&]()
		{
			StepScene(scene, 1);
			return physics.GetStats().StaticBodyCount == 1;
		}, std::chrono::seconds(10)));
		std::weak_ptr<const PhysicsCookedShape> shape;
		{
			const PhysicsMeshShapes::Result cooked = PhysicsMeshShapes::Request(floor, false);
			REQUIRE(cooked.CookState == PhysicsMeshShapes::State::Ready);
			shape = cooked.Shape;
		}

		// The mesh goes (unloaded on purpose), and the cache drops what it cooked of it at its next cook: the floor's body
		// keeps the shape box3d's mesh points to.
		floor.reset();
		provider->Changed();
		const Ref<const PhysicsMeshData> other = CreateGridMesh(2, 1.0f);
		REQUIRE(WaitUntil([&]() { return PhysicsMeshShapes::Request(other, false).CookState == PhysicsMeshShapes::State::Ready; }, std::chrono::seconds(10)));
		StepScene(scene, 90);
		CHECK_FALSE(shape.expired());
		CHECK(std::abs(GetWorldPosition(scene, box).y - 0.5f) < 0.03f);
		CHECK(physics.Raycast(glm::vec3(1.0f, 5.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f).has_value());
		scene.OnRuntimeStop();
		CHECK(shape.expired());
	}
}

TEST_SUITE("Physics.Lifecycle")
{
	TEST_CASE("A static floor rebuilt or created under a resting box keeps holding it")
	{
		Scene scene;
		Entity floor = CreateGround(scene);
		Entity box = CreateDynamicBox(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f));
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 120);
		REQUIRE(physics.IsSleeping(box));

		// The lowest the box gets over some steps: a floor that did not pair with it at once would let it sink until it moved
		// out of its bounds' margin.
		const auto lowestOver = [&](uint32_t steps)
		{
			float lowest = GetWorldPosition(scene, box).y;
			for (uint32_t step = 0; step < steps; step++)
			{
				StepScene(scene, 1);
				lowest = std::min(lowest, GetWorldPosition(scene, box).y);
			}
			return lowest;
		};

		// Rebuilt (an edit of its collider): its new shape must pair with the box that rests on it.
		floor.GetComponent<BoxColliderComponent>().HalfExtents.z += 1.0f;
		floor.MarkModified<BoxColliderComponent>();
		CHECK(lowestOver(60) > 0.49f);

		// Replaced: a new floor where the old one was, the old one destroyed.
		StepScene(scene, 120);
		Entity replacement = CreateGround(scene);
		StepScene(scene, 1);
		scene.DestroyEntity(floor);
		CHECK(lowestOver(60) > 0.49f);
		CHECK(physics.HasBody(replacement));
		CHECK(std::abs(GetWorldPosition(scene, box).y - 0.5f) < 0.03f);
		scene.OnRuntimeStop();
	}
}
