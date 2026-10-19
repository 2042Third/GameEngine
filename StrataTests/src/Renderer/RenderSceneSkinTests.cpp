#include <doctest/doctest.h>

#include "Animation/SkinnedMeshTestUtils.h"
#include "Animation/TestSkinningPoses.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

#include <cstring>
#include <string>
#include <utility>
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

	struct SkinFixture
	{
		Scene SceneData { "Skins" };
		RenderScene& Render = RenderScene::Get(SceneData);
		Ref<Mesh> Tube = CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 2, .Sides = 6, .JointLength = 1.0f, .Radius = 0.25f });
		AssetHandle MeshHandle = UUID(0x5C1A);
		TestSkinningPoses Poses;

		// An entity with the tube and a SkinComponent naming `animator` (null: the entity itself).
		Entity AddSkinned(const glm::vec3& position, UUID animator = UUID::Null(), Entity parent = {})
		{
			Entity entity = parent ? SceneData.CreateChildEntity(parent, "Skinned") : SceneData.CreateEntity("Skinned");
			entity.GetComponent<TransformComponent>().Translation = position;
			entity.AddComponent<MeshRendererComponent>().Mesh = MeshHandle;
			entity.AddComponent<SkinComponent>().Animator = animator.IsValid() ? animator : entity.GetComponent<IDComponent>().ID;
			return entity;
		}

		// Syncs without an asset manager, then gives the mesh its layout (as a loaded mesh would).
		void SyncWithLayout()
		{
			Render.Sync(nullptr);
			for (uint32_t slot = 0; slot < Render.GetMeshSlotCapacity(); slot++)
			{
				if (Render.GetMeshHandle(slot) == MeshHandle)
					Render.SetMeshLayout(slot, *Tube);
			}
			Render.Sync(nullptr);
		}
	};

}

