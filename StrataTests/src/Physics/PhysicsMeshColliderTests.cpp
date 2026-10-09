#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"

#include <unordered_map>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint64_t c_GroundMesh = 0x1001;
	constexpr uint64_t c_CubeMesh = 0x1002;
	constexpr uint64_t c_BrokenMesh = 0x1003;
	constexpr uint64_t c_TallMesh = 0x1004;
	constexpr uint64_t c_MissingMesh = 0x9999;

	// Square in the XZ plane at y = 0, 20 units wide, facing up (counter-clockwise seen from above).
	Ref<const PhysicsMeshData> CreateGroundMesh()
	{
		Ref<PhysicsMeshData> mesh = CreateRef<PhysicsMeshData>();
		mesh->Positions = { { -10.0f, 0.0f, -10.0f }, { -10.0f, 0.0f, 10.0f }, { 10.0f, 0.0f, 10.0f }, { 10.0f, 0.0f, -10.0f } };
		mesh->Indices = { 0, 1, 2, 0, 2, 3 };
		return mesh;
	}

	// Axis-aligned box centered on the origin, faces wound counter-clockwise seen from outside.
	Ref<const PhysicsMeshData> CreateBoxMesh(const glm::vec3& halfExtents)
	{
		Ref<PhysicsMeshData> mesh = CreateRef<PhysicsMeshData>();
		for (int index = 0; index < 8; index++)
			mesh->Positions.emplace_back((index & 1) ? halfExtents.x : -halfExtents.x, (index & 2) ? halfExtents.y : -halfExtents.y, (index & 4) ? halfExtents.z : -halfExtents.z);
		mesh->Indices = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
		return mesh;
	}

	// Fake asset system: serves the meshes above and counts requests.
	class FakeMeshAssets final : public PhysicsMeshProvider
	{
	public:
		FakeMeshAssets()
		{
			m_Meshes[c_GroundMesh] = CreateGroundMesh();
			m_Meshes[c_CubeMesh] = CreateBoxMesh(glm::vec3(0.5f));
			m_Meshes[c_TallMesh] = CreateBoxMesh(glm::vec3(0.5f, 1.5f, 0.5f));
			Ref<PhysicsMeshData> broken = CreateRef<PhysicsMeshData>();
			broken->Positions = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
			broken->Indices = { 0, 1, 7 };
			m_Meshes[c_BrokenMesh] = broken;
		}

		Ref<const PhysicsMeshData> GetMeshData(AssetHandle handle) override
		{
			m_Requests++;
			auto it = m_Meshes.find(static_cast<uint64_t>(handle));
			return it != m_Meshes.end() ? it->second : nullptr;
		}

		uint64_t GetVersion() override { return m_Version; }

		// Replaces a mesh's data (like a hot reload).
		void SetMesh(uint64_t handle, Ref<const PhysicsMeshData> mesh)
		{
			m_Meshes[handle] = std::move(mesh);
			m_Version++;
		}

		uint32_t GetRequestCount() const { return m_Requests; }
	private:
		std::unordered_map<uint64_t, Ref<const PhysicsMeshData>> m_Meshes;
		uint32_t m_Requests = 0;
		uint64_t m_Version = 0;
	};

	Entity CreateMeshEntity(Scene& scene, const std::string& name, const glm::vec3& position, uint64_t mesh, bool convex)
	{
		Entity entity = scene.CreateEntity(name);
		entity.GetTransform().Translation = position;
		MeshColliderComponent& collider = entity.AddComponent<MeshColliderComponent>();
		collider.Mesh = UUID(mesh);
		collider.Convex = convex;
		return entity;
	}

}

