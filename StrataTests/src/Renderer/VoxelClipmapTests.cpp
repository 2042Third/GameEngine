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
	constexpr AssetHandle c_SplitCubeMesh = AssetHandle(uint64_t(0x5B11));
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

	// A closed cube `squares` units wide (centered) whose faces are split into unit squares, each of its 12 squares^2 triangles a
	// submesh of its own. The faces' axes turn counter-clockwise seen from outside (glTF), and every coordinate is a multiple of a
	// half, so the faces' shared vertices are equal.
	Ref<Mesh> CreateSplitCube(int squares)
	{
		// Per face: its outward axis, then two axes whose cross product it is.
		const glm::vec3 faces[6][3] = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
			{ { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 0 } }, { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } }, { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
			{ { 0, 0, -1 }, { 0, 1, 0 }, { 1, 0, 0 } } };
		const float half = 0.5f * static_cast<float>(squares);
		const uint32_t row = static_cast<uint32_t>(squares) + 1;
		std::vector<glm::vec3> positions;
		std::vector<uint32_t> indices;
		for (const auto& face : faces)
		{
			const uint32_t first = static_cast<uint32_t>(positions.size());
			for (int v = 0; v <= squares; v++)
			{
				for (int u = 0; u <= squares; u++)
					positions.push_back(face[0] * half + face[1] * (static_cast<float>(u) - half) + face[2] * (static_cast<float>(v) - half));
			}
			for (uint32_t v = 0; v < row - 1; v++)
			{
				for (uint32_t u = 0; u < row - 1; u++)
				{
					const uint32_t corner = first + v * row + u;
					indices.insert(indices.end(), { corner, corner + 1, corner + row + 1, corner, corner + row + 1, corner + row });
				}
			}
		}
		Submesh triangle = MeshFactory::CreateCube()->GetSubmeshes()[0];
		triangle.VertexCount = static_cast<uint32_t>(positions.size());
		std::vector<Submesh> submeshes;
		for (uint32_t index = 0; index < indices.size(); index += 3)
		{
			triangle.LODs = { MeshLOD { index, 3 } };
			submeshes.push_back(triangle);
		}
		const Ref<Mesh> mesh = Mesh::Create(positions, std::vector<MeshVertexAttributes>(positions.size()), indices, submeshes);
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

	// The fading bricks of a frame, checked: each one's slot of the backup lies past the frame's bricks' own (which are their
	// order) and past those of the frame before where it is taken this frame, within the layout's, and no two share one.
	struct FadeCheck
	{
		uint32_t Fades = 0;
		uint32_t Snapshots = 0;
		uint64_t Voxels = 0;
		float Time = 0.0f; // The last fading brick's
	};

	FadeCheck CheckFades(const VoxelFrame& frame, const GlobalIlluminationLayout& layout, uint32_t level, size_t bricksBefore)
	{
		FadeCheck check;
		std::vector<uint32_t> slots;
		for (const RenderData::VoxelMoverBrick& brick : frame.MoverBricks)
		{
			const uint32_t fade = brick.Movers.z;
			if (fade == 0)
				continue;
			check.Fades++;
			const bool snapshot = (fade & RenderData::c_VoxelMoverFadeSnapshot) != 0;
			check.Snapshots += snapshot ? 1u : 0u;
			const uint32_t slot = (fade & RenderData::c_VoxelMoverFadeSlotMask) - 1;
			CHECK(slot >= frame.MoverBricks.size());
			if (snapshot)
				CHECK(slot >= bricksBefore);
			CHECK(slot < layout.MoverBricks);
			slots.push_back(slot);
			const glm::ivec3 low((fade >> RenderData::c_VoxelMoverFadeLowShift) & 7u, (fade >> (RenderData::c_VoxelMoverFadeLowShift + 3)) & 7u,
				(fade >> (RenderData::c_VoxelMoverFadeLowShift + 6)) & 7u);
			const glm::ivec3 high((fade >> RenderData::c_VoxelMoverFadeHighShift) & 7u, (fade >> (RenderData::c_VoxelMoverFadeHighShift + 3)) & 7u,
				(fade >> (RenderData::c_VoxelMoverFadeHighShift + 6)) & 7u);
			CHECK(glm::all(glm::lessThanEqual(low, high)));
			if (static_cast<uint32_t>(brick.Texel.w) == level)
				check.Voxels += static_cast<uint64_t>((high.x - low.x + 1) * (high.y - low.y + 1) * (high.z - low.z + 1));
			check.Time = glm::uintBitsToFloat(brick.Movers.w);
		}
		CHECK(check.Fades == frame.MoverFades);
		CHECK(check.Snapshots == frame.MoverSnapshots);
		std::sort(slots.begin(), slots.end());
		CHECK(std::adjacent_find(slots.begin(), slots.end()) == slots.end());
		return check;
	}

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

		// Moved: its old voxels are emptied in the frame it moved (its triangles are out of the voxels while it moves: the dynamic
		// layer holds it), and the camera's move in the same frame queues its slabs behind the change.
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

		// It rests by the clock, never by frames: while no time passes it stays a mover, and c_GIMoverRestSeconds after its last
		// move its new bounds are queued, with its items.
		fixture.Camera = glm::dvec3(0.0);
		fixture.Build();
		const AABB after = render.GetWorldBounds(render.FindProxy(cube));
		for (uint32_t frame = 0; frame < 100; frame++)
		{
			fixture.Build();
			CHECK(fixture.GetItems(cube).empty());
		}
		CHECK(fixture.Frame.MovingProxies == 1);
		fixture.Settings.StepSeconds = 0.4f * RenderData::c_GIMoverRestSeconds;
		for (uint32_t frame = 0; frame < 2; frame++)
		{
			fixture.Build();
			CHECK(fixture.GetItems(cube).empty());
			CHECK(fixture.Frame.MovingProxies == 1);
		}
		fixture.Build();
		CHECK(fixture.Frame.MovingProxies == 0);
		CHECK(fixture.Frame.Movers.empty());
		// Of the layer only the bricks that fade are left ("Where a proxy starts to move or rests, the bricks around it fade").
		CHECK(fixture.Frame.MoverBricks.size() == fixture.Frame.MoverFades);
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

	TEST_CASE("A moving proxy is listed for the dynamic layer, and nothing is while nothing moves")
	{
		Fixture fixture;
		fixture.Settings.StepSeconds = 0.4f * RenderData::c_GIMoverRestSeconds;
		Entity cube = fixture.Add(glm::vec3(2.0f, 0.5f, 3.03f), c_CubeMesh, UUID::Null(), glm::vec3(1.0f, 0.5f, 2.0f));
		fixture.Camera = glm::dvec3(1.0, 1.0, 1.0);
		fixture.Build();
		fixture.Build();
		CHECK(fixture.Frame.Movers.empty());
		CHECK(fixture.Frame.MoverBricks.empty());
		CHECK_FALSE(fixture.Frame.MoversChanged);

		// Moved and turned: one box, its center relative to the camera and its half extents along the proxy's turned axes.
		TransformComponent& transform = cube.GetComponent<TransformComponent>();
		transform.Translation = glm::vec3(2.3f, 0.5f, 3.03f);
		transform.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		cube.MarkModified<TransformComponent>();
		fixture.Build();
		CHECK(fixture.Frame.MovingProxies == 1);
		CHECK(fixture.Frame.MoversLeftOut == 0);
		CHECK(fixture.Frame.MoversChanged);
		REQUIRE(fixture.Frame.Movers.size() == 1);
		const RenderData::VoxelMover& mover = fixture.Frame.Movers.front();
		CHECK(glm::distance(glm::vec3(mover.Center), glm::vec3(1.3f, -0.5f, 2.03f)) < 1e-5f);
		// A quarter turn about y takes the box's x axis to -z and its z axis to +x.
		CHECK(glm::distance(glm::abs(glm::vec3(mover.AxisX)), glm::vec3(0.0f, 0.0f, 0.5f)) < 1e-5f);
		CHECK(glm::distance(glm::abs(glm::vec3(mover.AxisY)), glm::vec3(0.0f, 0.25f, 0.0f)) < 1e-5f);
		CHECK(glm::distance(glm::abs(glm::vec3(mover.AxisZ)), glm::vec3(1.0f, 0.0f, 0.0f)) < 1e-5f);

		// Its bricks: per level, in order, those holding every voxel of the window the box reaches, each in the window in part,
		// with its texel, its slot in order and the box's bit. Among them, without the bit: bricks the box does not reach where
		// the voxels it left fade.
		const AABB bounds = RenderScene::Get(fixture.SceneData).GetWorldBounds(RenderScene::Get(fixture.SceneData).FindProxy(cube));
		const glm::uvec3 dimensions = fixture.Clipmap.GetLayout().Levels.front().Dimensions;
		constexpr int64_t c_Brick = RenderData::c_GIMoverBrickVoxels;
		uint32_t previousLevel = 0;
		std::array<uint32_t, 4> perLevel = {};
		for (size_t index = 0; index < fixture.Frame.MoverBricks.size(); index++)
		{
			const RenderData::VoxelMoverBrick& brick = fixture.Frame.MoverBricks[index];
			const uint32_t level = static_cast<uint32_t>(brick.Texel.w);
			REQUIRE(level < 4);
			CHECK(level >= previousLevel);
			previousLevel = level;
			perLevel[level] += brick.Movers.x;
			CHECK(brick.Local.w == static_cast<int32_t>(index));
			CHECK(brick.Movers.x <= 1u);
			CHECK((brick.Movers.x == 1u || brick.Movers.z != 0u));
			CHECK(brick.Movers.y == 0u);
			const glm::i64vec3 window = fixture.Clipmap.GetLevel(level).Window;
			const glm::i64vec3 voxel = window + glm::i64vec3(glm::ivec3(brick.Local));
			CHECK(voxel.x % c_Brick == 0);
			CHECK(voxel.y % c_Brick == 0);
			CHECK(voxel.z % c_Brick == 0);
			CHECK(glm::uvec3(glm::ivec3(brick.Texel)) == GlobalIllumination::GetTexel(voxel, dimensions));
			const VoxelBox box { voxel, voxel + c_Brick };
			CHECK(box.Intersects(GlobalIllumination::GetWindow(fixture.Clipmap.GetLayout().Levels[level], window)));
		}
		for (uint32_t level = 0; level < 4; level++)
		{
			CAPTURE(level);
			// Along each axis the box and a voxel beyond, at least two voxels each way: what the layer weighs.
			const double voxelSize = static_cast<double>(fixture.Clipmap.GetLayout().Levels[level].VoxelSize);
			const glm::dvec3 center = 0.5 * (glm::dvec3(bounds.Min) + glm::dvec3(bounds.Max));
			const glm::dvec3 reach = glm::max(0.5 * (glm::dvec3(bounds.Max) - glm::dvec3(bounds.Min)), glm::dvec3(voxelSize)) + voxelSize;
			const glm::i64vec3 low(glm::floor((center - reach) / voxelSize));
			const glm::i64vec3 high(glm::floor((center + reach) / voxelSize));
			uint32_t expected = 1;
			for (int axis = 0; axis < 3; axis++)
			{
				const int64_t first = low[axis] >= 0 ? low[axis] / c_Brick : -((-low[axis] + c_Brick - 1) / c_Brick);
				const int64_t last = high[axis] >= 0 ? high[axis] / c_Brick : -((-high[axis] + c_Brick - 1) / c_Brick);
				expected *= static_cast<uint32_t>(last - first + 1);
			}
			CHECK(perLevel[level] == expected);
		}

		// Unmoved while no time passes, the list repeats (its fade, begun the frame before, stands still as well).
		fixture.Settings.StepSeconds = 0.0f;
		fixture.Build();
		fixture.Build();
		CHECK(fixture.Frame.Movers.size() == 1);
		CHECK_FALSE(fixture.Frame.MoversChanged);
		// Resting, its box leaves in the frame the regions that hold its triangles are taken; the bricks around it fade for
		// c_GIMoverFadeSeconds more, then nothing is listed and nothing changes.
		fixture.Settings.StepSeconds = 0.4f * RenderData::c_GIMoverRestSeconds;
		for (uint32_t frame = 0; frame < 3; frame++)
			fixture.Build();
		CHECK(fixture.Frame.MovingProxies == 0);
		CHECK(fixture.Frame.Movers.empty());
		CHECK(fixture.Frame.MoversChanged);
		CHECK_FALSE(fixture.GetItems(cube).empty());
		CHECK(fixture.Frame.MoverFades > 0);
		for (uint32_t frame = 0; frame < 2; frame++)
		{
			fixture.Build();
			CHECK(fixture.Frame.MoverFades > 0);
			CHECK(fixture.Frame.MoversChanged);
		}
		fixture.Build();
		CHECK(fixture.Frame.MoverBricks.empty());
		CHECK(fixture.Frame.MoverFades == 0);
		CHECK(fixture.Frame.MoversChanged);
		fixture.Build();
		CHECK_FALSE(fixture.Frame.MoversChanged);
	}

	TEST_CASE("Where a proxy starts to move or rests, the bricks around it fade by the clock")
	{
		Fixture fixture;
		fixture.Settings.StepSeconds = 0.4f * RenderData::c_GIMoverFadeSeconds;
		Entity cube = fixture.Add(glm::vec3(2.0f, 0.5f, 3.03f));
		fixture.Build();
		fixture.Build();
		CHECK(fixture.Frame.MoverFades == 0);
		const RenderScene& render = RenderScene::Get(fixture.SceneData);
		const AABB before = render.GetWorldBounds(render.FindProxy(cube));
		const GlobalIlluminationLayout& layout = fixture.Clipmap.GetLayout();
		// The voxels of a level that fade for bounds: those their triangles touch and one beyond.
		const auto getFaded = [&](const AABB& bounds, uint32_t level)
		{
			VoxelBox box = VoxelClipmap::GetTouchedVoxels(bounds, layout.Levels[level].VoxelSize);
			box.Minimum -= int64_t(1);
			box.Maximum += int64_t(1);
			REQUIRE(fixture.Clipmap.GetLevel(level).Valid.Contains(box));
			return box.GetVoxelCount();
		};

		// It starts: the frame takes what the bricks around its old place show, and they show all of it still.
		cube.GetComponent<TransformComponent>().Translation.x = 2.02f;
		cube.MarkModified<TransformComponent>();
		fixture.Build();
		CHECK(fixture.Frame.MovingProxies == 1);
		for (uint32_t level = 0; level < 4; level++)
		{
			CAPTURE(level);
			const FadeCheck check = CheckFades(fixture.Frame, layout, level, 0);
			CHECK(check.Fades > 0);
			CHECK(check.Snapshots == check.Fades);
			CHECK(check.Voxels == getFaded(before, level));
			CHECK(check.Time == 0.0f);
		}
		// The clock moves the fades: 0.4 and 0.8 of c_GIMoverFadeSeconds.
		size_t bricksBefore = fixture.Frame.MoverBricks.size();
		fixture.Build();
		FadeCheck check = CheckFades(fixture.Frame, layout, 0, bricksBefore);
		CHECK(check.Snapshots == 0);
		CHECK(check.Time == doctest::Approx(0.4f).epsilon(0.001));
		CHECK(fixture.Frame.MoversChanged);
		fixture.Build();
		check = CheckFades(fixture.Frame, layout, 0, bricksBefore);
		CHECK(check.Time == doctest::Approx(0.8f).epsilon(0.001));
		// Its time over, the fade ends; in the same frame the proxy has rested, and the bricks around it fade again, from what
		// they show with its box to what they hold with its triangles.
		bricksBefore = fixture.Frame.MoverBricks.size();
		const AABB after = render.GetWorldBounds(render.FindProxy(cube));
		fixture.Build();
		CHECK(fixture.Frame.MovingProxies == 0);
		CHECK(fixture.Frame.Movers.empty());
		for (uint32_t level = 0; level < 4; level++)
		{
			CAPTURE(level);
			check = CheckFades(fixture.Frame, layout, level, bricksBefore);
			CHECK(check.Snapshots == check.Fades);
			CHECK(check.Voxels == getFaded(after, level));
			CHECK(check.Time == 0.0f);
		}
		// Moved again while they fade: the bricks start over from what they show, with both events' voxels.
		fixture.Build();
		cube.GetComponent<TransformComponent>().Translation.x = 2.04f;
		cube.MarkModified<TransformComponent>();
		bricksBefore = fixture.Frame.MoverBricks.size();
		const uint32_t fadesBefore = fixture.Frame.MoverFades;
		fixture.Build();
		check = CheckFades(fixture.Frame, layout, 0, bricksBefore);
		CHECK(check.Fades == fadesBefore);
		CHECK(check.Snapshots == check.Fades);
		CHECK(check.Voxels >= getFaded(after, 0));
		CHECK(check.Time == 0.0f);
	}

	TEST_CASE("Movers come before fades in the dynamic layer's slots")
	{
		// No time passes, so what moved stays a mover and what fades keeps fading. Cubes start to move one a frame: a start fades
		// whole while the slots hold it, the fades' slots never reach into the frame's bricks' own, and where the movers' bricks
		// need the slots, fades end: no mover is left out before the movers alone fill the layer.
		Fixture fixture;
		std::vector<Entity> cubes;
		for (uint32_t index = 0; index < 72; index++)
		{
			cubes.push_back(fixture.Add(glm::vec3(-5.0f + 2.0f * static_cast<float>(index % 6), 0.5f + 2.0f * static_cast<float>(index / 36),
				-5.0f + 2.0f * static_cast<float>((index / 6) % 6))));
		}
		fixture.Build();
		const GlobalIlluminationLayout& layout = fixture.Clipmap.GetLayout();
		uint32_t peakFades = 0;
		uint32_t lastFades = 0;
		uint32_t moverBricks = 0;
		size_t bricksBefore = 0;
		bool leftOut = false;
		for (Entity cube : cubes)
		{
			cube.GetComponent<TransformComponent>().Translation.y += 0.01f;
			cube.MarkModified<TransformComponent>();
			fixture.Build();
			CHECK(fixture.Frame.MoverBricks.size() <= layout.MoverBricks);
			lastFades = CheckFades(fixture.Frame, layout, 0, bricksBefore).Fades;
			bricksBefore = fixture.Frame.MoverBricks.size();
			moverBricks = 0;
			for (const RenderData::VoxelMoverBrick& brick : fixture.Frame.MoverBricks)
				moverBricks += (brick.Movers.x | brick.Movers.y) != 0 ? 1u : 0u;
			if (fixture.Frame.MoversLeftOut > 0)
			{
				leftOut = true;
				break;
			}
			peakFades = std::max(peakFades, lastFades);
		}
		// A cube of 1 m reaches 51 bricks at most: the one left out found fewer free, whatever faded.
		CHECK(leftOut);
		CHECK(moverBricks + 51 > layout.MoverBricks);
		CHECK(peakFades > lastFades);
		CHECK(moverBricks + lastFades <= layout.MoverBricks);
	}

	TEST_CASE("A proxy that rested stays in the dynamic layer until its regions are taken")
	{
		// A budget of one triangle a frame takes one region a frame: the cube's bounds wait in every level's queue for a while,
		// and it stays a mover until the last of them is taken, so it is never in neither.
		Fixture fixture;
		fixture.Settings.TrianglesPerFrame = 1;
		fixture.Settings.StepSeconds = RenderData::c_GIMoverRestSeconds;
		Entity cube = fixture.Add(glm::vec3(2.0f, 0.5f, 3.0f));
		for (uint32_t frame = 0; frame < 2000 && (frame == 0 || fixture.Frame.QueuedRegions > 0); frame++)
			fixture.Build();
		REQUIRE(fixture.Frame.QueuedRegions == 0);
		cube.GetComponent<TransformComponent>().Translation = glm::vec3(2.33f, 0.5f, 3.0f);
		cube.MarkModified<TransformComponent>();
		fixture.Build();
		CHECK(fixture.Frame.MovingProxies == 1);
		// The frame after, it has rested: its bounds are queued, and one region a frame is taken.
		uint32_t waited = 0;
		bool voxelized = false;
		for (uint32_t frame = 0; frame < 200; frame++)
		{
			fixture.Build();
			voxelized = voxelized || !fixture.GetItems(cube).empty();
			if (fixture.Frame.QueuedRegions == 0)
				break;
			waited++;
			CHECK(fixture.Frame.MovingProxies == 1);
			CHECK(fixture.Frame.Movers.size() == 1);
		}
		CHECK(waited >= 3);
		CHECK(voxelized);
		CHECK(fixture.Frame.QueuedRegions == 0);
		CHECK(fixture.Frame.MovingProxies == 0);
		CHECK(fixture.Frame.Movers.empty());
	}

	TEST_CASE("Movers past the dynamic layer's bricks or boxes are left out")
	{
		Fixture fixture;
		Entity large = fixture.Add(glm::vec3(0.0f, 0.5f, 0.0f), c_CubeMesh, UUID::Null(), glm::vec3(12.0f, 6.0f, 12.0f));
		std::vector<Entity> cubes;
		for (uint32_t index = 0; index < RenderData::c_GIMaxMoverBoxes + 1; index++)
			cubes.push_back(fixture.Add(glm::vec3(-3.0f + 0.1f * static_cast<float>(index), 0.5f, 3.0f), c_CubeMesh, UUID::Null(), glm::vec3(0.05f)));
		fixture.Build();
		const auto move = [](Entity entity)
		{
			entity.GetComponent<TransformComponent>().Translation.y += 0.01f;
			entity.MarkModified<TransformComponent>();
		};

		// A box of 12 x 6 x 12 m reaches more of level 0's bricks than High's layer holds: left out, nothing listed (nor does its
		// start fade: more bricks than the layer has slots fade not at all).
		move(large);
		fixture.Build();
		CHECK(fixture.Frame.MovingProxies == 1);
		CHECK(fixture.Frame.MoversLeftOut == 1);
		CHECK(fixture.Frame.Movers.empty());
		CHECK(fixture.Frame.MoverBricks.empty());

		// One box more than a frame lists: the last proxy is left out, the others keep their boxes.
		for (Entity cube : cubes)
			move(cube);
		fixture.Build();
		CHECK(fixture.Frame.MovingProxies == RenderData::c_GIMaxMoverBoxes + 2);
		CHECK(fixture.Frame.MoversLeftOut == 2);
		CHECK(fixture.Frame.Movers.size() == RenderData::c_GIMaxMoverBoxes);
		CHECK(fixture.Frame.MoverBricks.size() <= fixture.Clipmap.GetLayout().MoverBricks);
		uint32_t bits = 0;
		for (const RenderData::VoxelMoverBrick& brick : fixture.Frame.MoverBricks)
			bits |= brick.Movers.y;
		CHECK(bits != 0u); // Boxes past the 32nd are named in the second word
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

	TEST_CASE("The brightest albedo voxelized bounds the bounces")
	{
		// GetBounceAlbedo, what GlobalIlluminationSettled's wait follows: the largest channel of the voxelized submeshes' albedos
		// (a transparent one is not voxelized), clamped below c_GIMaxBounceAlbedo, kept while the clipmap lives.
		Fixture fixture;
		fixture.Source.MaterialAlbedo[c_RedMaterial] = 0x10109Eu;
		fixture.Source.MaterialAlbedo[c_GlassMaterial] = 0xFFFFFFu;
		fixture.Source.MaterialFlags[c_GlassMaterial] = RenderData::c_CullMaterialAvailable | RenderData::c_CullMaterialTransparent;
		CHECK(fixture.Clipmap.GetBounceAlbedo() == 0.0f);
		fixture.Add(glm::vec3(2.0f, 0.5f, 2.0f), c_CubeMesh, c_RedMaterial);
		fixture.Add(glm::vec3(-2.0f, 0.5f, 2.0f), c_CubeMesh, c_GlassMaterial);
		fixture.Build();
		CHECK_FALSE(fixture.Frame.Items.empty());
		CHECK(fixture.Clipmap.GetBounceAlbedo() == 158.0f / 255.0f);
		fixture.Source.MaterialAlbedo[c_RedMaterial] = 0xFFFFFFu;
		fixture.Source.Version++;
		fixture.Build();
		CHECK(fixture.Clipmap.GetBounceAlbedo() == RenderData::c_GIMaxBounceAlbedo);
		fixture.Source.MaterialAlbedo[c_RedMaterial] = 0x101010u;
		fixture.Source.Version++;
		fixture.Build();
		CHECK(fixture.Clipmap.GetBounceAlbedo() == RenderData::c_GIMaxBounceAlbedo);
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

	TEST_CASE("A closed mesh past 256 submeshes is voxelized open, and counted")
	{
		// A work item holds 8 bits of submesh: the submeshes past the 256th are left out, so the rest would step into their
		// columns unbalanced and fill them solid. The rest is voxelized open instead, and the frame counts what it left out.
		Fixture fixture;
		fixture.Source.Meshes[c_SplitCubeMesh] = CreateSplitCube(5);
		REQUIRE(fixture.Source.Meshes[c_SplitCubeMesh]->GetSubmeshes().size() == 300);
		Entity split = fixture.Add(glm::vec3(1.0f, 0.5f, 1.0f), c_SplitCubeMesh, UUID::Null(), glm::vec3(0.2f));
		Entity solid = fixture.Add(glm::vec3(-3.0f, 0.5f, 1.0f));
		fixture.Build();
		const std::vector<RenderData::VoxelWorkItem> items = fixture.GetItems(split);
		REQUIRE_FALSE(items.empty());
		for (const RenderData::VoxelWorkItem& item : items)
			CHECK((item.Flags & (RenderData::c_VoxelItemClosed | RenderData::c_VoxelItemFillOnly)) == 0);
		CHECK(fixture.Frame.UnvoxelizedSubmeshes > 0);
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
