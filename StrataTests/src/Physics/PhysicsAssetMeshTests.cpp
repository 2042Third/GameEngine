#include <doctest/doctest.h>

#include "Physics/PhysicsTestUtils.h"
#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/EditorAssetManager.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Physics/AssetMeshProvider.h"
#include "Strata/Physics/PhysicsMeshShapes.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "TestHelpers.h"

#include <atomic>
#include <chrono>
#include <limits>
#include <optional>
#include <thread>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A project whose glTF model is a square floor in the XZ plane at y = 0 (two triangles facing up), imported by an
	// editor asset manager.
	struct FloorProject
	{
		std::filesystem::path Models;
		Ref<EditorAssetManager> Manager;
		AssetHandle Model = UUID::Null();
		AssetHandle FloorMesh = UUID::Null();

		explicit FloorProject(float halfSize)
		{
			const std::filesystem::path root = CreateTemporaryDirectory("PhysicsFloorProject");
			Models = root / "Assets" / "Models";
			REQUIRE(FileSystem::CreateDirectories(Models));
			WriteFloor(halfSize);
			const nlohmann::json gltf = {
				{ "asset", { { "version", "2.0" } } },
				{ "scenes", nlohmann::json::array({ { { "nodes", nlohmann::json::array({ 0 }) } } }) },
				{ "nodes", nlohmann::json::array({ { { "mesh", 0 } } }) },
				{ "meshes", nlohmann::json::array({ { { "primitives", nlohmann::json::array({ { { "attributes", { { "POSITION", 0 } } } } }) } } }) },
				{ "buffers", nlohmann::json::array({ { { "uri", "Floor.bin" }, { "byteLength", 72 } } }) },
				{ "bufferViews", nlohmann::json::array({ { { "buffer", 0 }, { "byteLength", 72 } } }) },
				{ "accessors", nlohmann::json::array({ { { "bufferView", 0 }, { "componentType", 5126 }, { "count", 6 }, { "type", "VEC3" } } }) }
			};
			const std::string text = JsonUtils::Dump(gltf);
			REQUIRE(FileSystem::WriteBytes(Models / "Floor.gltf", std::vector<uint8_t>(text.begin(), text.end())));

			EditorAssetManagerSpecification specification;
			specification.AssetDirectory = root / "Assets";
			specification.CacheDirectory = root / ".strata" / "Cache";
			specification.WatchFiles = false;
			Manager = CreateRef<EditorAssetManager>(specification);
			Manager->Scan();
			Model = Manager->FindAssetByPath("Models/Floor.gltf");
			REQUIRE(Model.IsValid());
			FloorMesh = DeriveSubAssetHandle(Model, "Mesh/0");
			REQUIRE(Manager->GetAssetType(FloorMesh) == AssetType::Mesh);
		}

		void WriteFloor(float halfSize) const
		{
			const float s = halfSize;
			const std::vector<float> positions = { -s, 0.0f, -s, -s, 0.0f, s, s, 0.0f, s, -s, 0.0f, -s, s, 0.0f, s, s, 0.0f, -s };
			const uint8_t* bytes = reinterpret_cast<const uint8_t*>(positions.data());
			REQUIRE(FileSystem::WriteBytes(Models / "Floor.bin", std::vector<uint8_t>(bytes, bytes + positions.size() * sizeof(float))));
		}
	};

	// Box corners with the triangles of its faces (counter-clockwise seen from outside), usable as hull and as triangle mesh.
	Ref<const PhysicsMeshData> CreateBoxMesh(const glm::vec3& halfExtents)
	{
		Ref<PhysicsMeshData> mesh = CreateRef<PhysicsMeshData>();
		for (int index = 0; index < 8; index++)
			mesh->Positions.emplace_back((index & 1) ? halfExtents.x : -halfExtents.x, (index & 2) ? halfExtents.y : -halfExtents.y, (index & 4) ? halfExtents.z : -halfExtents.z);
		mesh->Indices = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
		return mesh;
	}

	// A static slab made of a triangle mesh and a dynamic box made of a convex hull above it, both using the same data.
	void CreateMeshBodies(Scene& scene, AssetHandle mesh)
	{
		Entity slab = scene.CreateEntity("Slab");
		slab.GetTransform().Scale = glm::vec3(10.0f, 1.0f, 10.0f);
		slab.GetTransform().Translation = glm::vec3(0.0f, -0.5f, 0.0f);
		MeshColliderComponent& slabCollider = slab.AddComponent<MeshColliderComponent>();
		slabCollider.Mesh = mesh;
		slabCollider.Convex = false;

		Entity rock = scene.CreateEntity("Rock");
		rock.GetTransform().Translation = glm::vec3(0.0f, 2.0f, 0.0f);
		rock.AddComponent<RigidBodyComponent>();
		rock.AddComponent<MeshColliderComponent>().Mesh = mesh;
	}

	// Keeps every JobSystem worker busy until released, so that jobs submitted meanwhile (cooks) stay queued.
	class WorkerBlocker
	{
	public:
		WorkerBlocker()
		{
			for (uint32_t index = 0; index < JobSystem::GetWorkerThreadCount(); index++)
			{
				m_Jobs.push_back(JobSystem::Submit([this]()
				{
					m_Blocked++;
					while (!m_Release)
						std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}));
			}
			while (m_Blocked < JobSystem::GetWorkerThreadCount())
				std::this_thread::yield();
		}

		~WorkerBlocker()
		{
			Release();
		}

		WorkerBlocker(const WorkerBlocker&) = delete;
		WorkerBlocker& operator=(const WorkerBlocker&) = delete;

		void Release()
		{
			m_Release = true;
			JobSystem::WaitAll(m_Jobs);
			m_Jobs.clear();
		}
	private:
		std::atomic<bool> m_Release = false;
		std::atomic<uint32_t> m_Blocked = 0;
		std::vector<JobHandle> m_Jobs;
	};

}