TEST_SUITE("Physics.MeshColliders")
{
	TEST_CASE("A static triangle mesh works as ground")
	{
		Ref<FakeMeshAssets> assets = CreateRef<FakeMeshAssets>();
		ScopedMeshProvider provider(assets);
		REQUIRE(&PhysicsWorld::GetMeshProvider() == assets.get());

		Scene scene;
		Entity ground = CreateMeshEntity(scene, "Terrain", glm::vec3(0.0f), c_GroundMesh, false);
		Entity ball = CreateDynamicSphere(scene, "Ball", glm::vec3(2.0f, 3.0f, -1.0f), 0.5f);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(physics.HasBody(ground));

		std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(5.0f, 5.0f, 5.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == ground);
		CHECK(hit->Point.y == doctest::Approx(0.0f).epsilon(1.0e-4));
		CHECK(Math::IsNearlyEqual(hit->Normal, glm::vec3(0.0f, 1.0f, 0.0f), 1.0e-4f));
		CHECK_FALSE(physics.Raycast(glm::vec3(15.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f));

		StepScene(scene, 150);
		CHECK(std::abs(GetWorldPosition(scene, ball).y - 0.5f) < 0.03f);
	}

	TEST_CASE("Convex mesh colliders simulate as dynamic bodies and share their shape")
	{
		Ref<FakeMeshAssets> assets = CreateRef<FakeMeshAssets>();
		ScopedMeshProvider provider(assets);

		Scene scene;
		CreateGround(scene);
		Entity first = CreateMeshEntity(scene, "First", glm::vec3(0.0f, 2.0f, 0.0f), c_CubeMesh, true);
		first.AddComponent<RigidBodyComponent>();
		Entity second = CreateMeshEntity(scene, "Second", glm::vec3(3.0f, 2.0f, 0.0f), c_CubeMesh, true);
		second.AddComponent<RigidBodyComponent>();
		second.GetTransform().Scale = glm::vec3(2.0f, 1.0f, 1.0f); // Scaled hulls stay exact

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(physics.HasBody(first));
		REQUIRE(physics.HasBody(second));
		CHECK(assets->GetRequestCount() == 2);

		StepScene(scene, 150);
		CHECK(std::abs(GetWorldPosition(scene, first).y - 0.5f) < 0.03f);
		CHECK(std::abs(GetWorldPosition(scene, second).y - 0.5f) < 0.03f);
		std::optional<RaycastHit> side = physics.Raycast(glm::vec3(10.0f, 0.5f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), 10.0f);
		REQUIRE(side);
		CHECK(side->HitEntity == second);
		CHECK(side->Point.x == doctest::Approx(GetWorldPosition(scene, second).x + 1.0f).epsilon(0.01));
	}

	TEST_CASE("The Mesh Renderer's mesh is used when the collider has none")
	{
		Ref<FakeMeshAssets> assets = CreateRef<FakeMeshAssets>();
		ScopedMeshProvider provider(assets);

		Scene scene;
		Entity rock = CreateMeshEntity(scene, "Rock", glm::vec3(0.0f, 1.0f, 0.0f), 0, true);
		rock.AddComponent<MeshRendererComponent>().Mesh = UUID(c_CubeMesh);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == rock);
		CHECK(hit->Point.y == doctest::Approx(1.5f).epsilon(1.0e-3));

		// Changing the renderer's mesh rebuilds the collider.
		rock.GetComponent<MeshRendererComponent>().Mesh = UUID(c_TallMesh);
		rock.MarkModified<MeshRendererComponent>();
		hit = physics.Raycast(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == rock);
		CHECK(hit->Point.y == doctest::Approx(2.5f).epsilon(1.0e-3));

		// Triangle meshes work on static bodies, and the entity's scale stretches them.
		Entity terrain = CreateMeshEntity(scene, "Terrain", glm::vec3(0.0f, -1.0f, 0.0f), c_GroundMesh, false);
		terrain.GetTransform().Scale = glm::vec3(2.0f, 1.0f, 2.0f);
		hit = physics.Raycast(glm::vec3(15.0f, 5.0f, 15.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == terrain);
		CHECK(hit->Point.y == doctest::Approx(-1.0f).epsilon(1.0e-3));
		CHECK_FALSE(physics.Raycast(glm::vec3(21.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f));
	}

	TEST_CASE("Bodies follow mesh data changes the provider reports")
	{
		Ref<FakeMeshAssets> assets = CreateRef<FakeMeshAssets>();
		ScopedMeshProvider provider(assets);

		Scene scene;
		Entity rock = CreateMeshEntity(scene, "Rock", glm::vec3(0.0f), c_CubeMesh, true);
		Entity other = CreateMeshEntity(scene, "Other", glm::vec3(5.0f, 0.0f, 0.0f), c_GroundMesh, false);
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		const auto topOf = [&](float x)
		{
			std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(x, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
			return hit ? hit->Point.y : -1.0f;
		};
		CHECK(topOf(0.0f) == doctest::Approx(0.5f).epsilon(1.0e-3));

		// Steps without reported changes do not ask for mesh data again.
		StepScene(scene, 1);
		const uint32_t requests = assets->GetRequestCount();
		StepScene(scene, 5);
		CHECK(assets->GetRequestCount() == requests);

		// New data for a mesh (a hot reload) rebuilds the colliders using it, and only those.
		assets->SetMesh(c_CubeMesh, CreateBoxMesh(glm::vec3(0.5f, 2.0f, 0.5f)));
		StepScene(scene, 1);
		CHECK(physics.HasBody(rock));
		CHECK(topOf(0.0f) == doctest::Approx(2.0f).epsilon(1.0e-3));
		CHECK(physics.HasBody(other));
		CHECK(topOf(5.0f) == doctest::Approx(0.0f).epsilon(1.0e-4));

		// Data that becomes unavailable (an unloaded mesh) leaves the body as it is.
		assets->SetMesh(c_CubeMesh, nullptr);
		StepScene(scene, 1);
		CHECK(physics.HasBody(rock));
		CHECK(topOf(0.0f) == doctest::Approx(2.0f).epsilon(1.0e-3));
	}

	TEST_CASE("Unusable mesh colliders are skipped with a warning")
	{
		Ref<FakeMeshAssets> assets = CreateRef<FakeMeshAssets>();
		Scene scene;
		Entity dynamicTerrain = CreateMeshEntity(scene, "DynamicTerrain", glm::vec3(0.0f), c_GroundMesh, false);
		dynamicTerrain.AddComponent<RigidBodyComponent>();
		Entity missing = CreateMeshEntity(scene, "Missing", glm::vec3(5.0f, 0.0f, 0.0f), c_MissingMesh, true);
		Entity broken = CreateMeshEntity(scene, "Broken", glm::vec3(10.0f, 0.0f, 0.0f), c_BrokenMesh, false);
		Entity unset = CreateMeshEntity(scene, "Unset", glm::vec3(15.0f, 0.0f, 0.0f), 0, true);
		// Other colliders of the entity still work.
		Entity mixed = CreateMeshEntity(scene, "Mixed", glm::vec3(20.0f, 0.0f, 0.0f), c_MissingMesh, true);
		mixed.AddComponent<SphereColliderComponent>();

		{
			// The default provider reads the active asset manager: without one, mesh colliders wait.
			REQUIRE_FALSE(AssetManager::HasActive());
			const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
			Scene withoutAssets;
			Entity orphan = CreateMeshEntity(withoutAssets, "Orphan", glm::vec3(0.0f), c_CubeMesh, true);
			withoutAssets.OnRuntimeStart();
			StepScene(withoutAssets, 2);
			CHECK_FALSE(GetPhysics(withoutAssets).HasBody(orphan));
			CHECK(GetPhysics(withoutAssets).GetStats().PendingBodyCount == 1);
			CHECK(CountLogMessages(logStart, "'Orphan' waits for mesh") == 1);
		}

		ScopedMeshProvider provider(assets);
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK_FALSE(physics.HasBody(dynamicTerrain));
		CHECK_FALSE(physics.HasBody(missing));
		CHECK_FALSE(physics.HasBody(broken));
		CHECK_FALSE(physics.HasBody(unset));
		CHECK(physics.HasBody(mixed));
		CHECK(CountLogMessages(logStart, "triangle meshes are only supported on static bodies") == 1);
		CHECK(CountLogMessages(logStart, "is not available") == 2);
		CHECK(CountLogMessages(logStart, "'Broken'") >= 1);
		CHECK(CountLogMessages(logStart, "has no mesh") == 1);
	}
}