TEST_SUITE("Renderer.RenderScene")
{
	TEST_CASE("Skin proxies are posed by the scene's poses, with palettes and bounds that follow poses and placements")
	{
		SkinFixture fixture;
		Scene& scene = fixture.SceneData;
		RenderScene& renderScene = fixture.Render;
		Entity arm = fixture.AddSkinned(glm::vec3(2.0f, 0.0f, 0.0f));
		fixture.SyncWithLayout();
		const uint32_t proxy = renderScene.FindProxy(arm.GetHandle());
		REQUIRE(proxy != RenderScene::c_InvalidIndex);
		// Without poses: the bind pose, but known as a Skin proxy with its animator.
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Skin) != 0);
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Skinned) == 0);
		CHECK(renderScene.GetStats().SkinProxies == 1);
		CHECK(renderScene.GetStats().PosedProxies == 0);
		CHECK(renderScene.GetLocalBounds(proxy).Max.y == doctest::Approx(3.0f));
		CheckProxies(renderScene);

		// A pose: the palette (one matrix per skin joint) and the posed bounds (the chain bent at its elbow lies along -X).
		fixture.Poses.Poses[arm.GetHandle()] = BentChain(3, 1);
		REQUIRE(scene.RegisterService<ISkinningPoses>(fixture.Poses));
		renderScene.Sync(nullptr);
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Skinned) != 0);
		CHECK(renderScene.GetSkinAnimator(proxy) == arm.GetHandle());
		CHECK(renderScene.GetPaletteCount(proxy) == 3);
		CHECK(renderScene.GetStats().PaletteJoints == 3);
		CHECK(renderScene.GetStats().LastSyncPoses == 1);
		const AABB posed = renderScene.GetLocalBounds(proxy);
		CHECK(posed.Min.x == doctest::Approx(-2.0f)); // The two joints above the elbow turned along -X
		CHECK(posed.Max.y < 1.5f);
		CHECK(renderScene.GetWorldBounds(proxy).Min.x == doctest::Approx(0.0f));
		const RenderData::InstanceData& instance = renderScene.GetInstances()[renderScene.GetFirstInstance(proxy)];
		CHECK((instance.Flags & RenderData::c_InstanceSkinned) != 0);
		CHECK(instance.SkinPalette == renderScene.GetPaletteFirst(proxy));
		std::vector<std::pair<uint32_t, uint32_t>> changes;
		CHECK(renderScene.GetPaletteChanges(0, changes));
		CHECK(changes == std::vector<std::pair<uint32_t, uint32_t>> { { renderScene.GetPaletteFirst(proxy), 3u } });
		CheckProxies(renderScene);

		// Nothing changed: no palette is computed and none is logged.
		const uint64_t paletteVersion = renderScene.GetPaletteVersion();
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetStats().LastSyncPoses == 0);
		CHECK(renderScene.GetPaletteVersion() == paletteVersion);

		// Moving the entity that is its own animator moves the pose with it: the palette stays, the bounds move.
		arm.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 0.0f, 0.0f);
		arm.MarkModified<TransformComponent>();
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetStats().LastSyncPoses == 0);
		CHECK(renderScene.GetPaletteVersion() == paletteVersion);
		CHECK(renderScene.GetWorldBounds(proxy).Min.x == doctest::Approx(3.0f));
		CheckProxies(renderScene);

		// A new pose computes the palette again.
		fixture.Poses.Poses[arm.GetHandle()] = BentChain(3, 0);
		fixture.Poses.Version++;
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetStats().LastSyncPoses == 1);
		CHECK(renderScene.GetPaletteVersion() == paletteVersion + 1);
		CHECK(renderScene.GetLocalBounds(proxy).Min.x == doctest::Approx(-3.0f));
		CheckProxies(renderScene);

		// A pose without every joint the skin names, an inactive entity, a missing animator: the bind pose.
		fixture.Poses.Poses[arm.GetHandle()].resize(2);
		fixture.Poses.Version++;
		renderScene.Sync(nullptr);
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Skinned) == 0);
		CHECK(renderScene.GetStats().PaletteJoints == 0);
		CHECK(renderScene.GetLocalBounds(proxy).Max.y == doctest::Approx(3.0f));
		CheckProxies(renderScene);
		fixture.Poses.Poses[arm.GetHandle()] = BentChain(3, 1);
		fixture.Poses.Version++;
		arm.AddComponent<InactiveComponent>();
		renderScene.Sync(nullptr);
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Skinned) == 0);
		arm.RemoveComponent<InactiveComponent>();
		renderScene.Sync(nullptr);
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Skinned) != 0);
		arm.GetComponent<SkinComponent>().Animator = UUID(0xDEAD);
		arm.MarkModified<SkinComponent>();
		renderScene.Sync(nullptr);
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Skinned) == 0);
		CHECK((renderScene.GetSkinAnimator(proxy) == entt::null));
		CheckProxies(renderScene);

		// Removing the SkinComponent, or the entity, drops the skin state.
		arm.GetComponent<SkinComponent>().Animator = arm.GetComponent<IDComponent>().ID;
		arm.MarkModified<SkinComponent>();
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetStats().PosedProxies == 1);
		arm.RemoveComponent<SkinComponent>();
		renderScene.Sync(nullptr);
		CHECK((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Skin) == 0);
		CHECK(renderScene.GetStats().SkinProxies == 0);
		CHECK(renderScene.GetStats().PosedProxies == 0);
		CHECK(renderScene.GetStats().PaletteJoints == 0);
		CheckProxies(renderScene);
		arm.AddComponent<SkinComponent>().Animator = arm.GetComponent<IDComponent>().ID;
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetStats().PosedProxies == 1);
		scene.DestroyEntity(arm);
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetStats().SkinProxies == 0);
		CHECK(renderScene.GetStats().PosedProxies == 0);
		CHECK_FALSE(renderScene.HasSkinProxies());
		CheckProxies(renderScene);
		scene.UnregisterService<ISkinningPoses>(fixture.Poses);
	}

	TEST_CASE("A skinned mesh below its animator is posed in the animator's space wherever its own entity is")
	{
		SkinFixture fixture;
		Scene& scene = fixture.SceneData;
		RenderScene& renderScene = fixture.Render;
		Entity animator = scene.CreateEntity("Animator");
		animator.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 10.0f);
		// The mesh entity is offset from its animator: the pose still places the vertices relative to the animator.
		Entity mesh = fixture.AddSkinned(glm::vec3(1.0f, 2.0f, 0.0f), animator.GetComponent<IDComponent>().ID, animator);
		fixture.Poses.Poses[animator.GetHandle()] = BentChain(3, 3); // Unbent: the bind pose of the chain
		REQUIRE(scene.RegisterService<ISkinningPoses>(fixture.Poses));
		fixture.SyncWithLayout();
		const uint32_t proxy = renderScene.FindProxy(mesh.GetHandle());
		REQUIRE((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Skinned) != 0);
		// World bounds: around the animator's origin, not the mesh entity's.
		const AABB world = renderScene.GetWorldBounds(proxy);
		CHECK(world.Min.x == doctest::Approx(-0.25f));
		CHECK(world.Min.y == doctest::Approx(0.0f));
		CHECK(world.Max.y == doctest::Approx(3.0f));
		CHECK(world.GetCenter().z == doctest::Approx(10.0f));
		CheckProxies(renderScene);

		// Moving the animator moves the mesh's vertices with it (the mesh entity follows as its child); moving the mesh
		// entity alone leaves them in place, through a new palette.
		const uint64_t version = renderScene.GetPaletteVersion();
		mesh.GetComponent<TransformComponent>().Translation = glm::vec3(-3.0f, 0.0f, 0.0f);
		mesh.MarkModified<TransformComponent>();
		renderScene.Sync(nullptr);
		CHECK(renderScene.GetStats().LastSyncPoses == 1);
		CHECK(renderScene.GetPaletteVersion() == version + 1);
		CHECK(renderScene.GetWorldBounds(proxy).Min.x == doctest::Approx(-0.25f));
		CheckProxies(renderScene);
		scene.UnregisterService<ISkinningPoses>(fixture.Poses);
	}

	TEST_CASE("A posed proxy computes its palette again for another skinned mesh with as many joints, whatever the pose's version")
	{
		// Meshes of one skin (outfits, body parts) have the same joints but other vertices and inverse bind matrices: the
		// palette and the posed bounds come from the mesh as much as from the pose.
		SkinFixture fixture;
		Scene& scene = fixture.SceneData;
		RenderScene& renderScene = fixture.Render;
		const Ref<Mesh> wide = CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 2, .Sides = 6, .JointLength = 1.5f, .Radius = 0.5f });
		const AssetHandle wideHandle = UUID(0x5C1B);
		auto setLayouts = [&]()
		{
			for (uint32_t slot = 0; slot < renderScene.GetMeshSlotCapacity(); slot++)
			{
				if (renderScene.GetMeshHandle(slot) == fixture.MeshHandle)
					renderScene.SetMeshLayout(slot, *fixture.Tube);
				else if (renderScene.GetMeshHandle(slot) == wideHandle)
					renderScene.SetMeshLayout(slot, *wide);
			}
		};
		Entity arm = fixture.AddSkinned(glm::vec3(0.0f));
		// Another entity draws the wide mesh, so its layout is known when the arm switches to it.
		Entity other = fixture.AddSkinned(glm::vec3(10.0f, 0.0f, 0.0f));
		other.GetComponent<MeshRendererComponent>().Mesh = wideHandle;
		fixture.Poses.Poses[arm.GetHandle()] = BentChain(3, 1);
		fixture.Poses.Poses[other.GetHandle()] = BentChain(3, 1);
		REQUIRE(scene.RegisterService<ISkinningPoses>(fixture.Poses));
		renderScene.Sync(nullptr);
		setLayouts();
		renderScene.Sync(nullptr);
		const uint32_t proxy = renderScene.FindProxy(arm.GetHandle());
		REQUIRE((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Skinned) != 0);
		const AABB narrowBounds = renderScene.GetLocalBounds(proxy);
		CheckProxies(renderScene);

		// The mesh changes, the pose does not.
		arm.GetComponent<MeshRendererComponent>().Mesh = wideHandle;
		arm.MarkModified<MeshRendererComponent>();
		renderScene.Sync(nullptr);
		REQUIRE((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Skinned) != 0);
		CheckProxies(renderScene);
		CHECK(renderScene.GetLocalBounds(proxy).Max.z == doctest::Approx(2.0f * narrowBounds.Max.z));

		// A new layout of the mesh it draws (a reimport) with as many joints: the same.
		renderScene.SetMeshLayout(renderScene.GetMeshSlot(proxy), *fixture.Tube);
		renderScene.Sync(nullptr);
		CheckProxies(renderScene);
		CHECK(renderScene.GetLocalBounds(proxy).Max.z == doctest::Approx(narrowBounds.Max.z));
		scene.UnregisterService<ISkinningPoses>(fixture.Poses);
	}

	TEST_CASE("Palettes are posed again by another poses provider, whose versions say nothing of the previous one's: a play copy's clone")
	{
		// The editor poses an edited scene's skins in their rest pose (AnimationPreview), and the play copy clones its render
		// scene with those palettes; the copy's animation system numbers its poses from the start again.
		Ref<Scene> edited = CreateRef<Scene>("Edited");
		RenderScene& renderScene = RenderScene::Get(*edited);
		const Ref<Mesh> tube = CreateSkinnedTube({ .Joints = 3, .RingsPerJoint = 2, .Sides = 6, .JointLength = 1.0f, .Radius = 0.25f });
		const AssetHandle meshHandle = UUID(0x5C1C);
		Entity arm = edited->CreateEntity("Arm");
		arm.AddComponent<MeshRendererComponent>().Mesh = meshHandle;
		arm.AddComponent<SkinComponent>().Animator = arm.GetComponent<IDComponent>().ID;
		TestSkinningPoses rest;
		rest.Poses[arm.GetHandle()] = BentChain(3, 3); // Straight: no joint turns
		REQUIRE(edited->RegisterService<ISkinningPoses>(rest));
		renderScene.Sync(nullptr);
		renderScene.SetMeshLayout(renderScene.GetMeshSlot(renderScene.FindProxy(arm.GetHandle())), *tube);
		renderScene.Sync(nullptr);
		const uint32_t proxy = renderScene.FindProxy(arm.GetHandle());
		REQUIRE((renderScene.GetProxyFlags(proxy) & RenderProxyFlags::Skinned) != 0);
		CheckProxies(renderScene);
		const AABB restBounds = renderScene.GetLocalBounds(proxy);

		// Played: the copy's poses, bent, with the same version as the rest pose's.
		Ref<Scene> played = Scene::Copy(edited);
		RenderScene* clone = RenderScene::Find(*played);
		REQUIRE(clone != nullptr);
		TestSkinningPoses animation;
		animation.Poses[arm.GetHandle()] = BentChain(3, 1);
		REQUIRE(animation.Version == rest.Version);
		REQUIRE(played->RegisterService<ISkinningPoses>(animation));
		clone->Sync(nullptr);
		CheckProxies(*clone);
		CHECK(clone->GetLocalBounds(proxy).Min.x < restBounds.Min.x - 0.5f); // The bent joint leans the arm towards -X

		// The same within a scene: its provider replaced by another.
		edited->UnregisterService<ISkinningPoses>(rest);
		TestSkinningPoses replacement;
		replacement.Poses = animation.Poses;
		REQUIRE(edited->RegisterService<ISkinningPoses>(replacement));
		renderScene.Sync(nullptr);
		CheckProxies(renderScene);
		CHECK(renderScene.GetLocalBounds(proxy).Min.x < restBounds.Min.x - 0.5f);
		edited->UnregisterService<ISkinningPoses>(replacement);
		played->UnregisterService<ISkinningPoses>(animation);
	}
}
