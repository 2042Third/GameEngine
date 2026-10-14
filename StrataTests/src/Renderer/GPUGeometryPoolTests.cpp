#include <doctest/doctest.h>

#include "Renderer/SceneRendererTestUtils.h"
#include "Strata/Renderer/GeometryPool.h"
#include "Strata/Renderer/MeshFactory.h"

#include <cstring>
#include <initializer_list>
#include <map>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// Distinct bytes for an allocation's units: what it holds where.
	std::vector<uint8_t> MakePattern(uint32_t seed, uint64_t bytes)
	{
		std::vector<uint8_t> pattern(bytes);
		for (uint64_t index = 0; index < bytes; index++)
			pattern[index] = static_cast<uint8_t>((seed * 131 + index * 7) & 0xFF);
		return pattern;
	}

	struct Filled
	{
		GeometryHandle Handle = c_InvalidGeometry;
		std::vector<uint8_t> Positions;
		std::vector<uint8_t> Indices;
	};

	// Allocates static geometry of `vertices` and `indices` units and writes patterns into its positions and indices.
	Filled AllocateFilled(GPUContext& gpu, GeometryPool& pool, uint32_t seed, uint32_t vertices, uint32_t indices)
	{
		Filled filled;
		const GeometryPool::AllocateResult result = pool.Allocate(GeometryArena::Static, vertices, indices, 0);
		REQUIRE(result.Handle != c_InvalidGeometry);
		filled.Handle = result.Handle;
		const GeometryAllocation* allocation = pool.Find(result.Handle);
		REQUIRE(allocation);
		filled.Positions = MakePattern(seed, uint64_t(allocation->VertexCount) * GeometryPool::c_PositionStride);
		filled.Indices = MakePattern(seed + 1000, uint64_t(allocation->IndexCount) * GeometryPool::c_IndexStride);
		nvrhi::CommandListHandle commandList = gpu.GetNvrhiDevice()->createCommandList();
		commandList->open();
		commandList->writeBuffer(pool.GetPositionBuffer(GeometryArena::Static), filled.Positions.data(), filled.Positions.size(),
			uint64_t(allocation->FirstVertex) * GeometryPool::c_PositionStride);
		commandList->writeBuffer(pool.GetIndexBuffer(), filled.Indices.data(), filled.Indices.size(), uint64_t(allocation->FirstIndex) * GeometryPool::c_IndexStride);
		commandList->close();
		REQUIRE(gpu.ExecuteAndWait(commandList));
		pool.SetUploading(result.Handle, false);
		return filled;
	}

	// Whether the pool holds the allocation's patterns where its table says it is now.
	bool HoldsPattern(GPUContext& gpu, const GeometryPool& pool, const Filled& filled)
	{
		const GeometryAllocation* allocation = pool.Find(filled.Handle);
		if (!allocation)
			return false;
		const std::vector<uint8_t> positions = ReadBufferRange(gpu, pool.GetPositionBuffer(GeometryArena::Static),
			uint64_t(allocation->FirstVertex) * GeometryPool::c_PositionStride, filled.Positions.size());
		const std::vector<uint8_t> indices = ReadBufferRange(gpu, pool.GetIndexBuffer(), uint64_t(allocation->FirstIndex) * GeometryPool::c_IndexStride,
			filled.Indices.size());
		return positions == filled.Positions && indices == filled.Indices;
	}

	// A layout of submeshes and levels for SetLayout: per submesh its base vertex and its levels' (index offset, count).
	struct TestLayout
	{
		std::vector<RenderData::GeometrySubmesh> Submeshes;
		std::vector<RenderData::GeometryLOD> LODs;

		void AddSubmesh(uint32_t baseVertex, std::initializer_list<std::pair<uint32_t, uint32_t>> levels)
		{
			Submeshes.push_back(RenderData::GeometrySubmesh { baseVertex, static_cast<uint32_t>(LODs.size()), static_cast<uint32_t>(levels.size()), 0 });
			for (const auto& [offset, count] : levels)
				LODs.push_back(RenderData::GeometryLOD { offset, count, baseVertex, 0 });
		}
	};

	template<typename Record>
	std::vector<Record> ReadTable(GPUContext& gpu, nvrhi::IBuffer* buffer)
	{
		REQUIRE(buffer);
		const std::vector<uint8_t> bytes = ReadBufferRange(gpu, buffer, 0, buffer->getDesc().byteSize);
		std::vector<Record> records(bytes.size() / sizeof(Record));
		std::memcpy(records.data(), bytes.data(), records.size() * sizeof(Record));
		return records;
	}

	// Uploads the pool's tables and checks that the GPU's copies give each handle its place and layout.
	void CheckTables(GPUContext& gpu, GeometryPool& pool, const std::map<GeometryHandle, TestLayout>& layouts)
	{
		nvrhi::CommandListHandle commandList = gpu.GetNvrhiDevice()->createCommandList();
		commandList->open();
		REQUIRE(pool.RecordTableUploads(commandList));
		commandList->close();
		REQUIRE(gpu.ExecuteAndWait(commandList));
		const std::vector<RenderData::GeometryEntry> entries = ReadTable<RenderData::GeometryEntry>(gpu, pool.GetEntryBuffer());
		const std::vector<RenderData::GeometrySubmesh> submeshes = ReadTable<RenderData::GeometrySubmesh>(gpu, pool.GetSubmeshBuffer());
		const std::vector<RenderData::GeometryLOD> lods = ReadTable<RenderData::GeometryLOD>(gpu, pool.GetLODBuffer());
		CHECK(lods.size() >= pool.GetLODCapacity());
		for (const auto& [handle, layout] : layouts)
		{
			CAPTURE(handle);
			REQUIRE(handle < entries.size());
			const RenderData::GeometryEntry& entry = entries[handle];
			const GeometryAllocation* allocation = pool.Find(handle);
			if (!allocation)
			{
				// Freed: nothing to draw.
				CHECK(entry.SubmeshCount == 0);
				continue;
			}
			CHECK(entry.FirstVertex == allocation->FirstVertex);
			CHECK(entry.FirstIndex == allocation->FirstIndex);
			REQUIRE(entry.SubmeshCount == layout.Submeshes.size());
			for (uint32_t index = 0; index < entry.SubmeshCount; index++)
			{
				REQUIRE(entry.FirstSubmesh + index < submeshes.size());
				const RenderData::GeometrySubmesh& submesh = submeshes[entry.FirstSubmesh + index];
				const RenderData::GeometrySubmesh& expected = layout.Submeshes[index];
				CHECK(submesh.BaseVertex == expected.BaseVertex);
				REQUIRE(submesh.LODCount == expected.LODCount);
				for (uint32_t level = 0; level < submesh.LODCount; level++)
				{
					REQUIRE(submesh.FirstLOD + level < lods.size());
					const RenderData::GeometryLOD& lod = lods[submesh.FirstLOD + level];
					const RenderData::GeometryLOD& expectedLOD = layout.LODs[expected.FirstLOD + level];
					CHECK(lod.IndexOffset == expectedLOD.IndexOffset);
					CHECK(lod.IndexCount == expectedLOD.IndexCount);
					CHECK(lod.BaseVertex == expectedLOD.BaseVertex);
					CHECK(lod.Mesh == handle);
				}
			}
		}
	}

	GeometryPool::Specification SmallPool()
	{
		GeometryPool::Specification specification;
		specification.StaticVertices = 256;
		specification.SkinnedVertices = 64;
		specification.Indices = 1024;
		return specification;
	}

}

