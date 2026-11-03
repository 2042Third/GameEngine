#include <doctest/doctest.h>

#include "Strata/Renderer/GPUSceneData.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/MeshFactory.h"
#include "Strata/Renderer/RenderScene.h"
#include "Strata/Renderer/VoxelClipmap.h"
#include "Strata/Scene/Components.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

using namespace Strata;

namespace
{

	constexpr AssetHandle c_CubeMesh = AssetHandle(uint64_t(0xC0BE));
	constexpr AssetHandle c_PlaneMesh = AssetHandle(uint64_t(0x91A4E));
	constexpr AssetHandle c_GlassCubeMesh = AssetHandle(uint64_t(0x61A55));
	constexpr AssetHandle c_SkinnedMesh = AssetHandle(uint64_t(0x5C1));
	constexpr AssetHandle c_GlassMaterial = AssetHandle(uint64_t(0xA2));
	constexpr AssetHandle c_RedMaterial = AssetHandle(uint64_t(0xA3));

	// The render scene's GPU copy and asset manager as a test arranges them.
	struct TestSource : public IVoxelSceneSource
	{
		const RenderScene* Render = nullptr;
		std::function<bool(uint32_t)> Resident = [](uint32_t) { return true; };
		std::map<AssetHandle, Ref<Mesh>> Meshes;       // Loaded meshes
		std::set<AssetHandle> SkinnedArena;            // Meshes whose entry names the Skinned arena
		std::map<AssetHandle, uint32_t> MaterialFlags; // Others: available and opaque
		std::map<AssetHandle, uint32_t> MaterialAlbedo;
		uint64_t Version = 1;

		bool IsResident(uint32_t proxy) const override { return Resident(proxy); }
		uint64_t GetTablesVersion() const override { return Version; }
		uint32_t GetMeshTableEntry(uint32_t meshSlot) const override
		{
			const AssetHandle handle = Render->GetMeshHandle(meshSlot);
			if (!Meshes.contains(handle))
				return 0;
			const uint32_t entry = static_cast<uint32_t>(static_cast<uint64_t>(handle) & 0xFFFFFu) | 1u;
			return entry | (SkinnedArena.contains(handle) ? RenderData::c_CullMeshSkinnedArena : 0u);
		}
		Ref<const Mesh> GetMesh(uint32_t meshSlot) const override
		{
			const auto found = Meshes.find(Render->GetMeshHandle(meshSlot));
			return found != Meshes.end() ? found->second : nullptr;
		}
		RenderData::GIMaterial GetMaterial(uint32_t materialSlot) const override
		{
			const AssetHandle handle = Render->GetMaterialHandle(materialSlot);
			RenderData::GIMaterial material = {};
			const auto flags = MaterialFlags.find(handle);
			material.Flags = flags != MaterialFlags.end() ? flags->second : RenderData::c_CullMaterialAvailable;
			const auto albedo = MaterialAlbedo.find(handle);
			material.Albedo = albedo != MaterialAlbedo.end() ? albedo->second : 0x808080u;
			return material;
		}
	};

	// A cube whose faces split into two submeshes of their own materials: closed together.
	Ref<Mesh> CreateTwoMaterialCube(AssetHandle first, AssetHandle second)
	{
		const Ref<Mesh> cube = MeshFactory::CreateCube();
		Submesh a = cube->GetSubmeshes()[0];
		a.LODs = { MeshLOD { 0, 18 } };
		a.Material = first;
		Submesh b = a;
		b.LODs = { MeshLOD { 18, 18 } };
		b.Material = second;
		const Ref<Mesh> mesh = Mesh::Create(cube->GetPositions(), cube->GetAttributes(), cube->GetIndices(), { a, b });
		REQUIRE(mesh);
		REQUIRE(mesh->IsClosed());
		return mesh;
	}

