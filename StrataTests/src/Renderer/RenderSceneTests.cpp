#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Renderer/MeshTestUtils.h"
#include "Scene/SceneTestUtils.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Scene.h"

#if ST_WITH_ANIMATION
	#include "Animation/SkinnedMeshTestUtils.h"
	#include "Animation/TestSkinningPoses.h"
#endif

#include <algorithm>
#include <random>
#include <span>
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

#if ST_WITH_ANIMATION
	uint32_t FindMeshSlot(const RenderScene& renderScene, AssetHandle mesh)
	{
		for (uint32_t slot = 0; slot < renderScene.GetMeshSlotCapacity(); slot++)
		{
			if (renderScene.GetMeshHandle(slot) == mesh)
				return slot;
		}
		return RenderScene::c_InvalidIndex;
	}
#endif

	// What a reader of the proxy change log (the GPU mirror of a render scene) keeps of each proxy. Brought up to date from
	// the log alone, it must match the render scene: every change that matters to the GPU's culling is logged.
	struct ProxyRecord
	{
		entt::entity Entity = entt::null;
		uint32_t Flags = 0;
		glm::vec3 LocalMin = glm::vec3(0.0f);
		glm::vec3 LocalMax = glm::vec3(0.0f);
		glm::vec3 WorldMin = glm::vec3(0.0f);
		glm::vec3 WorldMax = glm::vec3(0.0f);
		uint32_t MeshSlot = RenderScene::c_InvalidIndex;
		uint32_t FirstInstance = 0;
		uint32_t InstanceCount = 0;

		bool operator==(const ProxyRecord&) const = default;
	};

	struct ProxyLogReader
	{
		uint64_t Version = 0;
		std::vector<ProxyRecord> Records;
		uint32_t FullReads = 0;
		uint32_t LoggedReads = 0;

		static ProxyRecord Read(const RenderScene& renderScene, uint32_t proxy)
		{
			ProxyRecord record;
			record.Entity = renderScene.GetEntity(proxy);
			record.Flags = renderScene.GetProxyFlags(proxy);
			record.LocalMin = renderScene.GetLocalBounds(proxy).Min;
			record.LocalMax = renderScene.GetLocalBounds(proxy).Max;
			record.WorldMin = renderScene.GetWorldBounds(proxy).Min;
			record.WorldMax = renderScene.GetWorldBounds(proxy).Max;
			record.MeshSlot = renderScene.GetMeshSlot(proxy);
			record.FirstInstance = renderScene.GetFirstInstance(proxy);
			record.InstanceCount = renderScene.GetInstanceCount(proxy);
			return record;
		}

		void Update(const RenderScene& renderScene)
		{
			std::vector<uint32_t> changed;
			const bool logged = renderScene.GetProxyChanges(Version, changed);
			Records.resize(renderScene.GetProxyCapacity());
			if (logged)
			{
				for (const uint32_t proxy : changed)
					Records[proxy] = Read(renderScene, proxy);
				LoggedReads++;
			}
			else
			{
				for (uint32_t proxy = 0; proxy < renderScene.GetProxyCapacity(); proxy++)
					Records[proxy] = Read(renderScene, proxy);
				FullReads++;
			}
			Version = renderScene.GetProxyVersion();
		}

		void Check(const RenderScene& renderScene) const
		{
			REQUIRE(Records.size() == renderScene.GetProxyCapacity());
			for (uint32_t proxy = 0; proxy < renderScene.GetProxyCapacity(); proxy++)
			{
				CAPTURE(proxy);
				CHECK(Records[proxy] == Read(renderScene, proxy));
			}
		}
	};

	Entity AddCube(Scene& scene, const glm::vec3& position, AssetHandle material = UUID::Null(), AssetHandle mesh = BuiltinAssets::CubeMesh)
	{
		Entity entity = scene.CreateEntity("Cube");
		entity.GetComponent<TransformComponent>().Translation = position;
		MeshRendererComponent& renderer = entity.AddComponent<MeshRendererComponent>();
		renderer.Mesh = mesh;
		renderer.Material = material;
		return entity;
	}

}