TEST_SUITE("GPU.Renderer.GeometryPool")
{
	TEST_CASE("Freed ranges are reused only once the GPU finished the work submitted before they were freed")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const Ref<GeometryPool> pool = GeometryPool::Create(gpu.GetNvrhiDevice(), SmallPool());
		REQUIRE(pool);
		const GeometryPool::AllocateResult first = pool->Allocate(GeometryArena::Static, 100, 300, 0);
		REQUIRE(first.Handle != c_InvalidGeometry);
		const GeometryAllocation firstPlace = *pool->Find(first.Handle);
		CHECK(firstPlace.VertexCount == 112); // Rounded to the granularity
		CHECK(firstPlace.IndexCount == 320);
		CHECK(pool->GetStats().Allocations == 1);
		CHECK(pool->GetStats().UsedBytes == GeometryPool::GetAllocationBytes(GeometryArena::Static, 100, 300));

		// Freed (from any thread): its ranges stay taken until a frame saw the GPU finish what was submitted before.
		pool->Free(first.Handle);
		CHECK(pool->Find(first.Handle) != nullptr);
		const GeometryPool::AllocateResult second = pool->Allocate(GeometryArena::Static, 100, 300, 0);
		REQUIRE(second.Handle != c_InvalidGeometry);
		CHECK(pool->Find(second.Handle)->FirstVertex != firstPlace.FirstVertex);
		pool->Free(second.Handle);

		// A frame with work in flight queues the frees behind it; once it completed, the ranges return.
		nvrhi::CommandListHandle commandList = gpu.GetNvrhiDevice()->createCommandList();
		commandList->open();
		commandList->clearBufferUInt(pool->GetIndexBuffer(), 0);
		commandList->close();
		gpu.GetNvrhiDevice()->executeCommandList(commandList);
		pool->BeginFrame();
		gpu.GetDevice().WaitForIdle();
		pool->BeginFrame();
		CHECK(pool->Find(first.Handle) == nullptr);
		CHECK(pool->GetStats().Allocations == 0);
		CHECK(pool->GetStats().PendingFrees == 0);
		// The handle and the ranges are reused.
		const GeometryPool::AllocateResult third = pool->Allocate(GeometryArena::Static, 100, 300, 0);
		REQUIRE(third.Handle != c_InvalidGeometry);
		CHECK(pool->Find(third.Handle)->FirstVertex == firstPlace.FirstVertex);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("The pool grows by a GPU copy that keeps what it holds")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		const Ref<GeometryPool> pool = GeometryPool::Create(gpu.GetNvrhiDevice(), SmallPool());
		REQUIRE(pool);
		std::vector<Filled> filled;
		for (uint32_t index = 0; index < 3; index++)
			filled.push_back(AllocateFilled(gpu, *pool, index, 60, 200));
		const uint64_t version = pool->GetBufferVersion();
		nvrhi::IBuffer* oldPositions = pool->GetPositionBuffer(GeometryArena::Static);

		// Too large for the rest: outside an upload batch that wrote into the buffers, it grows at once.
		const GeometryPool::AllocateResult large = pool->Allocate(GeometryArena::Static, 1000, 4000, 0);
		REQUIRE(large.Handle != c_InvalidGeometry);
		CHECK(pool->GetBufferVersion() > version);
		CHECK(pool->GetPositionBuffer(GeometryArena::Static) != oldPositions);
		CHECK(pool->GetVertexCapacity(GeometryArena::Static) >= 1000 + 3 * 64);
		CHECK(pool->GetIndexCapacity() >= 4000 + 3 * 256);
		CHECK(pool->GetStats().Growths >= 1);
		gpu.GetDevice().WaitForIdle();
		for (const Filled& entry : filled)
			CHECK(HoldsPattern(gpu, *pool, entry));

		// Under an upload batch that wrote into the buffers it waits for the next frame (the batch's copies target the
		// current buffers).
		pool->NoteUploadWrite(42);
		const GeometryPool::AllocateResult later = pool->Allocate(GeometryArena::Static, 100000, 10, 42);
		CHECK(later.Handle == c_InvalidGeometry);
		CHECK(later.Retry);
		pool->BeginFrame();
		CHECK(pool->GetVertexCapacity(GeometryArena::Static) >= 100000);
		CHECK(pool->Allocate(GeometryArena::Static, 100000, 10, 43).Handle != c_InvalidGeometry);
		// Larger than any buffer the pool makes: never.
		const GeometryPool::AllocateResult never = pool->Allocate(GeometryArena::Static, 0xFFFFFFF0u, 10, 0);
		CHECK(never.Handle == c_InvalidGeometry);
		CHECK_FALSE(never.Retry);
		gpu.GetDevice().WaitForIdle();
		for (const Filled& entry : filled)
			CHECK(HoldsPattern(gpu, *pool, entry));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Compaction moves geometry by GPU copies, its table follows, and uploads in progress stay")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		GeometryPool::Specification specification = SmallPool();
		specification.StaticVertices = 1024;
		specification.Indices = 4096;
		const Ref<GeometryPool> pool = GeometryPool::Create(gpu.GetNvrhiDevice(), specification);
		REQUIRE(pool);
		std::vector<Filled> filled;
		for (uint32_t index = 0; index < 12; index++)
			filled.push_back(AllocateFilled(gpu, *pool, index, 48 + index * 8, 160 + index * 16));
		// Every other one goes; the last stays "uploading", which pins it.
		std::vector<Filled> kept;
		for (uint32_t index = 0; index < filled.size(); index++)
		{
			if (index % 2 == 0)
				pool->Free(filled[index].Handle);
			else
				kept.push_back(filled[index]);
		}
		pool->SetUploading(kept.back().Handle, true);
		const GeometryAllocation pinned = *pool->Find(kept.back().Handle);
		std::map<GeometryHandle, GeometryAllocation> before;
		for (const Filled& entry : kept)
			before[entry.Handle] = *pool->Find(entry.Handle);
		// The frees return once the GPU is done; the frame then compacts the fragmented arena within its budget, and
		// Compact finishes the job in calls bounded by the bytes given.
		gpu.GetDevice().WaitForIdle();
		pool->BeginFrame();
		pool->BeginFrame();
		REQUIRE(pool->GetStats().Allocations == kept.size());
		int calls = 0;
		while (pool->Compact(512))
			REQUIRE(++calls < 1000);
		CHECK(pool->GetStats().Moves > 0);
		CHECK(pool->GetStats().MovedBytes > 0);
		gpu.GetDevice().WaitForIdle();
		bool anyMoved = false;
		for (const Filled& entry : kept)
		{
			const GeometryAllocation now = *pool->Find(entry.Handle);
			anyMoved = anyMoved || now.FirstVertex != before[entry.Handle].FirstVertex || now.FirstIndex != before[entry.Handle].FirstIndex;
			CHECK(HoldsPattern(gpu, *pool, entry));
		}
		CHECK(anyMoved);
		CHECK(*pool->Find(kept.back().Handle) == pinned);
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("Culling's tables give each handle its place, submeshes and levels, and follow frees and compaction")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		GeometryPool::Specification specification = SmallPool();
		specification.StaticVertices = 1024;
		specification.Indices = 4096;
		const Ref<GeometryPool> pool = GeometryPool::Create(gpu.GetNvrhiDevice(), specification);
		REQUIRE(pool);

		std::map<GeometryHandle, TestLayout> layouts;
		std::vector<Filled> filled;
		for (uint32_t index = 0; index < 6; index++)
		{
			filled.push_back(AllocateFilled(gpu, *pool, index, 64 + index * 8, 192 + index * 24));
			TestLayout layout;
			layout.AddSubmesh(0, { { 0, 96 }, { 96, 48 }, { 144, 24 } });
			if (index % 2 == 1)
				layout.AddSubmesh(40, { { 168, 24 } });
			REQUIRE(pool->SetLayout(filled.back().Handle, layout.Submeshes, layout.LODs));
			layouts[filled.back().Handle] = layout;
		}
		CHECK(pool->GetLODCapacity() >= 3 * 6 + 3);
		CheckTables(gpu, *pool, layouts);
		// A handle that does not exist has no layout to set.
		TestLayout unused;
		unused.AddSubmesh(0, { { 0, 3 } });
		CHECK_FALSE(pool->SetLayout(c_InvalidGeometry, unused.Submeshes, unused.LODs));
		CHECK_FALSE(pool->SetLayout(1000, unused.Submeshes, unused.LODs));

		// A new layout replaces the handle's earlier one.
		TestLayout replaced;
		replaced.AddSubmesh(8, { { 0, 30 }, { 30, 12 } });
		REQUIRE(pool->SetLayout(filled[2].Handle, replaced.Submeshes, replaced.LODs));
		layouts[filled[2].Handle] = replaced;
		CheckTables(gpu, *pool, layouts);

		// Every other allocation is freed: once the GPU is done, their entries hold nothing to draw, and compaction moves the
		// others, whose entries follow.
		for (uint32_t index = 0; index < filled.size(); index += 2)
			pool->Free(filled[index].Handle);
		gpu.GetDevice().WaitForIdle();
		pool->BeginFrame();
		pool->BeginFrame();
		while (pool->Compact(256))
		{
		}
		CHECK(pool->GetStats().Moves > 0);
		gpu.GetDevice().WaitForIdle();
		CheckTables(gpu, *pool, layouts);
		for (uint32_t index = 1; index < filled.size(); index += 2)
			CHECK(HoldsPattern(gpu, *pool, filled[index]));
		CHECK(gpu.GetNewErrorCount() == 0);
	}

	TEST_CASE("A frame draws the same pixels before and after the renderer's pool compacts its meshes")
	{
		GPUContext gpu;
		REQUIRE(gpu.IsValid());
		// Meshes of several shapes in a pack, so they can be unloaded (memory assets cannot): the first ones go, and the later
		// ones' geometry has gaps below it.
		const std::vector<Ref<Mesh>> sources = { MeshFactory::CreateSphere(0.5f, 64, 32), MeshFactory::CreateTorus(0.4f, 0.15f, 64, 32),
			MeshFactory::CreateCylinder(0.4f, 1.0f, 48), MeshFactory::CreateCone(0.5f, 1.0f, 48) };
		std::vector<AssetMetadata> metadata;
		for (size_t index = 0; index < sources.size(); index++)
		{
			AssetMetadata entry;
			entry.Handle = UUID(0x6E000 + index);
			entry.Type = AssetType::Mesh;
			entry.Path = fmt::format("Shape{}.mesh", index);
			metadata.push_back(entry);
		}
		const std::filesystem::path pack = CreateTemporaryDirectory("GeometryPoolPack") / "Shapes.stpak";
		REQUIRE(AssetPackWriter::Write(pack, metadata, [&](const AssetMetadata& entry, std::vector<uint8_t>& outData, std::string*)
		{
			outData = sources[static_cast<uint64_t>(entry.Handle) - 0x6E000]->Serialize();
			return true;
		}));
		const Ref<RuntimeAssetManager> manager = RuntimeAssetManager::Create(pack);
		REQUIRE(manager);
		AssetManager::SetActive(manager);
		for (const AssetMetadata& entry : metadata)
			REQUIRE(manager->LoadAssetSync(entry.Handle));

		Scene scene;
		AddNeutralPostProcess(scene);
		AddSun(scene, glm::normalize(glm::vec3(-0.3f, -1.0f, -0.5f)), 3.0f);
		std::vector<Entity> entities;
		for (size_t index = 0; index < metadata.size(); index++)
			entities.push_back(AddMesh(scene, metadata[index].Handle, UUID::Null(), glm::vec3(static_cast<float>(index) * 1.5f - 2.25f, 0.0f, 0.0f), "Shape"));
		const SceneCamera camera = LookAt(glm::vec3(0.0f, 2.0f, 6.0f), glm::vec3(0.0f));

		SceneRenderer renderer;
		renderer.SetViewportSize(c_SceneTestSize, c_SceneTestSize);
		RenderToImage(renderer, scene, camera);
		// The sphere and the torus go: their geometry is freed once the GPU is done with the frames that drew them.
		scene.DestroyEntity(entities[0]);
		scene.DestroyEntity(entities[1]);
		manager->UnloadAsset(metadata[0].Handle);
		manager->UnloadAsset(metadata[1].Handle);
		const Ref<GeometryPool> pool = Renderer::GetGeometryPool();
		for (int frame = 0; frame < 4; frame++)
		{
			REQUIRE(renderer.Render(scene, camera));
			gpu.GetDevice().WaitForIdle();
			pool->BeginFrame();
		}
		const ReadbackImage before = RenderToImage(renderer, scene, camera);
		const uint32_t movesBefore = pool->GetStats().Moves;
		while (pool->Compact(GeometryPool::c_MaxDefragBytesPerFrame))
		{
		}
		CHECK(pool->GetStats().Moves > movesBefore);
		const ReadbackImage after = RenderToImage(renderer, scene, camera);
		CHECK(MaxDifference(before, after) == 0);
		AssetManager::SetActive(nullptr);
		CHECK(gpu.GetNewErrorCount() == 0);
	}
}