TEST_SUITE("Physics.AssetMeshes")
{
	TEST_CASE("The asset mesh provider serves the meshes of the active asset manager")
	{
		AssetMeshProvider provider;
		REQUIRE_FALSE(AssetManager::HasActive());
		CHECK_FALSE(provider.GetMeshData(BuiltinAssets::CubeMesh));

		FloorProject project(5.0f);
		ScopedActiveAssetManager active(project.Manager);
		const uint64_t version = provider.GetVersion();

		// Built-in meshes are loaded in every asset manager: their data is there right away, the same object every time.
		const Ref<const PhysicsMeshData> cube = provider.GetMeshData(BuiltinAssets::CubeMesh);
		REQUIRE(cube);
		const Ref<Mesh> cubeMesh = AssetManager::GetAsset<Mesh>(BuiltinAssets::CubeMesh);
		REQUIRE(cubeMesh);
		CHECK(cube->Positions == cubeMesh->GetPositions());
		CHECK(cube->Indices.size() == cubeMesh->GetTriangleCount() * 3);
		CHECK(provider.GetMeshData(BuiltinAssets::CubeMesh) == cube);
		CHECK_FALSE(provider.GetMeshData(BuiltinAssets::DefaultMaterial)); // Not a mesh
		CHECK_FALSE(provider.GetMeshData(UUID(0x12345678)));               // Unknown

		// Imported meshes are loaded in the background: unavailable (never blocking) until the asset manager is done.
		CHECK_FALSE(provider.GetMeshData(project.FloorMesh));
		CHECK(project.Manager->GetAssetState(project.FloorMesh) != AssetState::Unloaded); // The load was requested
		CHECK(provider.GetVersion() == version);
		REQUIRE(project.Manager->WaitForPendingLoads());
		CHECK(provider.GetVersion() != version);
		const Ref<const PhysicsMeshData> floor = provider.GetMeshData(project.FloorMesh);
		REQUIRE(floor);
		CHECK(floor->Indices.size() == 6);
		CHECK(provider.GetMeshData(project.FloorMesh) == floor);
		CHECK(provider.GetCachedMeshCount() == 2);

		// A reloaded mesh is a new object and gets new data; unloaded meshes' data is dropped.
		REQUIRE(project.Manager->ReimportAsset(project.Model));
		REQUIRE(project.Manager->WaitForPendingLoads());
		const Ref<const PhysicsMeshData> reloaded = provider.GetMeshData(project.FloorMesh);
		REQUIRE(reloaded);
		CHECK(reloaded != floor);
		project.Manager->UnloadAsset(project.FloorMesh);
		provider.GetVersion();
		CHECK(provider.GetCachedMeshCount() == 1);

		// Without an active asset manager, nothing is available.
		const uint64_t lastVersion = provider.GetVersion();
		AssetManager::SetActive(nullptr);
		CHECK(provider.GetVersion() != lastVersion);
		CHECK_FALSE(provider.GetMeshData(BuiltinAssets::CubeMesh));
	}

	TEST_CASE("Mesh data covers every submesh at full detail")
	{
		// Two triangles in separate submeshes (indices relative to each submesh's first vertex), the first with a second
		// level of detail that must not be used.
		std::vector<glm::vec3> positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 5, 0, 0 }, { 6, 0, 0 }, { 5, 1, 0 } };
		std::vector<MeshVertexAttributes> attributes(positions.size());
		std::vector<uint32_t> indices = { 0, 1, 2, 0, 1, 2, 2, 1, 0 };
		std::vector<Submesh> submeshes(2);
		submeshes[0].VertexCount = 3;
		submeshes[0].LODs = { MeshLOD { 0, 3 }, MeshLOD { 6, 3 } };
		submeshes[1].BaseVertex = 3;
		submeshes[1].VertexCount = 3;
		submeshes[1].LODs = { MeshLOD { 3, 3 } };
		std::string error;
		const Ref<Mesh> mesh = Mesh::Create(positions, attributes, indices, submeshes, &error);
		REQUIRE_MESSAGE(mesh, error);

		FloorProject project(5.0f);
		ScopedActiveAssetManager active(project.Manager);
		AssetMetadata metadata;
		metadata.Name = "TwoTriangles";
		const AssetHandle handle = project.Manager->AddMemoryAsset(mesh, metadata);

		AssetMeshProvider provider;
		const Ref<const PhysicsMeshData> data = provider.GetMeshData(handle);
		REQUIRE(data);
		CHECK(data->Positions == positions);
		CHECK(data->Indices == std::vector<uint32_t> { 0, 1, 2, 3, 4, 5 });
	}

	TEST_CASE("Mesh colliders on built-in and imported meshes become bodies and follow reloads")
	{
		FloorProject project(5.0f);
		ScopedActiveAssetManager active(project.Manager);

		Scene scene;
		Entity floor = scene.CreateEntity("Floor");
		MeshColliderComponent& floorCollider = floor.AddComponent<MeshColliderComponent>();
		floorCollider.Mesh = project.FloorMesh;
		floorCollider.Convex = false;
		Entity crate = scene.CreateEntity("Crate");
		crate.GetTransform().Translation = glm::vec3(1.0f, 2.0f, 1.0f);
		crate.AddComponent<RigidBodyComponent>();
		crate.AddComponent<MeshColliderComponent>().Mesh = BuiltinAssets::CubeMesh;
		// A mesh collider without a mesh of its own uses the renderer's.
		Entity ball = scene.CreateEntity("Ball");
		ball.GetTransform().Translation = glm::vec3(20.0f, 0.0f, 0.0f);
		ball.AddComponent<MeshRendererComponent>().Mesh = BuiltinAssets::SphereMesh;
		ball.AddComponent<MeshColliderComponent>();

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK(physics.HasBody(crate)); // Built-in meshes are always loaded
		CHECK(physics.HasBody(ball));
		// The imported mesh was requested but is still loading: its collider waits, nothing blocks.
		CHECK_FALSE(physics.HasBody(floor));
		CHECK(physics.GetStats().PendingBodyCount == 1);
		StepScene(scene, 10);
		CHECK_FALSE(physics.HasBody(floor));
		// The simulation waits for the floor to start: the crate stays where it is.
		CHECK(physics.GetStats().WaitingForMeshes);
		CHECK(physics.GetStats().StepCount == 0);
		CHECK(GetWorldPosition(scene, crate) == glm::vec3(1.0f, 2.0f, 1.0f));

		// Once the asset manager finished loading it, the next step builds the body and starts the simulation.
		REQUIRE(project.Manager->WaitForPendingLoads());
		StepScene(scene, 1);
		REQUIRE(physics.HasBody(floor));
		CHECK_FALSE(physics.GetStats().WaitingForMeshes);
		CHECK(physics.GetStats().StepCount == 1);
		std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(4.0f, 5.0f, -4.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == floor);
		CHECK(hit->Point.y == doctest::Approx(0.0f).epsilon(1.0e-4));
		CHECK_FALSE(physics.Raycast(glm::vec3(7.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f));
		hit = physics.Raycast(glm::vec3(20.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == ball);
		CHECK(hit->Point.y == doctest::Approx(0.5f).epsilon(0.02));

		StepScene(scene, 120);
		CHECK(std::abs(GetWorldPosition(scene, crate).y - 0.5f) < 0.03f);

		// Hot reload: a larger floor replaces the mesh, and the collider follows at the next step.
		project.WriteFloor(10.0f);
		REQUIRE(project.Manager->ReimportAsset(project.Model));
		REQUIRE(project.Manager->WaitForPendingLoads());
		StepScene(scene, 1);
		hit = physics.Raycast(glm::vec3(7.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == floor);
		StepScene(scene, 30);
		CHECK(std::abs(GetWorldPosition(scene, crate).y - 0.5f) < 0.03f);
	}

	TEST_CASE("Mesh changes are checked only for the colliders using the changed meshes")
	{
		FloorProject project(5.0f);
		ScopedActiveAssetManager active(project.Manager);

		Scene scene;
		Entity floor = scene.CreateEntity("Floor");
		MeshColliderComponent& floorCollider = floor.AddComponent<MeshColliderComponent>();
		floorCollider.Mesh = project.FloorMesh;
		floorCollider.Convex = false;
		for (int index = 0; index < 20; index++)
		{
			Entity block = scene.CreateEntity("Block");
			block.GetTransform().Translation = glm::vec3(20.0f + 2.0f * static_cast<float>(index), 0.5f, 0.0f);
			block.AddComponent<MeshColliderComponent>().Mesh = BuiltinAssets::CubeMesh;
		}

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(project.Manager->WaitForPendingLoads());
		StepScene(scene, 2);
		REQUIRE(physics.HasBody(floor));
		uint64_t checks = physics.GetStats().MeshCheckCount;
		const auto floorHitAt = [&](float x)
		{
			std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(x, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
			return hit && hit->HitEntity == floor;
		};

		// Other asset changes (e.g. textures streaming in) check no mesh collider.
		for (int index = 0; index < 10; index++)
		{
			AssetMetadata metadata;
			metadata.Name = "Runtime";
			project.Manager->AddMemoryAsset(Material::Create(), metadata);
			StepScene(scene, 1);
		}
		CHECK(physics.GetStats().MeshCheckCount == checks);

		// A reloaded mesh checks, and rebuilds, only the collider using it.
		const uint64_t builds = physics.GetStats().BuildCount;
		CHECK_FALSE(floorHitAt(7.0f));
		project.WriteFloor(10.0f);
		REQUIRE(project.Manager->ReimportAsset(project.Model));
		REQUIRE(project.Manager->WaitForPendingLoads());
		StepScene(scene, 1);
		CHECK(physics.GetStats().MeshCheckCount == checks + 1);
		CHECK(physics.GetStats().BuildCount == builds + 1);
		CHECK(floorHitAt(7.0f));

		// Cooks finishing for another world check nothing here.
		checks = physics.GetStats().MeshCheckCount;
		{
			Scene other;
			Entity ball = other.CreateEntity("Ball");
			MeshColliderComponent& ballCollider = ball.AddComponent<MeshColliderComponent>();
			ballCollider.Mesh = BuiltinAssets::SphereMesh;
			ballCollider.Convex = false;
			other.OnRuntimeStart();
			CHECK(GetPhysics(other).HasBody(ball));
		}
		StepScene(scene, 1);
		CHECK(physics.GetStats().MeshCheckCount == checks);

		// A mesh unloaded on purpose is not loaded again, and its collider keeps its shape.
		project.Manager->UnloadAsset(project.FloorMesh);
		StepScene(scene, 1);
		CHECK(project.Manager->GetAssetState(project.FloorMesh) == AssetState::Unloaded);
		CHECK(physics.HasBody(floor));
		CHECK(floorHitAt(7.0f));
	}

	TEST_CASE("Cooked mesh shapes are reused by later worlds and dropped with their data")
	{
		Ref<const PhysicsMeshData> box = CreateBoxMesh(glm::vec3(0.5f));
		Ref<FunctionMeshProvider> meshes = CreateRef<FunctionMeshProvider>([&](AssetHandle) { return box; });
		ScopedMeshProvider provider(meshes);
		const AssetHandle mesh = UUID(0x7001);

		const uint64_t cooksBefore = PhysicsMeshShapes::GetCookCount();
		{
			Scene scene;
			CreateMeshBodies(scene, mesh);
			scene.OnRuntimeStart();
			CHECK(GetPhysics(scene).GetStats().BodyCount == 2);
			CHECK(PhysicsMeshShapes::GetCookCount() == cooksBefore + 2); // A triangle mesh and a hull
		}

		// Running again (like entering Play mode once more) restores the cooked shapes instead of cooking again.
		const size_t cachedBefore = PhysicsMeshShapes::GetCachedCount();
		{
			Scene scene;
			CreateMeshBodies(scene, mesh);
			scene.OnRuntimeStart();
			PhysicsSystem& physics = GetPhysics(scene);
			CHECK(physics.GetStats().BodyCount == 2);
			CHECK(PhysicsMeshShapes::GetCookCount() == cooksBefore + 2);
			StepScene(scene, 90);
			CHECK(std::abs(GetWorldPosition(scene, scene.FindEntityByName("Rock")).y - 0.5f) < 0.03f);
		}

		// New data (a reloaded mesh) is cooked, and the shapes of the destroyed data are dropped.
		box = CreateBoxMesh(glm::vec3(0.5f));
		meshes->Changed();
		{
			Scene scene;
			CreateMeshBodies(scene, mesh);
			scene.OnRuntimeStart();
			CHECK(GetPhysics(scene).GetStats().BodyCount == 2);
			CHECK(PhysicsMeshShapes::GetCookCount() == cooksBefore + 4);
			CHECK(PhysicsMeshShapes::GetCachedCount() <= cachedBefore);
		}
	}

	TEST_CASE("Mesh shapes cook on the job system while their bodies wait")
	{
		ScopedJobSystem jobSystem(2);
		// New data, so that it has not been cooked before.
		const Ref<const PhysicsMeshData> box = CreateBoxMesh(glm::vec3(0.5f));
		ScopedMeshProvider provider(CreateRef<FunctionMeshProvider>([&](AssetHandle) { return box; }));

		// While every worker is busy the cook stays queued.
		WorkerBlocker blocker;

		Scene scene;
		CreateGround(scene);
		Entity rock = scene.CreateEntity("Rock");
		rock.GetTransform().Translation = glm::vec3(0.0f, 2.0f, 0.0f);
		rock.AddComponent<RigidBodyComponent>();
		rock.AddComponent<MeshColliderComponent>().Mesh = UUID(0x7002);

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CHECK_FALSE(physics.HasBody(rock));
		CHECK(physics.GetStats().PendingBodyCount == 1);
		StepScene(scene, 5); // The scene keeps running, the simulation waits for the cook to start
		CHECK_FALSE(physics.HasBody(rock));
		CHECK(physics.GetStats().StepCount == 0);
		CHECK(physics.GetStats().HeldStepCount == 5);

		blocker.Release();
		CHECK(WaitUntil([&]()
		{
			StepScene(scene, 1);
			return physics.HasBody(rock);
		}, std::chrono::milliseconds(10000)));
		StepScene(scene, 120);
		CHECK(std::abs(GetWorldPosition(scene, rock).y - 0.5f) < 0.03f);
	}

	TEST_CASE("The simulation starts once the mesh floors are cooked")
	{
		ScopedJobSystem jobSystem(2);
		const Ref<const PhysicsMeshData> slab = CreateBoxMesh(glm::vec3(5.0f, 0.5f, 5.0f));
		ScopedMeshProvider provider(CreateRef<FunctionMeshProvider>([&](AssetHandle) { return slab; }));
		WorkerBlocker blocker;

		Scene scene;
		Entity floor = scene.CreateEntity("Floor");
		floor.GetTransform().Translation = glm::vec3(0.0f, -0.5f, 0.0f);
		MeshColliderComponent& floorCollider = floor.AddComponent<MeshColliderComponent>();
		floorCollider.Mesh = UUID(0x7003);
		floorCollider.Convex = false;
		Entity crate = CreateDynamicBox(scene, "Crate", glm::vec3(0.0f, 1.0f, 0.0f));
		Entity spinner = CreateDynamicBox(scene, "Spinner", glm::vec3(20.0f, 5.0f, 0.0f));

		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		CollisionRecorder recorder(physics);
		REQUIRE(physics.HasBody(crate));
		CHECK_FALSE(physics.HasBody(floor));

		// While the floor cooks, the scene runs but nothing moves (half a second, during which the crate would fall through).
		StepScene(scene, 30);
		CHECK_FALSE(physics.HasBody(floor));
		CHECK(physics.GetStats().WaitingForMeshes);
		CHECK(physics.GetStats().StepCount == 0);
		CHECK(physics.GetStats().HeldStepCount == 30);
		CHECK(GetWorldPosition(scene, crate) == glm::vec3(0.0f, 1.0f, 0.0f));
		CHECK(GetWorldPosition(scene, spinner) == glm::vec3(20.0f, 5.0f, 0.0f));
		CHECK(recorder.GetEvents().empty());
		// Changes made meanwhile still apply, and queries see the bodies.
		CHECK(physics.SetLinearVelocity(spinner, glm::vec3(0.0f, 0.0f, 1.0f)));
		const std::optional<RaycastHit> hit = physics.Raycast(glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);
		REQUIRE(hit);
		CHECK(hit->HitEntity == crate);

		// The step after the cook finished builds the floor and starts the simulation, with the crate on top of it.
		const uint64_t completedCooks = PhysicsMeshShapes::GetCompletedCount();
		blocker.Release();
		REQUIRE(WaitUntil([&]() { return PhysicsMeshShapes::GetCompletedCount() != completedCooks; }, std::chrono::milliseconds(10000)));
		StepScene(scene, 1);
		CHECK(physics.HasBody(floor));
		CHECK_FALSE(physics.GetStats().WaitingForMeshes);
		CHECK(physics.GetStats().StepCount == 1);
		StepScene(scene, 90);
		CHECK(std::abs(GetWorldPosition(scene, crate).y - 0.5f) < 0.03f);
		CHECK(GetWorldPosition(scene, spinner).z > 1.0f);
		CHECK(physics.GetStats().HeldStepCount == 30);
	}

	TEST_CASE("The start waits for loading meshes at most MeshWaitTimeout")
	{
		PhysicsSettings settings;
		settings.MeshWaitTimeout = 0.5f;
		ScopedPhysicsSettings scopedSettings(settings);
		Ref<FunctionMeshProvider> meshes = CreateRef<FunctionMeshProvider>([](AssetHandle) { return Ref<const PhysicsMeshData>(); });
		meshes->SetLoading(true); // Never arrives
		ScopedMeshProvider provider(meshes);

		Scene scene;
		REQUIRE(scene.GetSettings().FixedTimestep == doctest::Approx(1.0f / 60.0f));
		CreateGround(scene);
		Entity crate = CreateDynamicBox(scene, "Crate", glm::vec3(0.0f, 3.0f, 0.0f));
		Entity rock = scene.CreateEntity("Rock");
		rock.AddComponent<MeshColliderComponent>().Mesh = UUID(0x7004);

		const uint64_t logSequence = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();
		PhysicsSystem& physics = GetPhysics(scene);
		StepScene(scene, 28);
		CHECK(physics.GetStats().WaitingForMeshes);
		CHECK(physics.GetStats().StepCount == 0);
		CHECK(GetWorldPosition(scene, crate).y == 3.0f);
		CHECK(CountLogMessages(logSequence, "starts simulating") == 0);

		// After half a second (30 steps), the simulation starts without the mesh collider, warning once.
		StepScene(scene, 4);
		CHECK_FALSE(physics.GetStats().WaitingForMeshes);
		CHECK(physics.GetStats().HeldStepCount >= 29);
		CHECK(physics.GetStats().HeldStepCount <= 31);
		CHECK(physics.GetStats().StepCount + physics.GetStats().HeldStepCount == 32);
		CHECK(CountLogMessages(logSequence, "starts simulating after waiting 0.5 s for meshes, without the mesh colliders of 'Rock'") == 1);
		StepScene(scene, 30);
		CHECK(GetWorldPosition(scene, crate).y < 2.5f);
		CHECK_FALSE(physics.HasBody(rock));
		CHECK(CountLogMessages(logSequence, "starts simulating") == 1);

		// Mesh colliders added once the simulation runs are not waited for.
		const uint64_t steps = physics.GetStats().StepCount;
		Entity late = scene.CreateEntity("Late");
		late.AddComponent<MeshColliderComponent>().Mesh = UUID(0x7005);
		StepScene(scene, 1);
		CHECK(physics.GetStats().StepCount == steps + 1);
		CHECK_FALSE(physics.GetStats().WaitingForMeshes);
	}

	TEST_CASE("The start does not wait for meshes that are not loading, inactive colliders or without a timeout")
	{
		Ref<FunctionMeshProvider> meshes = CreateRef<FunctionMeshProvider>([](AssetHandle) { return Ref<const PhysicsMeshData>(); });
		ScopedMeshProvider provider(meshes);
		PhysicsSettings settings;
		float expectedTimeout = settings.MeshWaitTimeout;

		Scene scene;
		CreateGround(scene);
		Entity crate = CreateDynamicBox(scene, "Crate", glm::vec3(0.0f, 3.0f, 0.0f));
		Entity rock = scene.CreateEntity("Rock");
		rock.AddComponent<MeshColliderComponent>().Mesh = UUID(0x7006);

		SUBCASE("Unknown or failed meshes")
		{
			meshes->SetLoading(false);
		}
		SUBCASE("Inactive mesh colliders")
		{
			meshes->SetLoading(true);
			rock.SetActive(false);
		}
		SUBCASE("No wait")
		{
			meshes->SetLoading(true);
			settings.MeshWaitTimeout = 0.0f;
			expectedTimeout = 0.0f;
		}
		SUBCASE("Invalid wait (sanitized to none)")
		{
			meshes->SetLoading(true);
			settings.MeshWaitTimeout = std::numeric_limits<float>::quiet_NaN();
			expectedTimeout = 0.0f;
		}
		ScopedPhysicsSettings scopedSettings(settings);
		const uint64_t logSequence = Log::GetBuffer().GetLatestSequence();
		scene.OnRuntimeStart();

		PhysicsSystem& physics = GetPhysics(scene);
		REQUIRE(physics.GetWorld() != nullptr);
		CHECK(physics.GetWorld()->GetSettings().MeshWaitTimeout == expectedTimeout);
		StepScene(scene, 1);
		CHECK(physics.GetStats().StepCount == 1);
		CHECK(physics.GetStats().HeldStepCount == 0);
		CHECK(GetWorldPosition(scene, crate).y < 3.0f);
		CHECK(CountLogMessages(logSequence, "starts simulating") == 0);
	}
}