	// A 1 m square in the xz plane facing +y (open) with levels of detail of the given errors (the same triangles).
	Ref<Mesh> CreateLODPlane(std::vector<float> errors)
	{
		std::vector<glm::vec3> positions = { { -0.5f, 0, -0.5f }, { -0.5f, 0, 0.5f }, { 0.5f, 0, 0.5f }, { 0.5f, 0, -0.5f } };
		std::vector<uint32_t> indices;
		Submesh submesh;
		submesh.VertexCount = 4;
		for (float error : errors)
		{
			submesh.LODs.push_back(MeshLOD { static_cast<uint32_t>(indices.size()), 6, error });
			indices.insert(indices.end(), { 0, 1, 2, 0, 2, 3 });
		}
		const Ref<Mesh> mesh = Mesh::Create(positions, std::vector<MeshVertexAttributes>(4), indices, { submesh });
		REQUIRE(mesh);
		REQUIRE_FALSE(mesh->IsClosed());
		return mesh;
	}

	struct Fixture
	{
		Scene SceneData { "Voxels" };
		TestSource Source;
		VoxelClipmap Clipmap { *GlobalIllumination::ComputeLayout(GlobalIlluminationLevel::High) };
		VoxelClipmapSettings Settings { 0, 0 };
		VoxelFrame Frame;
		glm::dvec3 Camera { 0.0 };

		Fixture()
		{
			Source.Meshes[c_CubeMesh] = MeshFactory::CreateCube();
		}

		Entity Add(const glm::vec3& position, AssetHandle mesh = c_CubeMesh, AssetHandle material = UUID::Null(), const glm::vec3& scale = glm::vec3(1.0f))
		{
			Entity entity = SceneData.CreateEntity("Voxelized");
			TransformComponent& transform = entity.GetComponent<TransformComponent>();
			transform.Translation = position;
			transform.Scale = scale;
			MeshRendererComponent& renderer = entity.AddComponent<MeshRendererComponent>();
			renderer.Mesh = mesh;
			renderer.Material = material;
			return entity;
		}

		RenderScene& Sync()
		{
			RenderScene& render = RenderScene::Get(SceneData);
			Source.Render = &render;
			render.Sync(nullptr);
			for (const auto& [handle, mesh] : Source.Meshes)
			{
				const uint32_t slot = render.FindMeshSlot(handle);
				if (slot != RenderScene::c_InvalidIndex)
					render.SetMeshLayout(slot, *mesh);
			}
			return render;
		}

		void Build()
		{
			RenderScene& render = Sync();
			Clipmap.BuildFrame(render, Source, Camera, Settings, Frame);
		}

		// The frame's work items of an entity's proxy.
		std::vector<RenderData::VoxelWorkItem> GetItems(Entity entity)
		{
			const RenderScene& render = RenderScene::Get(SceneData);
			const uint32_t proxy = render.FindProxy(entity);
			std::vector<RenderData::VoxelWorkItem> items;
			if (proxy == RenderScene::c_InvalidIndex)
				return items;
			const uint32_t first = render.GetFirstInstance(proxy);
			for (const RenderData::VoxelWorkItem& item : Frame.Items)
			{
				if (item.Instance >= first && item.Instance < first + render.GetInstanceCount(proxy))
					items.push_back(item);
			}
			return items;
		}

		// The frame's regions of a level, in world voxel indices.
		std::vector<VoxelBox> GetRegions(uint32_t level) const
		{
			std::vector<VoxelBox> boxes;
			const glm::i64vec3 anchor = Clipmap.GetLevel(level).Anchor;
			for (const RenderData::VoxelRegion& region : Frame.Regions)
			{
				if (static_cast<uint32_t>(region.Minimum.w) == level)
					boxes.push_back(VoxelBox { anchor + glm::i64vec3(glm::ivec3(region.Minimum)), anchor + glm::i64vec3(glm::ivec3(region.Maximum)) });
			}
			return boxes;
		}

		// What a world box's change queues in a level: its touched voxels in whole bricks, within the window.
		VoxelBox GetChangeBox(const AABB& bounds, uint32_t level) const
		{
			const VoxelLevelLayout& layout = Clipmap.GetLayout().Levels[level];
			return GlobalIllumination::SnapToBricks(VoxelClipmap::GetTouchedVoxels(bounds, layout.VoxelSize))
				.Intersection(GlobalIllumination::GetWindow(layout, Clipmap.GetLevel(level).Window));
		}