TEST_SUITE("Renderer.RenderScene")
{
#if ST_WITH_ANIMATION
	TEST_CASE("Proxies match a rebuild from the scene through 5,000 random edits")
	{
		std::mt19937 random(2024);
		// Meshes: a built-in one (always loaded), a stored one whose layout (bounds and submeshes) changes with reloads,
		// one that fails to load, one the asset manager does not know, a skinned one, and none.
		const Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle reloaded = manager->AddStored(0x5E01, AssetType::Mesh, CreatePartedCube(1.0f, 1)->Serialize());
		const AssetHandle failing = manager->AddStored(0x5E02, AssetType::Mesh, std::vector<uint8_t> { 1, 2, 3 });
		const AssetHandle skinned = manager->AddStored(0x5E03, AssetType::Mesh, CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 2, .Sides = 4 })->Serialize());
		// Poses for some entities, which skin the mesh renderers whose SkinComponents name them.
		TestSkinningPoses poses;
		const Ref<Scene> scene = CreateRef<Scene>("Edits");
		REQUIRE(scene->RegisterService<ISkinningPoses>(poses));
		RenderScene& renderScene = RenderScene::Get(*scene);
		CHECK(&RenderScene::Get(*scene) == &renderScene);
		CHECK(RenderScene::Find(*scene) == &renderScene);

		const AssetHandle meshes[] = { BuiltinAssets::CubeMesh, reloaded, failing, UUID(0x7777), skinned, skinned, UUID::Null() };
		const AssetHandle materials[] = { UUID::Null(), UUID(0xA1), UUID(0xA2), BuiltinAssets::DefaultMaterial };
		uint32_t reloads = 0;
		uint32_t mostPosed = 0;

		std::vector<Entity> entities;
		// A reader of the proxy change log, as the GPU mirror reads it; a second one reads only now and then.
		ProxyLogReader reader;
		ProxyLogReader slowReader;
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

		for (int edit = 0; edit < 5000; edit++)
		{
			CAPTURE(edit);
			// Entities destroyed with a destroyed ancestor drop out here.
			std::erase_if(entities, [](const Entity& entity) { return !entity.IsValid(); });
			if (entities.empty())
				entities.push_back(scene->CreateEntity("Entity"));
			Entity entity = entities[pick(entities.size())];
			switch (pick(15))
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
				{
					// A reload of the stored mesh with another layout, which the next sync after it loaded learns. Renderers
					// request the meshes; here the edits do, now and then.
					reloads++;
					manager->SetStoredData(reloaded, CreatePartedCube(1.0f + static_cast<float>(reloads % 3), 1 + reloads % 2)->Serialize());
					if (pick(2) == 0)
						manager->RequestLoad(reloaded);
					if (pick(4) == 0)
						manager->RequestLoad(failing);
					if (pick(2) == 0)
						manager->RequestLoad(skinned);
					manager->ReloadAsset(reloaded);
					break;
				}
				case 12:
				case 13:
				{
					// Skins naming the entity itself, another entity, or no entity that exists.
					const UUID animators[] = { entity.GetUUID(), entities[pick(entities.size())].GetUUID(), UUID(0xDEAD) };
					if (!entity.HasComponent<SkinComponent>())
					{
						entity.AddComponent<SkinComponent>().Animator = animators[pick(std::size(animators))];
					}
					else if (pick(3) == 0)
					{
						entity.RemoveComponent<SkinComponent>();
					}
					else
					{
						entity.GetComponent<SkinComponent>().Animator = animators[pick(std::size(animators))];
						entity.MarkModified<SkinComponent>();
					}
					break;
				}
				case 14:
				{
					// Poses come and go, with fewer joints than the skin needs now and then.
					if (poses.Poses.contains(entity.GetHandle()) && pick(3) == 0)
						poses.Poses.erase(entity.GetHandle());
					else
						poses.Poses[entity.GetHandle()] = BentChain(pick(4) == 0 ? 2u : 3u, static_cast<uint32_t>(pick(3)));
					poses.Version++;
					break;
				}
			}
			if (edit % 25 == 24)
			{
				// Loads arrive in the manager's updates, which not every sync follows.
				if (pick(2) == 0)
					manager->Update();
				renderScene.Sync(manager);
				CheckProxies(renderScene);
				reader.Update(renderScene);
				reader.Check(renderScene);
				if (edit % 500 == 499)
				{
					slowReader.Update(renderScene);
					slowReader.Check(renderScene);
				}
				mostPosed = std::max(mostPosed, renderScene.GetStats().PosedProxies);
			}
		}
		CHECK(reader.LoggedReads > 0);
		CHECK(reader.FullReads == 0); // The log holds far more changes than 500 edits make
		manager->RequestLoad(reloaded);
		manager->RequestLoad(failing);
		manager->RequestLoad(skinned);
		REQUIRE(manager->WaitForPendingLoads());
		renderScene.Sync(manager);
		CheckProxies(renderScene);
		CheckSceneCaches(*scene);
		CHECK(reloads > 0);
		CHECK(mostPosed > 0);
		// One posed skinned mesh for certain, whatever the edits left.
		Entity posed = scene->CreateEntity("Posed");
		posed.AddComponent<MeshRendererComponent>().Mesh = skinned;
		posed.AddComponent<SkinComponent>().Animator = posed.GetUUID();
		poses.Poses[posed.GetHandle()] = BentChain(3, 1);
		poses.Version++;
		renderScene.Sync(manager);
		CheckProxies(renderScene);
		CHECK(renderScene.GetStats().SkinProxies > 0);
		CHECK(renderScene.GetStats().PosedProxies > 0);
		// Without poses every skinned mesh shows its bind pose, as in the copy below (which has none).
		scene->UnregisterService<ISkinningPoses>(poses);
		renderScene.Sync(manager);
		CheckProxies(renderScene);
		CHECK(renderScene.GetStats().PosedProxies == 0);
		// The stored mesh has the layout it was reloaded with last; the failing one and the one the manager does not know
		// stay without bounds.
		const uint32_t reloadedSlot = FindMeshSlot(renderScene, reloaded);
		if (reloadedSlot != RenderScene::c_InvalidIndex)
		{
			CHECK(renderScene.IsMeshLayoutKnown(reloadedSlot));
			CHECK(renderScene.GetSubmeshMaterialSlots(reloadedSlot).size() == 1 + reloads % 2);
		}
		for (const uint32_t slot : renderScene.GetMeshesWithoutBounds())
			CHECK((renderScene.GetMeshHandle(slot) == failing || renderScene.GetMeshHandle(slot) == UUID(0x7777)));

		// A render scene built from scratch for a copy of the scene agrees on every proxy.
		const Ref<Scene> copy = Scene::Copy(scene);
		RenderScene& fresh = RenderScene::Get(*copy);
		CHECK(fresh.GetId() != renderScene.GetId());
		fresh.Sync(manager);
		CheckProxies(fresh);
		CHECK(fresh.GetStats().Proxies == renderScene.GetStats().Proxies);
		CHECK(fresh.GetStats().IndexedProxies == renderScene.GetStats().IndexedProxies);
		CHECK(fresh.GetStats().InstanceSlots == renderScene.GetStats().InstanceSlots);
		CHECK(fresh.GetStats().Meshes == renderScene.GetStats().Meshes);
		CHECK(fresh.GetStats().Materials == renderScene.GetStats().Materials);
		CHECK(fresh.GetMeshesWithoutBounds().size() == renderScene.GetMeshesWithoutBounds().size());
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
#endif

	TEST_CASE("A static sync updates nothing; moving one of 100,000 cubes updates one proxy and one instance")
	{
		Scene scene("Cubes");
		for (int index = 0; index < 100'000; index++)
			AddCube(scene, glm::vec3(static_cast<float>(index % 316) * 2.0f, 0.5f, static_cast<float>(index / 316) * 2.0f));
		RenderScene& renderScene = RenderScene::Get(scene);
		CHECK(renderScene.Sync(nullptr) == 100'000);
		CHECK(renderScene.GetStats().Proxies == 100'000);
		CHECK(renderScene.GetStats().IndexedProxies == 100'000);
		REQUIRE(renderScene.GetMeshesWithoutBounds().size() == 1);

		// The cube's layout arrives: every proxy gets bounds from the mesh and an instance.
		const Ref<Mesh> cube = MeshFactory::CreateCube();
		const uint32_t slot = renderScene.GetMeshesWithoutBounds()[0];
		CHECK(renderScene.GetIndexedUserCount(slot) == 100'000);
		CHECK(renderScene.SetMeshLayout(slot, *cube) == 100'000);
		CHECK(renderScene.GetMeshesWithoutBounds().empty());
		CHECK(renderScene.GetStats().InstanceSlots == 100'000);
		CHECK(renderScene.SetMeshLayout(slot, *cube) == 0); // The same layout again changes nothing
		CheckProxies(renderScene);

		uint64_t version = renderScene.GetInstanceVersion();
		std::vector<uint32_t> changes;
		CHECK(renderScene.Sync(nullptr) == 0);
		CHECK(renderScene.GetInstanceVersion() == version);
		CHECK(renderScene.GetInstanceChanges(version, changes));
		CHECK(changes.empty());

		Entity moved(renderScene.GetEntity(1234), &scene);
		moved.GetComponent<TransformComponent>().Translation.y += 1.0f;
		moved.MarkModified<TransformComponent>();
		CHECK(renderScene.Sync(nullptr) == 1);
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
		CHECK(renderScene.Sync(nullptr) == 1);
		CHECK((renderScene.GetProxyFlags(1234) & RenderProxyFlags::CastShadows) == 0);
		CheckProxies(renderScene);
	}

	TEST_CASE("Meshes that never loaded have unknown bounds: their proxies get placeholders, and active users are counted")
	{
		Scene scene("Bounds");
		Entity entity = AddCube(scene, glm::vec3(10.0f, 0.0f, 0.0f));
		entity.GetComponent<TransformComponent>().Scale = glm::vec3(3.0f);
		Entity other = AddCube(scene, glm::vec3(-10.0f, 0.0f, 0.0f));
		RenderScene& renderScene = RenderScene::Get(scene);
		renderScene.Sync(nullptr);
		const uint32_t proxy = renderScene.FindProxy(entity.GetHandle());
		REQUIRE(proxy != RenderScene::c_InvalidIndex);
		const uint32_t slot = renderScene.GetMeshSlot(proxy);
		const AABB placeholder = RenderScene::GetPlaceholderBounds();
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::MeshBounds) == 0);
		CHECK(renderScene.GetWorldBounds(proxy).Min == glm::vec3(10.0f, 0.0f, 0.0f) + placeholder.Min * 3.0f);
		CHECK(renderScene.GetInstanceCount(proxy) == 0);
		CHECK(renderScene.GetMeshesWithoutBounds().size() == 1);
		CHECK(renderScene.GetIndexedUserCount(slot) == 2);

		// Only active, drawable users need the mesh.
		other.SetActive(false);
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetIndexedUserCount(slot) == 1);
		entity.GetComponent<TransformComponent>().Scale = glm::vec3(0.0f);
		entity.MarkModified<TransformComponent>();
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetIndexedUserCount(slot) == 0);
		CheckProxies(renderScene);
		entity.GetComponent<TransformComponent>().Scale = glm::vec3(3.0f);
		entity.MarkModified<TransformComponent>();
		other.SetActive(true);
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetIndexedUserCount(slot) == 2);

		const Ref<Mesh> wide = MeshFactory::CreateCube(4.0f);
		renderScene.SetMeshLayout(renderScene.GetMeshSlot(proxy), *wide);
		CHECK(renderScene.GetMeshesWithoutBounds().empty());
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::MeshBounds) != 0);
		CHECK(renderScene.GetWorldBounds(proxy).Min.x == doctest::Approx(10.0f - 6.0f));
		CHECK(renderScene.GetInstanceCount(proxy) == 1);
		std::vector<uint32_t> found;
		renderScene.GetSpatialIndex().QuerySphere(glm::vec3(4.5f, 0.0f, 0.0f), 0.6f, found);
		CHECK(found == std::vector<uint32_t> { proxy });
		CHECK(renderScene.SetMeshLayout(renderScene.GetMeshSlot(proxy), *wide) == 0); // The same layout again changes nothing
		CHECK(renderScene.GetIndexedUserCount(slot) == 2);
		CheckProxies(renderScene);

		// Users that move to another mesh are counted for that one.
		other.GetComponent<MeshRendererComponent>().Mesh = BuiltinAssets::SphereMesh;
		other.MarkModified<MeshRendererComponent>();
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetIndexedUserCount(slot) == 1);
		CHECK(renderScene.GetIndexedUserCount(renderScene.GetMeshSlot(renderScene.FindProxy(other.GetHandle()))) == 1);
		CheckProxies(renderScene);
	}

	TEST_CASE("Mesh layouts follow the asset manager: learned once loaded, kept through eviction, updated by reloads")
	{
		const Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle stored = manager->AddStored(0x5E10, AssetType::Mesh, CreatePartedCube(1.0f, 1)->Serialize());
		const AssetHandle failing = manager->AddStored(0x5E11, AssetType::Mesh, std::vector<uint8_t> { 1, 2, 3 });
		Scene scene("Layouts");
		Entity storedUser = AddCube(scene, glm::vec3(100.0f, 0.0f, 0.0f), UUID::Null(), stored);
		Entity failingUser = AddCube(scene, glm::vec3(0.0f), UUID::Null(), failing);
		Entity unknownUser = AddCube(scene, glm::vec3(0.0f), UUID::Null(), UUID(0x7777));
		Entity builtinUser = AddCube(scene, glm::vec3(0.0f));
		RenderScene& renderScene = RenderScene::Get(scene);
		renderScene.Sync(manager);
		const uint32_t storedProxy = renderScene.FindProxy(storedUser.GetHandle());
		const uint32_t storedSlot = renderScene.GetMeshSlot(storedProxy);
		const uint32_t failingSlot = renderScene.GetMeshSlot(renderScene.FindProxy(failingUser.GetHandle()));
		const uint32_t unknownSlot = renderScene.GetMeshSlot(renderScene.FindProxy(unknownUser.GetHandle()));
		auto withoutBounds = [&](uint32_t slot)
		{
			const std::span<const uint32_t> slots = renderScene.GetMeshesWithoutBounds();
			return std::find(slots.begin(), slots.end(), slot) != slots.end();
		};

		// The built-in mesh is loaded: its layout is learned at once. The others never loaded and have no recorded bounds:
		// their bounds are unknown. The render scene loads nothing itself; renderers request such meshes as if in view.
		CHECK(renderScene.IsMeshLayoutKnown(renderScene.GetMeshSlot(renderScene.FindProxy(builtinUser.GetHandle()))));
		CHECK(renderScene.GetMeshesWithoutBounds().size() == 3);
		CHECK(withoutBounds(storedSlot));
		CHECK(withoutBounds(failingSlot));
		CHECK(withoutBounds(unknownSlot));
		CHECK((renderScene.GetProxyFlags(storedProxy) & RenderProxyFlags::MeshBounds) == 0);
		CHECK(renderScene.GetIndexedUserCount(storedSlot) == 1);
		CHECK(manager->GetAssetState(stored) == AssetState::Unloaded);
		CHECK(renderScene.GetStats().LastSyncMeshChecks == 4);
		// Nothing loaded or changed since: a sync compares no mesh with the manager.
		CHECK(renderScene.Sync(manager) == 0);
		CHECK(renderScene.GetStats().LastSyncMeshChecks == 0);

		// Requested (as a renderer does) and loaded: the next sync learns the layout from the content change; the failed
		// load changes nothing.
		manager->RequestLoad(stored);
		manager->RequestLoad(failing);
		REQUIRE(manager->WaitForPendingLoads());
		REQUIRE(manager->GetAssetState(failing) == AssetState::Failed);
		CHECK(renderScene.Sync(manager) == 1);
		CHECK(renderScene.GetStats().LastSyncMeshChecks == 1);
		CHECK(renderScene.IsMeshLayoutKnown(storedSlot));
		CHECK_FALSE(withoutBounds(storedSlot));
		CHECK((renderScene.GetProxyFlags(storedProxy) & RenderProxyFlags::MeshBounds) != 0);
		CHECK(renderScene.GetInstanceCount(storedProxy) == 1);
		CHECK(renderScene.GetWorldBounds(storedProxy).Max.x == doctest::Approx(100.5f));
		CHECK_FALSE(renderScene.IsMeshLayoutKnown(failingSlot));
		CHECK(withoutBounds(failingSlot));
		CheckProxies(renderScene);

		// Nothing holds the mesh any more: it is evicted like any unused asset, and its proxy keeps its layout without
		// loading it again.
		manager->Update();
		CHECK(manager->TrimUnused(0) == 1);
		CHECK(manager->GetAssetState(stored) == AssetState::Unloaded);
		CHECK(renderScene.Sync(manager) == 0);
		CHECK(renderScene.GetStats().LastSyncMeshChecks == 1); // The eviction is a content change
		CHECK(renderScene.IsMeshLayoutKnown(storedSlot));
		CHECK(renderScene.GetInstanceCount(storedProxy) == 1);
		CHECK(manager->GetAssetState(stored) == AssetState::Unloaded);

		// Reloaded with another layout while the render scene is not synced (its scene is not rendered): the next sync
		// learns it, whoever renders the scene and wherever the old bounds were.
		REQUIRE(manager->LoadAssetSync(stored));
		manager->SetStoredData(stored, CreatePartedCube(4.0f, 2)->Serialize());
		manager->ReloadAsset(stored);
		for (int update = 0; update < 3; update++)
			manager->Update();
		REQUIRE(manager->GetAssetState(stored) == AssetState::Ready);
		CHECK(renderScene.Sync(manager) == 1);
		CHECK(renderScene.GetInstanceCount(storedProxy) == 2);
		CHECK(renderScene.GetSubmeshMaterialSlots(storedSlot).size() == 2);
		CHECK(renderScene.GetWorldBounds(storedProxy).Max.x == doctest::Approx(102.0f));
		CheckProxies(renderScene);

		// Another asset manager (another project): every layout is compared with its meshes, and the mesh that failed in
		// the first one loads in this one.
		const Ref<FakeAssetManager> other = CreateRef<FakeAssetManager>();
		other->AddStored(0x5E10, AssetType::Mesh, CreatePartedCube(1.0f, 3)->Serialize());
		other->AddStored(0x5E11, AssetType::Mesh, CreatePartedCube(1.0f, 1)->Serialize());
		REQUIRE(other->LoadAssetSync(stored));
		CHECK(renderScene.Sync(other) == 1);
		CHECK(renderScene.GetStats().LastSyncMeshChecks == 4);
		CHECK(renderScene.GetInstanceCount(storedProxy) == 3);
		CHECK(withoutBounds(failingSlot));
		REQUIRE(other->LoadAssetSync(failing));
		CHECK(renderScene.Sync(other) == 1);
		CHECK_FALSE(withoutBounds(failingSlot));
		CHECK(renderScene.GetMeshesWithoutBounds().size() == 1); // The mesh neither manager knows
		CheckProxies(renderScene);
	}

	TEST_CASE("A render scene that missed more content changes than the manager remembers compares every layout")
	{
		const Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle stored = manager->AddStored(0x5E20, AssetType::Mesh, CreatePartedCube(1.0f, 1)->Serialize());
		Scene scene("Missed");
		Entity entity = AddCube(scene, glm::vec3(0.0f), UUID::Null(), stored);
		RenderScene& renderScene = RenderScene::Get(scene);
		renderScene.Sync(manager);
		REQUIRE(manager->LoadAssetSync(stored));
		renderScene.Sync(manager);
		const uint32_t proxy = renderScene.FindProxy(entity.GetHandle());
		REQUIRE(renderScene.GetInstanceCount(proxy) == 1);
		const uint64_t synced = manager->GetContentVersion();

		manager->SetStoredData(stored, CreatePartedCube(2.0f, 2)->Serialize());
		manager->ReloadAsset(stored);
		manager->Update();
		// More changes than the manager keeps, after the reload: the reload is no longer among them.
		AssetMetadata other;
		other.Handle = UUID(0x3A7E);
		for (size_t change = 0; change <= AssetManagerBase::c_MaxContentChanges; change++)
			manager->AddMemoryAsset(Material::Create(), other);
		std::vector<AssetHandle> changes;
		REQUIRE_FALSE(manager->GetContentChanges(synced, changes));

		CHECK(renderScene.Sync(manager) == 1);
		CHECK(renderScene.GetInstanceCount(proxy) == 2);
		CheckProxies(renderScene);
	}

	TEST_CASE("A mesh reimported while it was not loaded loses its layout until it loads again; an eviction or a reload under way keeps it")
	{
		const Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		const AssetHandle stored = manager->AddStored(0x5E30, AssetType::Mesh, CreatePartedCube(1.0f, 2)->Serialize());
		Scene scene("Reimported");
		Entity entity = AddCube(scene, glm::vec3(100.0f, 0.0f, 0.0f), UUID::Null(), stored);
		RenderScene& renderScene = RenderScene::Get(scene);
		REQUIRE(manager->LoadAssetSync(stored));
		renderScene.Sync(manager);
		const uint32_t proxy = renderScene.FindProxy(entity.GetHandle());
		const uint32_t slot = renderScene.GetMeshSlot(proxy);
		REQUIRE(renderScene.IsMeshLayoutKnown(slot));
		REQUIRE(renderScene.GetInstanceCount(proxy) == 2);
		CHECK(renderScene.GetStats().Materials == 2);

		// Evicted: the layout stays, the source being the one it was learned from.
		manager->Update();
		REQUIRE(manager->TrimUnused(0) == 1);
		CHECK(renderScene.Sync(manager) == 0);
		CHECK(renderScene.GetStats().LastSyncMeshChecks == 1);
		CHECK(renderScene.IsMeshLayoutKnown(slot));
		CHECK(renderScene.GetInstanceCount(proxy) == 2);

		// Reimported eight times larger while it is not loaded: the old bounds no longer say where the mesh is, and nothing
		// would request it where its new bounds are. Its layout is forgotten (bounds, instances, the submeshes' material
		// slots), so its bounds are unknown and renderers request it as if in view.
		manager->SetStoredData(stored, CreatePartedCube(8.0f, 1)->Serialize());
		manager->ReloadAsset(stored);
		REQUIRE(manager->GetAssetState(stored) == AssetState::Unloaded);
		CHECK(renderScene.Sync(manager) == 1);
		CHECK(renderScene.GetStats().LastSyncMeshChecks == 1);
		CHECK_FALSE(renderScene.IsMeshLayoutKnown(slot));
		REQUIRE(renderScene.GetMeshesWithoutBounds().size() == 1);
		CHECK(renderScene.GetMeshesWithoutBounds()[0] == slot);
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::MeshBounds) == 0);
		CHECK(renderScene.GetIndexedUserCount(slot) == 1);
		CHECK(renderScene.GetInstanceCount(proxy) == 0);
		CHECK(renderScene.GetStats().InstanceSlots == 0);
		CHECK(renderScene.GetStats().Materials == 0);
		CheckProxies(renderScene);
		CHECK(renderScene.Sync(manager) == 0);
		CHECK(renderScene.GetStats().LastSyncMeshChecks == 0);

		// Loaded again: the new layout.
		REQUIRE(manager->LoadAssetSync(stored));
		CHECK(renderScene.Sync(manager) == 1);
		CHECK(renderScene.IsMeshLayoutKnown(slot));
		CHECK(renderScene.GetMeshesWithoutBounds().empty());
		CHECK(renderScene.GetInstanceCount(proxy) == 1);
		CHECK(renderScene.GetWorldBounds(proxy).Max.x == doctest::Approx(104.0f));
		CHECK(renderScene.GetStats().Materials == 1);
		CheckProxies(renderScene);

		// Reloaded while it is loaded: renderers draw the previous version until the new one arrives, so a sync that compares
		// the mesh meanwhile (here every mesh, after more changes than the manager remembers) keeps that version's layout.
		manager->SetStoredData(stored, CreatePartedCube(2.0f, 3)->Serialize());
		manager->ReloadAsset(stored);
		REQUIRE(manager->GetAssetState(stored) == AssetState::Loading);
		AssetMetadata other;
		other.Handle = UUID(0x3A7F);
		for (size_t change = 0; change <= AssetManagerBase::c_MaxContentChanges; change++)
			manager->AddMemoryAsset(Material::Create(), other);
		CHECK(renderScene.Sync(manager) == 0);
		CHECK(renderScene.GetStats().LastSyncMeshChecks == 1);
		CHECK(renderScene.GetInstanceCount(proxy) == 1);
		manager->Update();
		REQUIRE(manager->GetAssetState(stored) == AssetState::Ready);
		CHECK(renderScene.Sync(manager) == 1);
		CHECK(renderScene.GetInstanceCount(proxy) == 3);
		CHECK(renderScene.GetWorldBounds(proxy).Max.x == doctest::Approx(101.0f));
		CheckProxies(renderScene);
	}

	TEST_CASE("Meshes whose bounds the asset database recorded are placed with them before they load, and follow new records")
	{
		const Ref<FakeAssetManager> manager = CreateRef<FakeAssetManager>();
		// Recorded as imports and packs record a model's meshes, before anything draws them.
		const AssetHandle recorded = manager->AddStored(0x5E40, AssetType::Mesh, CreatePartedCube(4.0f, 2)->Serialize());
		manager->SetDependencies(recorded, {}, AABB(glm::vec3(-2.0f), glm::vec3(2.0f)));
		const AssetHandle late = manager->AddStored(0x5E41, AssetType::Mesh, MeshFactory::CreateCube()->Serialize());
		Scene scene("Recorded");
		Entity entity = AddCube(scene, glm::vec3(100.0f, 0.0f, 0.0f), UUID::Null(), recorded);
		Entity lateUser = AddCube(scene, glm::vec3(-100.0f, 0.0f, 0.0f), UUID::Null(), late);
		RenderScene& renderScene = RenderScene::Get(scene);
		renderScene.Sync(manager);
		const uint32_t proxy = renderScene.FindProxy(entity.GetHandle());
		const uint32_t slot = renderScene.GetMeshSlot(proxy);
		const uint32_t lateProxy = renderScene.FindProxy(lateUser.GetHandle());
		const uint32_t lateSlot = renderScene.GetMeshSlot(lateProxy);

		// Not loaded and not requested: the recorded bounds place its proxy where the mesh is, so renderers cull it like a
		// loaded one. Instances wait for the layout (the submeshes and their materials).
		CHECK(manager->GetAssetState(recorded) == AssetState::Unloaded);
		CHECK_FALSE(renderScene.IsMeshLayoutKnown(slot));
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::MeshBounds) != 0);
		CHECK(renderScene.GetWorldBounds(proxy).Min.x == doctest::Approx(98.0f));
		CHECK(renderScene.GetWorldBounds(proxy).Max.x == doctest::Approx(102.0f));
		CHECK(renderScene.GetInstanceCount(proxy) == 0);
		std::vector<uint32_t> found;
		renderScene.GetSpatialIndex().QuerySphere(glm::vec3(101.5f, 0.0f, 0.0f), 0.25f, found);
		CHECK(found == std::vector<uint32_t> { proxy });
		// The other mesh has no record yet: its bounds are unknown.
		REQUIRE(renderScene.GetMeshesWithoutBounds().size() == 1);
		CHECK(renderScene.GetMeshesWithoutBounds()[0] == lateSlot);
		CheckProxies(renderScene);

		// Bounds recorded after the scene was synced (an import that finished later): the next sync reads them. The same
		// record again changes nothing.
		manager->SetDependencies(late, {}, AABB(glm::vec3(-0.5f), glm::vec3(0.5f)));
		CHECK(renderScene.Sync(manager) == 1);
		CHECK(renderScene.GetStats().LastSyncMeshChecks == 1);
		CHECK(renderScene.GetMeshesWithoutBounds().empty());
		CHECK((renderScene.GetProxyFlags(lateProxy) & RenderProxyFlags::MeshBounds) != 0);
		CHECK(renderScene.GetWorldBounds(lateProxy).Max.x == doctest::Approx(-99.5f));
		manager->SetDependencies(late, {}, AABB(glm::vec3(-0.5f), glm::vec3(0.5f)));
		CHECK(renderScene.Sync(manager) == 0);
		CHECK(renderScene.GetStats().LastSyncMeshChecks == 0);
		CheckProxies(renderScene);

		// Loaded: the layout (the mesh's own bounds and its submeshes) takes the record's place.
		REQUIRE(manager->LoadAssetSync(recorded));
		CHECK(renderScene.Sync(manager) == 1);
		CHECK(renderScene.IsMeshLayoutKnown(slot));
		CHECK(renderScene.GetInstanceCount(proxy) == 2);
		CHECK(renderScene.GetWorldBounds(proxy).Max.x == doctest::Approx(102.0f));
		CheckProxies(renderScene);

		// A record without bounds: unknown again, until the mesh loads.
		manager->SetDependencies(late, {}, std::nullopt);
		CHECK(renderScene.Sync(manager) == 1);
		REQUIRE(renderScene.GetMeshesWithoutBounds().size() == 1);
		CHECK(renderScene.GetMeshesWithoutBounds()[0] == lateSlot);
		CHECK((renderScene.GetProxyFlags(lateProxy) & RenderProxyFlags::MeshBounds) == 0);
		CheckProxies(renderScene);
	}

	TEST_CASE("Signals queue each entity once, however often they come before the next sync")
	{
		Scene scene("Pending");
		std::vector<Entity> cubes;
		for (int index = 0; index < 10; index++)
			cubes.push_back(AddCube(scene, glm::vec3(static_cast<float>(index), 0.0f, 0.0f)));
		RenderScene& renderScene = RenderScene::Get(scene);
		CHECK(renderScene.GetPendingCount() == 10);
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetPendingCount() == 0);

		// A scene that changes while nothing renders it (a hidden view, an agent capturing now and then).
		for (int round = 0; round < 1000; round++)
		{
			for (Entity cube : cubes)
				cube.MarkModified<MeshRendererComponent>();
			// Created and destroyed between syncs: gone from the queue with its component.
			scene.DestroyEntity(AddCube(scene, glm::vec3(0.0f)));
		}
		CHECK(renderScene.GetPendingCount() == 10);
		cubes[3].RemoveComponent<MeshRendererComponent>();
		CHECK(renderScene.GetPendingCount() == 9);

		Entity added = AddCube(scene, glm::vec3(20.0f));
		CHECK(renderScene.GetPendingCount() == 10);
		CHECK(renderScene.Sync(nullptr) == 1); // The new proxy; the others did not change
		CHECK(renderScene.GetPendingCount() == 0);
		CHECK(renderScene.GetStats().Proxies == 10);
		CHECK(renderScene.FindProxy(added.GetHandle()) != RenderScene::c_InvalidIndex);
		CheckProxies(renderScene);
	}

	TEST_CASE("A scene's proxies are built a chunk at a time before it is shown, and match the scene however it changes meanwhile")
	{
		// A loaded scene waiting to replace the shown one (SceneLoadOptions::Prepare): nothing renders it yet.
		Scene scene("Loaded");
		const uint32_t count = RenderScene::c_PrepareChunk * 3 + 7;
		std::vector<Entity> cubes;
		for (uint32_t index = 0; index < count; index++)
			cubes.push_back(AddCube(scene, glm::vec3(static_cast<float>(index % 100) * 2.0f, 0.5f, static_cast<float>(index / 100) * 2.0f)));
		RenderScene& renderScene = RenderScene::Get(scene);
		REQUIRE(renderScene.GetPendingCount() == count);

		// A partial sync reads only as many queued entities as it is given; the others wait in the queue.
		CHECK(renderScene.Sync(nullptr, 5) == 5);
		CHECK(renderScene.GetStats().Proxies == 5);
		CHECK(renderScene.GetPendingCount() == count - 5);
		const size_t built = static_cast<size_t>(std::count_if(cubes.begin(), cubes.end(),
			[&](Entity cube) { return renderScene.FindProxy(cube.GetHandle()) != RenderScene::c_InvalidIndex; }));
		CHECK(built == 5);

		// Without a budget each call reads one chunk, so a frame's share stays bounded however large the scene is.
		CHECK_FALSE(RenderScene::Prepare(scene, nullptr, std::chrono::microseconds(0)));
		CHECK(renderScene.GetStats().Proxies == 5 + RenderScene::c_PrepareChunk);
		CHECK(renderScene.GetPendingCount() == count - 5 - RenderScene::c_PrepareChunk);

		// The scene changes between the calls: a built proxy moves, a queued entity gets another material, another is destroyed,
		// and a new one is created.
		std::vector<Entity> withProxy;
		std::vector<Entity> withoutProxy;
		for (Entity cube : cubes)
			(renderScene.FindProxy(cube.GetHandle()) != RenderScene::c_InvalidIndex ? withProxy : withoutProxy).push_back(cube);
		REQUIRE(withoutProxy.size() >= 2);
		Entity moved = withProxy.front();
		moved.GetComponent<TransformComponent>().Translation.y = 7.0f;
		moved.MarkModified<TransformComponent>();
		Entity queued = withoutProxy[0];
		queued.GetComponent<MeshRendererComponent>().Material = UUID(0x4242);
		queued.MarkModified<MeshRendererComponent>();
		scene.DestroyEntity(withoutProxy[1]);
		const Entity added = AddCube(scene, glm::vec3(-5.0f));
		CHECK_FALSE(RenderScene::Prepare(scene, nullptr, std::chrono::microseconds(0)));
		const auto start = std::chrono::steady_clock::now();
		while (!RenderScene::Prepare(scene, nullptr, std::chrono::microseconds(0)))
			REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));

		// Built as one sync would build them.
		CHECK(renderScene.GetPendingCount() == 0);
		CHECK(renderScene.GetStats().Proxies == count); // One destroyed, one added
		CHECK(renderScene.GetWorldBounds(renderScene.FindProxy(moved.GetHandle())).Min.y == doctest::Approx(6.0f));
		CHECK(renderScene.GetMaterialOverride(renderScene.FindProxy(queued.GetHandle())) == UUID(0x4242));
		CHECK(renderScene.FindProxy(added.GetHandle()) != RenderScene::c_InvalidIndex);
		CheckProxies(renderScene);
		// Prepared: the frame that shows it finds nothing to build.
		CHECK(RenderScene::Prepare(scene, nullptr, std::chrono::microseconds(0)));
		CHECK(renderScene.Sync(nullptr) == 0);
	}

	TEST_CASE("Mesh and material slots are shared by their users and released with the last one")
	{
		Scene scene("Slots");
		Entity first = AddCube(scene, glm::vec3(0.0f), UUID(0xB1));
		Entity second = AddCube(scene, glm::vec3(2.0f), UUID(0xB1));
		Entity third = AddCube(scene, glm::vec3(4.0f), UUID(0xB2));
		RenderScene& renderScene = RenderScene::Get(scene);
		renderScene.Sync(nullptr);
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
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetStats().Proxies == 2);
		CHECK(renderScene.GetStats().Materials == 2); // 0xB2 went with its only user
		CHECK(renderScene.GetInstances()[renderScene.GetFirstInstance(renderScene.FindProxy(third.GetHandle()))].MaterialIndex
			== renderScene.GetSubmeshMaterialSlots(renderScene.GetMeshSlot(secondProxy))[0]);
		CheckProxies(renderScene);

		scene.DestroyEntity(second);
		scene.DestroyEntity(third);
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetStats().Proxies == 0);
		CHECK(renderScene.GetStats().Meshes == 0);
		CHECK(renderScene.GetStats().Materials == 0);
		CHECK(renderScene.GetStats().IndexedProxies == 0);
		CHECK(renderScene.GetStats().InstanceSlots == 0);
		CheckProxies(renderScene);
	}

	TEST_CASE("A material's users are found by their override and through the layouts of their meshes, and follow edits")
	{
		Scene scene("Users");
		constexpr AssetHandle c_Override(0xC1);
		constexpr AssetHandle c_Parted(0xC2);
		Entity overridden = AddCube(scene, glm::vec3(0.0f), c_Override);
		Entity plain = AddCube(scene, glm::vec3(2.0f));
		Entity parted = AddCube(scene, glm::vec3(4.0f), UUID::Null(), c_Parted);
		Entity partedOverridden = AddCube(scene, glm::vec3(6.0f), c_Override, c_Parted);
		RenderScene& renderScene = RenderScene::Get(scene);
		renderScene.Sync(nullptr);
		// The layouts give the meshes their submesh materials: the cube's default material, the parted cube's 0xA1-0xA3.
		const uint32_t cubeSlot = renderScene.FindMeshSlot(BuiltinAssets::CubeMesh);
		const uint32_t partedSlot = renderScene.FindMeshSlot(c_Parted);
		REQUIRE(cubeSlot != RenderScene::c_InvalidIndex);
		REQUIRE(partedSlot != RenderScene::c_InvalidIndex);
		renderScene.SetMeshLayout(cubeSlot, *MeshFactory::CreateCube(), 1);
		renderScene.SetMeshLayout(partedSlot, *CreatePartedCube(1.0f, 3), 1);
		CheckProxies(renderScene);

		const auto users = [&](std::initializer_list<AssetHandle> materials)
		{
			std::vector<uint32_t> slots;
			for (const AssetHandle material : materials)
			{
				const uint32_t slot = renderScene.FindMaterialSlot(material);
				REQUIRE(slot != RenderScene::c_InvalidIndex);
				slots.push_back(slot);
			}
			std::vector<uint32_t> proxies;
			renderScene.CollectMaterialUsers(slots, proxies);
			std::vector<entt::entity> entities;
			for (const uint32_t proxy : proxies)
				entities.push_back(renderScene.GetEntity(proxy));
			std::sort(entities.begin(), entities.end());
			return entities;
		};
		const auto sorted = [](std::vector<Entity> entities)
		{
			std::vector<entt::entity> handles;
			for (const Entity entity : entities)
				handles.push_back(entity.GetHandle());
			std::sort(handles.begin(), handles.end());
			return handles;
		};
		CHECK(users({ c_Override }) == sorted({ overridden, partedOverridden }));
		CHECK(users({ BuiltinAssets::DefaultMaterial }) == sorted({ plain }));
		CHECK(users({ AssetHandle(0xA2) }) == sorted({ parted }));
		// A mesh using several of the slots names its users once per slot.
		CHECK(users({ AssetHandle(0xA1), AssetHandle(0xA3) }) == sorted({ parted, parted }));

		// Without its override the parted cube's other user draws the parted materials; destroyed, the plain one draws none.
		partedOverridden.GetComponent<MeshRendererComponent>().Material = UUID::Null();
		partedOverridden.MarkModified<MeshRendererComponent>();
		scene.DestroyEntity(plain);
		renderScene.Sync(nullptr);
		CheckProxies(renderScene);
		CHECK(users({ c_Override }) == sorted({ overridden }));
		CHECK(users({ AssetHandle(0xA2) }) == sorted({ parted, partedOverridden }));
		CHECK(users({ BuiltinAssets::DefaultMaterial }).empty());

		// A new layout links the mesh to its new materials only.
		renderScene.SetMeshLayout(partedSlot, *CreatePartedCube(1.0f, 2, 0xB1), 2);
		CheckProxies(renderScene);
		CHECK(renderScene.FindMaterialSlot(AssetHandle(0xA2)) == RenderScene::c_InvalidIndex);
		CHECK(users({ AssetHandle(0xB2) }) == sorted({ parted, partedOverridden }));
		scene.DestroyEntity(parted);
		scene.DestroyEntity(partedOverridden);
		renderScene.Sync(nullptr);
		CheckProxies(renderScene);
		CHECK(renderScene.FindMaterialSlot(AssetHandle(0xB2)) == RenderScene::c_InvalidIndex);
		CHECK(users({ c_Override }) == sorted({ overridden }));
	}

	TEST_CASE("A copied scene gets a clone of its render scene with the same indices, and each diverges on its own")
	{
		Ref<Scene> source = CreateRef<Scene>("Source");
		std::vector<Entity> cubes;
		for (uint32_t index = 0; index < 50; index++)
			cubes.push_back(AddCube(*source, glm::vec3(static_cast<float>(index) * 2.0f, 0.0f, 0.0f), index % 3 == 0 ? UUID(0xD1) : UUID::Null()));
		RenderScene& renderScene = RenderScene::Get(*source);
		renderScene.Sync(nullptr);
		renderScene.SetMeshLayout(renderScene.FindMeshSlot(BuiltinAssets::CubeMesh), *MeshFactory::CreateCube(), 1);
		// A change the source's render scene has not read yet is read by the clone too.
		cubes[7].GetComponent<TransformComponent>().Translation.y = 5.0f;
		cubes[7].MarkModified<TransformComponent>();
		source->UpdateWorldTransforms();

		// A scene without a render scene is copied without one.
		Ref<Scene> plain = CreateRef<Scene>("Plain");
		AddCube(*plain, glm::vec3(0.0f));
		CHECK(RenderScene::Find(*Scene::Copy(plain)) == nullptr);

		Ref<Scene> copy = Scene::Copy(source);
		RenderScene* clone = RenderScene::Find(*copy);
		REQUIRE(clone != nullptr);
		CHECK(clone->GetId() != renderScene.GetId());
		CHECK(clone->GetCloneSource() == renderScene.GetId());
		CHECK(clone->GetCloneVersions().Proxy == renderScene.GetProxyVersion());
		CHECK(clone->GetProxyCapacity() == renderScene.GetProxyCapacity());
		CHECK(clone->GetStats().Proxies == renderScene.GetStats().Proxies);
		CHECK(clone->GetStats().Materials == renderScene.GetStats().Materials);
		CHECK(clone->GetStats().CloneMs >= 0.0f);
		for (const Entity& cube : cubes)
		{
			const uint32_t proxy = renderScene.FindProxy(cube.GetHandle());
			REQUIRE(proxy != RenderScene::c_InvalidIndex);
			CHECK(clone->FindProxy(cube.GetHandle()) == proxy);
			CHECK(clone->GetFirstInstance(proxy) == renderScene.GetFirstInstance(proxy));
			CHECK(clone->GetMeshSlot(proxy) == renderScene.GetMeshSlot(proxy));
		}
		// Its first sync builds no proxy, reads the change the source had not, and leaves it valid for the copy.
		CHECK(clone->Sync(nullptr) <= 1);
		CheckProxies(*clone);
		CHECK(clone->GetWorldBounds(clone->FindProxy(cubes[7].GetHandle())).Min.y == doctest::Approx(4.5f));

		// Each changes on its own: an entity moved in the copy, another destroyed in the source, one created in each.
		Entity copied = copy->GetEntityByUUID(cubes[3].GetUUID());
		copied.GetComponent<TransformComponent>().Translation.z = -10.0f;
		copied.MarkModified<TransformComponent>();
		const UUID destroyed = cubes[4].GetUUID();
		source->DestroyEntity(cubes[4]);
		AddCube(*copy, glm::vec3(0.0f, 20.0f, 0.0f), UUID(0xD2));
		AddCube(*source, glm::vec3(0.0f, -20.0f, 0.0f));
		renderScene.Sync(nullptr);
		clone->Sync(nullptr);
		CheckProxies(renderScene);
		CheckProxies(*clone);
		CHECK(renderScene.GetWorldBounds(renderScene.FindProxy(cubes[3].GetHandle())).Min.z == doctest::Approx(-0.5f));
		CHECK(clone->GetWorldBounds(clone->FindProxy(copied.GetHandle())).Min.z == doctest::Approx(-10.5f));
		CHECK(clone->FindProxy(copy->GetEntityByUUID(destroyed).GetHandle()) != RenderScene::c_InvalidIndex);
		CHECK(renderScene.GetStats().Proxies == 50);
		CHECK(clone->GetStats().Proxies == 51);
		CHECK(clone->GetStats().Materials == renderScene.GetStats().Materials + 1);

		// A copy of the copy clones the clone: copied from a synced render scene, it is valid at once and its sync updates no
		// proxy.
		Ref<Scene> second = Scene::Copy(copy);
		RenderScene* secondClone = RenderScene::Find(*second);
		REQUIRE(secondClone != nullptr);
		CHECK(secondClone->GetCloneSource() == clone->GetId());
		CheckProxies(*secondClone);
		CHECK(secondClone->Sync(nullptr) == 0);
		CheckProxies(*secondClone);
	}

	TEST_CASE("Renderers that fall behind the bounded change log upload everything")
	{
		Scene scene("Changes");
		std::vector<Entity> cubes;
		for (int index = 0; index < 3000; index++)
			cubes.push_back(AddCube(scene, glm::vec3(static_cast<float>(index), 0.0f, 0.0f)));
		RenderScene& renderScene = RenderScene::Get(scene);
		renderScene.Sync(nullptr);
		renderScene.SetMeshLayout(renderScene.GetMeshesWithoutBounds()[0], *MeshFactory::CreateCube());
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
			CHECK(renderScene.Sync(nullptr) == cubes.size());
			moves += cubes.size();
		}
		CHECK_FALSE(renderScene.GetInstanceChanges(start, changes));
		CHECK(changes.empty());
		CHECK(renderScene.GetInstanceChanges(recent, changes));
		CHECK(changes.size() == cubes.size());
	}

	TEST_CASE("The proxy change log names the proxies that changed since a reader's version, each once, until it overflows")
	{
		Scene scene("ProxyChanges");
		std::vector<Entity> cubes;
		for (int index = 0; index < 100; index++)
			cubes.push_back(AddCube(scene, glm::vec3(static_cast<float>(index) * 2.0f, 0.0f, 0.0f)));
		RenderScene& renderScene = RenderScene::Get(scene);
		renderScene.Sync(nullptr);
		renderScene.SetMeshLayout(renderScene.GetMeshesWithoutBounds()[0], *MeshFactory::CreateCube());

		// A reader that takes the version without reading the log (it reads every proxy itself) is told of the next change.
		const uint64_t taken = renderScene.GetProxyVersion();
		cubes[3].GetComponent<TransformComponent>().Translation.y += 1.0f;
		cubes[3].MarkModified<TransformComponent>();
		CHECK(renderScene.Sync(nullptr) == 1);
		std::vector<uint32_t> changes;
		REQUIRE(renderScene.GetProxyChanges(taken, changes));
		CHECK(changes == std::vector<uint32_t> { renderScene.FindProxy(cubes[3].GetHandle()) });

		ProxyLogReader reader;
		reader.Update(renderScene);
		CHECK(reader.FullReads == 0); // From version 0: everything since the scene began is in the log
		reader.Check(renderScene);

		// Nothing changed: nothing to read.
		const uint64_t quiet = renderScene.GetProxyVersion();
		changes.clear();
		CHECK(renderScene.Sync(nullptr) == 0);
		CHECK(renderScene.GetProxyVersion() == quiet);
		CHECK(renderScene.GetProxyChanges(quiet, changes));
		CHECK(changes.empty());

		// A cube moved twice before anyone read the log is named once.
		const uint32_t moved = renderScene.FindProxy(cubes[7].GetHandle());
		for (int step = 0; step < 2; step++)
		{
			cubes[7].GetComponent<TransformComponent>().Translation.y += 1.0f;
			cubes[7].MarkModified<TransformComponent>();
			CHECK(renderScene.Sync(nullptr) == 1);
		}
		REQUIRE(renderScene.GetProxyChanges(quiet, changes));
		CHECK(changes == std::vector<uint32_t> { moved });
		// Read: the next change is logged again, for readers that read before it.
		const uint64_t afterMove = renderScene.GetProxyVersion();
		cubes[7].GetComponent<MeshRendererComponent>().CastShadows = false;
		cubes[7].MarkModified<MeshRendererComponent>();
		renderScene.Sync(nullptr);
		changes.clear();
		REQUIRE(renderScene.GetProxyChanges(afterMove, changes));
		CHECK(changes == std::vector<uint32_t> { moved });

		// A destroyed proxy is named (with no entity), and so is the proxy that takes its index.
		const uint32_t destroyed = renderScene.FindProxy(cubes[20].GetHandle());
		scene.DestroyEntity(cubes[20]);
		renderScene.Sync(nullptr);
		reader.Update(renderScene);
		reader.Check(renderScene);
		CHECK((reader.Records[destroyed].Entity == entt::null));
		CHECK(reader.Records[destroyed].InstanceCount == 0);
		Entity replacement = AddCube(scene, glm::vec3(0.0f, 10.0f, 0.0f));
		renderScene.Sync(nullptr);
		CHECK(renderScene.FindProxy(replacement.GetHandle()) == destroyed);
		reader.Update(renderScene);
		reader.Check(renderScene);
		CHECK(reader.Records[destroyed].Entity == replacement.GetHandle());

		// More changes than the log keeps since a reader's version: it reads everything.
		const uint64_t behind = renderScene.GetProxyVersion();
		ProxyLogReader late;
		late.Update(renderScene);
		late.Version = behind;
		uint64_t logged = 0;
		while (logged <= RenderScene::c_MaxProxyChanges)
		{
			for (Entity cube : cubes)
			{
				if (!cube.IsValid())
					continue;
				cube.GetComponent<TransformComponent>().Translation.z += 1.0f;
				cube.MarkModified<TransformComponent>();
			}
			const uint32_t updated = renderScene.Sync(nullptr);
			// Every sync's changes are read, so each one logs its proxies again.
			changes.clear();
			renderScene.GetProxyChanges(renderScene.GetProxyVersion(), changes);
			logged += updated;
		}
		changes.clear();
		CHECK_FALSE(renderScene.GetProxyChanges(behind, changes));
		CHECK(changes.empty());
		late.Update(renderScene);
		CHECK(late.FullReads == 1);
		late.Check(renderScene);
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
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetStats().Proxies == 4);
		CHECK(renderScene.GetStats().IndexedProxies == 1);
		CHECK((renderScene.GetProxyFlags(renderScene.FindProxy(mirrored.GetHandle())) & RenderProxyFlags::Mirrored) != 0);
		CHECK((renderScene.GetProxyFlags(renderScene.FindProxy(inactive.GetHandle())) & RenderProxyFlags::Active) == 0);

		inactive.SetActive(true);
		CHECK(renderScene.Sync(nullptr) == 1);
		CHECK(renderScene.GetStats().IndexedProxies == 2);
		CheckProxies(renderScene);
	}
}
