#include <doctest/doctest.h>

#include "Scene/SceneTestUtils.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	void CheckProxies(const RenderScene& renderScene)
	{
		std::string error;
		CHECK_MESSAGE(renderScene.Validate(&error), error);
	}

	uint32_t FindMeshSlot(const RenderScene& renderScene, AssetHandle mesh)
	{
		for (uint32_t slot = 0; slot < renderScene.GetMeshSlotCapacity(); slot++)
		{
			if (renderScene.GetMeshHandle(slot) == mesh)
				return slot;
		}
		return RenderScene::c_InvalidIndex;
	}

	Entity AddCube(Scene& scene, const glm::vec3& position, AssetHandle material = UUID::Null())
	{
		Entity entity = scene.CreateEntity("Cube");
		entity.GetComponent<TransformComponent>().Translation = position;
		MeshRendererComponent& renderer = entity.AddComponent<MeshRendererComponent>();
		renderer.Mesh = BuiltinAssets::CubeMesh;
		renderer.Material = material;
		return entity;
	}

}

TEST_SUITE("Renderer.RenderScene")
{
	TEST_CASE("Proxies match a rebuild from the scene through 5,000 random edits")
	{
		std::mt19937 random(2024);
		const Ref<Scene> scene = CreateRef<Scene>("Edits");
		RenderScene& renderScene = RenderScene::Get(*scene);
		CHECK(&RenderScene::Get(*scene) == &renderScene);
		CHECK(RenderScene::Find(*scene) == &renderScene);

		// Two meshes whose layouts become known (with submeshes of their own materials), one that never loads, and none.
		const Ref<Mesh> cube = MeshFactory::CreateCube(1.0f, UUID(0xC0));
		const Ref<Mesh> sphere = MeshFactory::CreateSphere(0.5f, 12, 6);
		REQUIRE(cube);
		REQUIRE(sphere);
		const AssetHandle meshes[] = { BuiltinAssets::CubeMesh, BuiltinAssets::SphereMesh, UUID(0x7777), UUID::Null() };
		const AssetHandle materials[] = { UUID::Null(), UUID(0xA1), UUID(0xA2), BuiltinAssets::DefaultMaterial };

		std::vector<Entity> entities;
		auto pick = [&](size_t count) { return std::uniform_int_distribution<size_t>(0, count - 1)(random); };
		auto addRenderer = [&](Entity entity)
		{
			MeshRendererComponent& renderer = entity.AddComponent<MeshRendererComponent>();
			renderer.Mesh = meshes[pick(std::size(meshes))];
			renderer.Material = materials[pick(std::size(materials))];
			renderer.CastShadows = pick(2) == 0;
		};
		for (int index = 0; index < 200; index++)
		{
			Entity entity = scene->CreateEntity("Entity");
			RandomizeTransform(entity, random);
			if (pick(4) != 0)
				addRenderer(entity);
			entities.push_back(entity);
		}

		auto learnLayouts = [&]()
		{
			for (const auto& [handle, mesh] : { std::pair { BuiltinAssets::CubeMesh, cube }, std::pair { BuiltinAssets::SphereMesh, sphere } })
			{
				const uint32_t slot = FindMeshSlot(renderScene, handle);
				if (slot != RenderScene::c_InvalidIndex && (!renderScene.IsMeshLayoutKnown(slot) || renderScene.IsMeshLayoutStale(slot)))
					renderScene.SetMeshLayout(slot, *mesh);
			}
		};

		for (int edit = 0; edit < 5000; edit++)
		{
			CAPTURE(edit);
			// Entities destroyed with a destroyed ancestor drop out here.
			std::erase_if(entities, [](const Entity& entity) { return !entity.IsValid(); });
			if (entities.empty())
				entities.push_back(scene->CreateEntity("Entity"));
			Entity entity = entities[pick(entities.size())];
			switch (pick(12))
			{
				case 0:
				{
					Entity created = pick(2) == 0 ? scene->CreateEntity("Entity") : scene->CreateChildEntity(entity, "Child");
					RandomizeTransform(created, random);
					if (pick(3) != 0)
						addRenderer(created);
					entities.push_back(created);
					break;
				}
				case 1:
					scene->DestroyEntity(entity);
					break;
				case 2:
					if (!entity.HasComponent<MeshRendererComponent>())
						addRenderer(entity);
					else
						entity.RemoveComponent<MeshRendererComponent>();
					break;
				case 3:
					if (entity.HasComponent<MeshRendererComponent>())
					{
						entity.GetComponent<MeshRendererComponent>().Mesh = meshes[pick(std::size(meshes))];
						entity.MarkModified<MeshRendererComponent>();
					}
					break;
				case 4:
					if (entity.HasComponent<MeshRendererComponent>())
					{
						entity.GetComponent<MeshRendererComponent>().Material = materials[pick(std::size(materials))];
						entity.MarkModified<MeshRendererComponent>();
					}
					break;
				case 5:
					if (entity.HasComponent<MeshRendererComponent>())
					{
						MeshRendererComponent& renderer = entity.GetComponent<MeshRendererComponent>();
						renderer.CastShadows = !renderer.CastShadows;
						entity.MarkModified<MeshRendererComponent>();
					}
					break;
				case 6:
				case 7:
					RandomizeTransform(entity, random);
					entity.MarkModified<TransformComponent>();
					break;
				case 8:
				{
					// Mirrored, flat and non-finite transforms.
					const glm::vec3 scales[] = { glm::vec3(-1.0f, 1.0f, 1.0f), glm::vec3(0.0f), glm::vec3(1.0f, std::numeric_limits<float>::infinity(), 1.0f), glm::vec3(2.0f) };
					entity.GetComponent<TransformComponent>().Scale = scales[pick(std::size(scales))];
					entity.MarkModified<TransformComponent>();
					break;
				}
				case 9:
				{
					// The local transform is kept, so the subtree's world transforms change.
					Entity parent = pick(3) == 0 ? Entity() : entities[pick(entities.size())];
					scene->SetParent(entity, parent, false);
					break;
				}
				case 10:
					entity.SetActive(!entity.IsActive());
					break;
				case 11:
					// A reload: layouts go stale and are learned again from the "loaded" meshes.
					renderScene.OnAssetsChanged(std::vector<AssetHandle> { meshes[pick(2)] }, pick(10) == 0);
					break;
			}
			if (edit % 25 == 24)
			{
				renderScene.Sync();
				if (pick(2) == 0)
					learnLayouts();
				CheckProxies(renderScene);
			}
		}
		renderScene.Sync();
		learnLayouts();
		CheckProxies(renderScene);
		CheckSceneCaches(*scene);

		// A render scene built from scratch for a copy of the scene agrees on every proxy.
		const Ref<Scene> copy = Scene::Copy(scene);
		RenderScene& fresh = RenderScene::Get(*copy);
		CHECK(fresh.GetId() != renderScene.GetId());
		fresh.Sync();
		for (const auto& [handle, mesh] : { std::pair { BuiltinAssets::CubeMesh, cube }, std::pair { BuiltinAssets::SphereMesh, sphere } })
		{
			const uint32_t slot = FindMeshSlot(fresh, handle);
			if (slot != RenderScene::c_InvalidIndex)
				fresh.SetMeshLayout(slot, *mesh);
		}
		CheckProxies(fresh);
		CHECK(fresh.GetStats().Proxies == renderScene.GetStats().Proxies);
		CHECK(fresh.GetStats().IndexedProxies == renderScene.GetStats().IndexedProxies);
		CHECK(fresh.GetStats().InstanceSlots == renderScene.GetStats().InstanceSlots);
		CHECK(fresh.GetStats().Meshes == renderScene.GetStats().Meshes);
		CHECK(fresh.GetStats().Materials == renderScene.GetStats().Materials);
		for (uint32_t proxy = 0; proxy < renderScene.GetProxyCapacity(); proxy++)
		{
			const entt::entity handle = renderScene.GetEntity(proxy);
			if (handle == entt::null)
				continue;
			const Entity original(handle, scene.get());
			const Entity copied = copy->GetEntityByUUID(original.GetUUID());
			const uint32_t copiedProxy = fresh.FindProxy(copied.GetHandle());
			REQUIRE(copiedProxy != RenderScene::c_InvalidIndex);
			CHECK(fresh.GetProxyFlags(copiedProxy) == renderScene.GetProxyFlags(proxy));
			CHECK(fresh.GetWorldBounds(copiedProxy).Min == renderScene.GetWorldBounds(proxy).Min);
			CHECK(fresh.GetWorldBounds(copiedProxy).Max == renderScene.GetWorldBounds(proxy).Max);
			CHECK(fresh.GetInstanceCount(copiedProxy) == renderScene.GetInstanceCount(proxy));
		}
	}

	TEST_CASE("A static sync updates nothing; moving one of 100,000 cubes updates one proxy and one instance")
	{
		Scene scene("Cubes");
		for (int index = 0; index < 100'000; index++)
			AddCube(scene, glm::vec3(static_cast<float>(index % 316) * 2.0f, 0.5f, static_cast<float>(index / 316) * 2.0f));
		RenderScene& renderScene = RenderScene::Get(scene);
		CHECK(renderScene.Sync() == 100'000);
		CHECK(renderScene.GetStats().Proxies == 100'000);
		CHECK(renderScene.GetStats().IndexedProxies == 100'000);
		REQUIRE(renderScene.GetMeshesWithoutLayout().size() == 1);

		// The cube's layout arrives: every proxy gets bounds from the mesh and an instance.
		const Ref<Mesh> cube = MeshFactory::CreateCube();
		const uint32_t slot = renderScene.GetMeshesWithoutLayout()[0];
		CHECK(renderScene.SetMeshLayout(slot, *cube) == 100'000);
		CHECK(renderScene.GetMeshesWithoutLayout().empty());
		CHECK(renderScene.GetStats().InstanceSlots == 100'000);
		CHECK(renderScene.SetMeshLayout(slot, *cube) == 0); // The same layout again changes nothing
		CheckProxies(renderScene);

		uint64_t version = renderScene.GetInstanceVersion();
		std::vector<uint32_t> changes;
		CHECK(renderScene.Sync() == 0);
		CHECK(renderScene.GetInstanceVersion() == version);
		CHECK(renderScene.GetInstanceChanges(version, changes));
		CHECK(changes.empty());

		Entity moved(renderScene.GetEntity(1234), &scene);
		moved.GetComponent<TransformComponent>().Translation.y += 1.0f;
		moved.MarkModified<TransformComponent>();
		CHECK(renderScene.Sync() == 1);
		CHECK(renderScene.GetStats().LastSyncUpdates == 1);
		REQUIRE(renderScene.GetInstanceChanges(version, changes));
		REQUIRE(changes.size() == 1);
		CHECK(changes[0] == renderScene.GetFirstInstance(1234));
		CHECK(renderScene.GetWorldBounds(1234).Min.y == doctest::Approx(1.0f));
		CHECK(renderScene.GetInstances()[changes[0]].World.Rows[1].w == doctest::Approx(1.5f));
		CheckProxies(renderScene);

		// A write that is not signaled is noticed by the validation (Debug builds assert it after every sync).
		version = renderScene.GetInstanceVersion();
		moved.GetComponent<MeshRendererComponent>().CastShadows = false;
		std::string error;
		CHECK_FALSE(renderScene.Validate(&error));
		CHECK(error.find("shadow") != std::string::npos);
		moved.MarkModified<MeshRendererComponent>();
		CHECK(renderScene.Sync() == 1);
		CHECK((renderScene.GetProxyFlags(1234) & RenderProxyFlags::CastShadows) == 0);
		CheckProxies(renderScene);
	}

	TEST_CASE("Proxies of meshes that never loaded are bounded around their entity until the mesh arrives")
	{
		Scene scene("Bounds");
		Entity entity = AddCube(scene, glm::vec3(10.0f, 0.0f, 0.0f));
		entity.GetComponent<TransformComponent>().Scale = glm::vec3(3.0f);
		RenderScene& renderScene = RenderScene::Get(scene);
		renderScene.Sync();
		const uint32_t proxy = renderScene.FindProxy(entity.GetHandle());
		REQUIRE(proxy != RenderScene::c_InvalidIndex);
		const AABB unloaded = RenderScene::ResolveUnloadedBounds(BuiltinAssets::CubeMesh);
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::MeshBounds) == 0);
		CHECK(renderScene.GetWorldBounds(proxy).Min == glm::vec3(10.0f, 0.0f, 0.0f) + unloaded.Min * 3.0f);
		CHECK(renderScene.GetInstanceCount(proxy) == 0);

		const Ref<Mesh> wide = MeshFactory::CreateCube(4.0f);
		renderScene.SetMeshLayout(renderScene.GetMeshSlot(proxy), *wide);
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::MeshBounds) != 0);
		CHECK(renderScene.GetWorldBounds(proxy).Min.x == doctest::Approx(10.0f - 6.0f));
		CHECK(renderScene.GetInstanceCount(proxy) == 1);
		std::vector<uint32_t> found;
		renderScene.GetSpatialIndex().QuerySphere(glm::vec3(4.5f, 0.0f, 0.0f), 0.6f, found);
		CHECK(found == std::vector<uint32_t> { proxy });

		// A changed mesh (a reload) is compared with the layout when it is learned again.
		renderScene.OnAssetsChanged(std::vector<AssetHandle> { BuiltinAssets::CubeMesh }, false);
		CHECK(renderScene.IsMeshLayoutStale(renderScene.GetMeshSlot(proxy)));
		CHECK(renderScene.SetMeshLayout(renderScene.GetMeshSlot(proxy), *wide) == 0);
		CHECK_FALSE(renderScene.IsMeshLayoutStale(renderScene.GetMeshSlot(proxy)));
		CheckProxies(renderScene);
	}

	TEST_CASE("Mesh and material slots are shared by their users and released with the last one")
	{
		Scene scene("Slots");
		Entity first = AddCube(scene, glm::vec3(0.0f), UUID(0xB1));
		Entity second = AddCube(scene, glm::vec3(2.0f), UUID(0xB1));
		Entity third = AddCube(scene, glm::vec3(4.0f), UUID(0xB2));
		RenderScene& renderScene = RenderScene::Get(scene);
		renderScene.Sync();
		CHECK(renderScene.GetStats().Meshes == 1);
		CHECK(renderScene.GetStats().Materials == 2);
		const uint32_t firstProxy = renderScene.FindProxy(first.GetHandle());
		const uint32_t secondProxy = renderScene.FindProxy(second.GetHandle());
		CHECK(renderScene.GetOverrideMaterialSlot(firstProxy) == renderScene.GetOverrideMaterialSlot(secondProxy));
		CHECK(renderScene.GetMeshSlot(firstProxy) == renderScene.GetMeshSlot(secondProxy));

		// The cube's own submesh material (the default material) gets a slot of its own with the layout.
		renderScene.SetMeshLayout(renderScene.GetMeshSlot(firstProxy), *MeshFactory::CreateCube());
		CHECK(renderScene.GetStats().Materials == 3);
		CHECK(renderScene.GetInstances()[renderScene.GetFirstInstance(firstProxy)].MaterialIndex == renderScene.GetOverrideMaterialSlot(firstProxy));

		scene.DestroyEntity(first);
		third.GetComponent<MeshRendererComponent>().Material = UUID::Null();
		third.MarkModified<MeshRendererComponent>();
		renderScene.Sync();
		CHECK(renderScene.GetStats().Proxies == 2);
		CHECK(renderScene.GetStats().Materials == 2); // 0xB2 went with its only user
		CHECK(renderScene.GetInstances()[renderScene.GetFirstInstance(renderScene.FindProxy(third.GetHandle()))].MaterialIndex
			== renderScene.GetSubmeshMaterialSlots(renderScene.GetMeshSlot(secondProxy))[0]);
		CheckProxies(renderScene);

		scene.DestroyEntity(second);
		scene.DestroyEntity(third);
		renderScene.Sync();
		CHECK(renderScene.GetStats().Proxies == 0);
		CHECK(renderScene.GetStats().Meshes == 0);
		CHECK(renderScene.GetStats().Materials == 0);
		CHECK(renderScene.GetStats().IndexedProxies == 0);
		CHECK(renderScene.GetStats().InstanceSlots == 0);
		CheckProxies(renderScene);
	}

	TEST_CASE("Renderers that fall behind the bounded change log upload everything")
	{
		Scene scene("Changes");
		std::vector<Entity> cubes;
		for (int index = 0; index < 3000; index++)
			cubes.push_back(AddCube(scene, glm::vec3(static_cast<float>(index), 0.0f, 0.0f)));
		RenderScene& renderScene = RenderScene::Get(scene);
		renderScene.Sync();
		renderScene.SetMeshLayout(renderScene.GetMeshesWithoutLayout()[0], *MeshFactory::CreateCube());
		const uint64_t start = renderScene.GetInstanceVersion();

		std::vector<uint32_t> changes;
		size_t moves = 0;
		uint64_t recent = start;
		while (moves <= RenderScene::c_MaxInstanceChanges)
		{
			recent = renderScene.GetInstanceVersion();
			for (Entity cube : cubes)
			{
				cube.GetComponent<TransformComponent>().Translation.y += 1.0f;
				cube.MarkModified<TransformComponent>();
			}
			CHECK(renderScene.Sync() == cubes.size());
			moves += cubes.size();
		}
		CHECK_FALSE(renderScene.GetInstanceChanges(start, changes));
		CHECK(changes.empty());
		CHECK(renderScene.GetInstanceChanges(recent, changes));
		CHECK(changes.size() == cubes.size());
	}

	TEST_CASE("Entities without a mesh, inactive or with a flat transform are not indexed")
	{
		Scene scene("Indexed");
		Entity noMesh = scene.CreateEntity("NoMesh");
		noMesh.AddComponent<MeshRendererComponent>();
		Entity inactive = AddCube(scene, glm::vec3(0.0f));
		inactive.SetActive(false);
		Entity flat = AddCube(scene, glm::vec3(0.0f));
		flat.GetComponent<TransformComponent>().Scale = glm::vec3(1.0f, 0.0f, 1.0f);
		Entity mirrored = AddCube(scene, glm::vec3(0.0f));
		mirrored.GetComponent<TransformComponent>().Scale = glm::vec3(-1.0f, 1.0f, 1.0f);
		RenderScene& renderScene = RenderScene::Get(scene);
		renderScene.Sync();
		CHECK(renderScene.GetStats().Proxies == 4);
		CHECK(renderScene.GetStats().IndexedProxies == 1);
		CHECK((renderScene.GetProxyFlags(renderScene.FindProxy(mirrored.GetHandle())) & RenderProxyFlags::Mirrored) != 0);
		CHECK((renderScene.GetProxyFlags(renderScene.FindProxy(inactive.GetHandle())) & RenderProxyFlags::Active) == 0);

		inactive.SetActive(true);
		CHECK(renderScene.Sync() == 1);
		CHECK(renderScene.GetStats().IndexedProxies == 2);
		CheckProxies(renderScene);
	}
}