		// Whether the frame's regions of a level are exactly the given boxes (each split into pieces, in any order).
		bool RegionsCover(uint32_t level, const std::vector<VoxelBox>& expected) const
		{
			const std::vector<VoxelBox> regions = GetRegions(level);
			uint64_t covered = 0;
			for (const VoxelBox& region : regions)
			{
				if (std::none_of(expected.begin(), expected.end(), [&](const VoxelBox& box) { return box.Contains(region); }))
					return false;
				covered += region.GetVoxelCount();
			}
			uint64_t total = 0;
			for (const VoxelBox& box : expected)
				total += box.GetVoxelCount();
			return covered == total;
		}
	};

}

TEST_SUITE("Renderer.VoxelClipmap")
{
	TEST_CASE("The first frame queues every window and voxelizes what it holds")
	{
		Fixture fixture;
		Entity cube = fixture.Add(glm::vec3(2.0f, 0.5f, 3.0f));
		fixture.Build();
		CHECK(fixture.Frame.Rebuilt);
		CHECK_FALSE(fixture.Frame.Pending);
		CHECK(fixture.Frame.QueuedRegions == 0);
		// Every level's whole window, valid.
		for (uint32_t level = 0; level < 4; level++)
		{
			CAPTURE(level);
			const VoxelBox window = GlobalIllumination::GetWindow(fixture.Clipmap.GetLayout().Levels[level], fixture.Clipmap.GetLevel(level).Window);
			CHECK(fixture.RegionsCover(level, { window }));
			CHECK(fixture.Clipmap.GetLevel(level).Valid == window);
		}
		// The cube's one submesh in each region of each level that its bounds (with a voxel's margin) reach.
		const std::vector<RenderData::VoxelWorkItem> items = fixture.GetItems(cube);
		CHECK(items.size() >= 4);
		for (const RenderData::VoxelWorkItem& item : items)
		{
			CHECK((item.Flags & RenderData::c_VoxelItemClosed) != 0);
			CHECK((item.MeshSubmeshLOD >> 28) == 0);
		}
		CHECK(fixture.Frame.Triangles == 12 * items.size());
		// The batches hold one level each, their regions' voxels within the scratch, the regions' scratch offsets in order.
		for (const VoxelFrameBatch& batch : fixture.Frame.Batches)
		{
			CHECK(batch.Voxels <= fixture.Clipmap.GetLayout().BatchVoxels);
			uint32_t voxels = 0;
			for (uint32_t index = batch.FirstRegion; index < batch.FirstRegion + batch.RegionCount; index++)
			{
				const RenderData::VoxelRegion& region = fixture.Frame.Regions[index];
				CHECK(static_cast<uint32_t>(region.Minimum.w) == batch.Level);
				CHECK(static_cast<uint32_t>(region.Maximum.w) == voxels);
				const glm::ivec3 size = glm::ivec3(region.Maximum) - glm::ivec3(region.Minimum);
				voxels += static_cast<uint32_t>(size.x * size.y * size.z);
			}
			CHECK(voxels == batch.Voxels);
		}

		// A still frame does nothing.
		fixture.Build();
		CHECK(fixture.Frame.Regions.empty());
		CHECK(fixture.Frame.Items.empty());
		CHECK_FALSE(fixture.Frame.Pending);
	}

	TEST_CASE("A moved proxy queues its old and new bounds")
	{
		Fixture fixture;
		Entity cube = fixture.Add(glm::vec3(2.0f, 0.5f, 3.0f));
		fixture.Build();
		const RenderScene& render = RenderScene::Get(fixture.SceneData);
		const AABB before = render.GetWorldBounds(render.FindProxy(cube));

		// Moved: its old voxels are emptied in the frame it moved (it is out of the voxels while it moves), and the camera's move
		// in the same frame queues its slabs behind the change.
		cube.GetComponent<TransformComponent>().Translation = glm::vec3(3.0f, 0.5f, 3.0f);
		cube.MarkModified<TransformComponent>();
		fixture.Camera = glm::dvec3(0.45, 0.0, 0.0);
		fixture.Build();
		CHECK(fixture.GetItems(cube).empty());
		CHECK(fixture.Frame.MovingProxies == 1);
		for (uint32_t level = 0; level < 4; level++)
		{
			CAPTURE(level);
			const VoxelBox change = fixture.GetChangeBox(before, level);
			REQUIRE_FALSE(change.IsEmpty());
			const std::vector<VoxelBox> regions = fixture.GetRegions(level);
			REQUIRE_FALSE(regions.empty());
			// The change's pieces come first, then (level 0, whose window moved a step) the slab.
			uint64_t changeVoxels = 0;
			size_t index = 0;
			for (; index < regions.size() && change.Contains(regions[index]); index++)
				changeVoxels += regions[index].GetVoxelCount();
			CHECK(changeVoxels == change.GetVoxelCount());
			if (level == 0)
				CHECK(index < regions.size());
		}

		// It rests: c_GIMovingFrames frames later its new bounds are queued, with its items.
		fixture.Camera = glm::dvec3(0.0);
		fixture.Build();
		const AABB after = render.GetWorldBounds(render.FindProxy(cube));
		for (uint32_t frame = 2; frame < RenderData::c_GIMovingFrames; frame++)
		{
			fixture.Build();
			CHECK(fixture.GetItems(cube).empty());
		}
		fixture.Build();
		CHECK(fixture.Frame.MovingProxies == 0);
		for (uint32_t level = 0; level < 4; level++)
		{
			CAPTURE(level);
			CHECK(fixture.RegionsCover(level, { fixture.GetChangeBox(after, level) }));
		}
		CHECK_FALSE(fixture.GetItems(cube).empty());

		// A removed one queues its old bounds.
		fixture.SceneData.DestroyEntity(cube);
		fixture.Build();
		for (uint32_t level = 0; level < 4; level++)
			CHECK(fixture.RegionsCover(level, { fixture.GetChangeBox(after, level) }));
		CHECK(fixture.Frame.Items.empty());
	}

	TEST_CASE("A jump queues every window within the cut's budget, level 0 first")
	{
		Fixture fixture;
		// A field of cubes, so the windows hold triangles to spread.
		for (int x = -20; x <= 20; x += 2)
		{
			for (int z = -20; z <= 20; z += 2)
				fixture.Add(glm::vec3(static_cast<float>(x), 0.5f, static_cast<float>(z)));
		}
		fixture.Build();
		REQUIRE(fixture.Frame.QueuedRegions == 0);

		// The camera jumps 500 m (to an empty place, then back): the windows are queued whole, taken within the budget.
		fixture.Settings = VoxelClipmapSettings { 100, 2000 };
		fixture.Camera = glm::dvec3(500.0, 0.0, 0.0);
		fixture.Build();
		fixture.Camera = glm::dvec3(0.0);
		fixture.Build();
		CHECK(fixture.Frame.Pending);
		CHECK(fixture.Frame.Triangles <= 2000);
		// Level 0 first.
		REQUIRE_FALSE(fixture.Frame.Regions.empty());
		CHECK(fixture.Frame.Regions.front().Minimum.w == 0);
		uint32_t queued = fixture.Frame.QueuedRegions;
		CHECK(queued > 0);
		// The next frames take the rest within the per-frame budget but for each frame's first region, until nothing is left.
		uint32_t frames = 0;
		while (fixture.Frame.QueuedRegions > 0 && frames < 1000)
		{
			fixture.Build();
			frames++;
			CHECK(fixture.Frame.QueuedRegions <= queued);
			queued = fixture.Frame.QueuedRegions;
			if (fixture.Frame.Regions.size() > 1)
				CHECK(fixture.Frame.Triangles <= 100);
		}
		CHECK(fixture.Frame.QueuedRegions == 0);
		CHECK_FALSE(fixture.Frame.Pending);
	}

	TEST_CASE("Levels of detail follow the voxel size")
	{
		Fixture fixture;
		fixture.Source.Meshes[c_PlaneMesh] = CreateLODPlane({ 0.0f, 0.04f, 0.2f });
		Entity plane = fixture.Add(glm::vec3(1.0f, 0.3f, 1.0f), c_PlaneMesh);
		Entity cube = fixture.Add(glm::vec3(-2.0f, 0.5f, 1.0f));
		fixture.Build();
		// Half voxels of 0.05 and 0.1 m take level 1 (error 0.04), of 0.2 and 0.4 m level 2; the closed cube full detail.
		const uint32_t expected[] = { 1, 1, 2, 2 };
		const std::vector<RenderData::VoxelWorkItem> items = fixture.GetItems(plane);
		REQUIRE_FALSE(items.empty());
		std::set<uint32_t> levels;
		for (const RenderData::VoxelWorkItem& item : items)
		{
			const uint32_t level = static_cast<uint32_t>(fixture.Frame.Regions[item.Region].Minimum.w);
			CHECK((item.MeshSubmeshLOD >> 28) == expected[level]);
			CHECK((item.Flags & RenderData::c_VoxelItemClosed) == 0);
			levels.insert(level);
		}
		CHECK(levels.size() == 4);
		for (const RenderData::VoxelWorkItem& item : fixture.GetItems(cube))
			CHECK((item.MeshSubmeshLOD >> 28) == 0);
	}

	TEST_CASE("What is left out")
	{
		Fixture fixture;
		fixture.Source.Meshes[c_SkinnedMesh] = MeshFactory::CreateCube();
		fixture.Source.SkinnedArena.insert(c_SkinnedMesh);
		fixture.Source.MaterialFlags[c_GlassMaterial] = RenderData::c_CullMaterialAvailable | RenderData::c_CullMaterialTransparent | RenderData::c_CullMaterialTransmissive;
		fixture.Source.MaterialFlags[c_RedMaterial] = RenderData::c_CullMaterialAvailable | RenderData::c_CullMaterialTransparent;
		Entity skinned = fixture.Add(glm::vec3(0.0f, 0.5f, 2.0f), c_SkinnedMesh);
		Entity glass = fixture.Add(glm::vec3(2.0f, 0.5f, 2.0f), c_CubeMesh, c_GlassMaterial);
		Entity transparent = fixture.Add(glm::vec3(-2.0f, 0.5f, 2.0f), c_CubeMesh, c_RedMaterial);
		Entity waiting = fixture.Add(glm::vec3(4.0f, 0.5f, 2.0f));
		Entity kept = fixture.Add(glm::vec3(-4.0f, 0.5f, 2.0f));
		uint32_t waitingProxy = RenderScene::c_InvalidIndex;
		fixture.Source.Resident = [&waitingProxy](uint32_t proxy) { return proxy != waitingProxy; };
		waitingProxy = fixture.Sync().FindProxy(waiting);
		fixture.Build();
		CHECK(fixture.GetItems(skinned).empty());
		CHECK(fixture.GetItems(glass).empty());
		CHECK(fixture.GetItems(transparent).empty());
		CHECK(fixture.GetItems(waiting).empty());
		CHECK_FALSE(fixture.GetItems(kept).empty());
		// A proxy waiting for residency never makes the frame pending.
		CHECK(fixture.Frame.WaitingProxies == 1);
		CHECK_FALSE(fixture.Frame.Pending);

		// Resident: its bounds are queued with its items.
		const RenderScene& render = RenderScene::Get(fixture.SceneData);
		waitingProxy = RenderScene::c_InvalidIndex;
		fixture.Build();
		CHECK(fixture.Frame.WaitingProxies == 0);
		CHECK_FALSE(fixture.GetItems(waiting).empty());
		for (uint32_t level = 0; level < 4; level++)
			CHECK(fixture.RegionsCover(level, { fixture.GetChangeBox(render.GetWorldBounds(render.FindProxy(waiting)), level) }));
	}

	TEST_CASE("Material and mesh changes requeue their users")
	{
		Fixture fixture;
		Entity red = fixture.Add(glm::vec3(2.0f, 0.5f, 2.0f), c_CubeMesh, c_RedMaterial);
		Entity other = fixture.Add(glm::vec3(-6.0f, 0.5f, -6.0f));
		fixture.Build();
		CHECK_FALSE(fixture.GetItems(red).empty());
		const RenderScene& render = RenderScene::Get(fixture.SceneData);
		const AABB bounds = render.GetWorldBounds(render.FindProxy(red));

		// The red material's data changes in the GPU copy's tables: its users' bounds, nobody else's.
		fixture.Source.MaterialAlbedo[c_RedMaterial] = 0x10109Eu;
		fixture.Source.Version++;
		fixture.Build();
		CHECK(fixture.Frame.MaterialsChanged);
		for (uint32_t level = 0; level < 4; level++)
			CHECK(fixture.RegionsCover(level, { fixture.GetChangeBox(bounds, level) }));
		CHECK_FALSE(fixture.GetItems(red).empty());
		CHECK(fixture.GetItems(other).empty());

		// Its mesh is dropped: its old bounds, and it waits for the mesh.
		fixture.Source.Meshes.erase(c_CubeMesh);
		fixture.Source.Version++;
		fixture.Build();
		CHECK(fixture.Frame.Items.empty());
		CHECK(fixture.Frame.WaitingProxies == 2);
		CHECK_FALSE(fixture.Frame.Regions.empty());
	}

	TEST_CASE("A closed mesh with a glass submesh fills nothing")
	{
		Fixture fixture;
		fixture.Source.Meshes[c_GlassCubeMesh] = CreateTwoMaterialCube(c_RedMaterial, c_GlassMaterial);
		fixture.Source.MaterialFlags[c_GlassMaterial] = RenderData::c_CullMaterialAvailable | RenderData::c_CullMaterialTransparent | RenderData::c_CullMaterialTransmissive;
		Entity house = fixture.Add(glm::vec3(1.0f, 0.5f, 1.0f), c_GlassCubeMesh);
		Entity solid = fixture.Add(glm::vec3(-3.0f, 0.5f, 1.0f));
		fixture.Build();
		const std::vector<RenderData::VoxelWorkItem> items = fixture.GetItems(house);
		REQUIRE_FALSE(items.empty());
		for (const RenderData::VoxelWorkItem& item : items)
		{
			// Open as drawn: only the opaque submesh, without winding steps.
			CHECK(((item.MeshSubmeshLOD >> 20) & 0xFFu) == 0);
			CHECK((item.Flags & RenderData::c_VoxelItemClosed) == 0);
		}
		for (const RenderData::VoxelWorkItem& item : fixture.GetItems(solid))
			CHECK((item.Flags & RenderData::c_VoxelItemClosed) != 0);
	}

	TEST_CASE("Closed meshes below a region step into it")
	{
		Fixture fixture;
		// A tower 30 m tall from the ground: level 0's regions above its middle, out of its bounds' reach, still need its steps.
		Entity tower = fixture.Add(glm::vec3(1.0f, 15.0f, 1.0f), c_CubeMesh, UUID::Null(), glm::vec3(1.0f, 30.0f, 1.0f));
		Entity low = fixture.Add(glm::vec3(1.0f, -20.0f, 1.0f), c_CubeMesh, UUID::Null(), glm::vec3(1.0f, 1.0f, 1.0f));
		fixture.Camera = glm::dvec3(1.0, 25.0, 1.0);
		fixture.Build();
		bool fillOnly = false;
		for (const RenderData::VoxelWorkItem& item : fixture.GetItems(low))
		{
			CHECK((item.Flags & RenderData::c_VoxelItemClosed) != 0);
			// The low cube lies below every level-0 region over it: it only steps into them.
			if (fixture.Frame.Regions[item.Region].Minimum.w == 0)
			{
				CHECK((item.Flags & RenderData::c_VoxelItemFillOnly) != 0);
				fillOnly = true;
			}
		}
		CHECK(fillOnly);
		CHECK_FALSE(fixture.GetItems(tower).empty());
	}
}
