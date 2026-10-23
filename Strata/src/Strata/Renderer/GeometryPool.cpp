#include "stpch.h"
#include "Strata/Renderer/GeometryPool.h"

#include "Strata/Renderer/Renderer.h"

#include <algorithm>

namespace Strata
{

	namespace
	{

		uint32_t RoundUp(uint32_t value, uint32_t granularity)
		{
			return (std::max(value, 1u) + granularity - 1) / granularity * granularity;
		}

	}

	Ref<GeometryPool> GeometryPool::Create(nvrhi::IDevice* device, const Specification& specification)
	{
		Ref<GeometryPool> pool(new GeometryPool(device, specification));
		for (const VertexArena& arena : pool->m_Arenas)
		{
			if (!arena.Positions || !arena.Attributes || (arena.Skinned && !arena.Skin))
				return nullptr;
		}
		if (!pool->m_Indices.Buffer)
			return nullptr;
		return pool;
	}

	Ref<GeometryPool> GeometryPool::Create(nvrhi::IDevice* device)
	{
		return Create(device, Specification());
	}

	GeometryPool::GeometryPool(nvrhi::IDevice* device, const Specification& specification)
		: m_Device(device)
	{
		m_Arenas[static_cast<size_t>(GeometryArena::Skinned)].Skinned = true;
		const std::array<uint32_t, c_GeometryArenaCount> vertices = { RoundUp(specification.StaticVertices, c_VertexGranularity),
			RoundUp(specification.SkinnedVertices, c_VertexGranularity) };
		for (size_t index = 0; index < c_GeometryArenaCount; index++)
		{
			if (CreateArenaBuffers(m_Arenas[index], vertices[index], nullptr))
				m_Arenas[index].Allocator.Grow(vertices[index]);
		}
		const uint32_t indices = RoundUp(specification.Indices, c_IndexGranularity);
		if (CreateIndexBuffer(indices, nullptr))
			m_Indices.Allocator.Grow(indices);
	}

	GeometryPool::~GeometryPool()
	{
		// An open command list was never executed: nothing it recorded ran.
		if (m_CommandListOpen)
		{
			m_Timer.reset();
			m_CommandList->close();
		}
	}

	uint64_t GeometryPool::GetAllocationBytes(GeometryArena arena, uint32_t vertices, uint32_t indices)
	{
		const uint64_t vertexBytes = c_PositionStride + c_AttributeStride + (arena == GeometryArena::Skinned ? c_SkinStride : 0);
		return static_cast<uint64_t>(RoundUp(vertices, c_VertexGranularity)) * vertexBytes + static_cast<uint64_t>(RoundUp(indices, c_IndexGranularity)) * c_IndexStride;
	}

	uint32_t GeometryPool::GetVertexBytes(const VertexArena& arena)
	{
		return c_PositionStride + c_AttributeStride + (arena.Skinned ? c_SkinStride : 0);
	}

	nvrhi::BufferHandle GeometryPool::CreateBuffer(uint64_t bytes, const char* name, bool vertex) const
	{
		nvrhi::BufferDesc desc;
		desc.byteSize = std::max<uint64_t>(bytes, 4);
		desc.debugName = name;
		desc.isVertexBuffer = vertex;
		desc.isIndexBuffer = !vertex;
		// Also read by shaders (GPU culling reads the pool's tables and, later, the geometry).
		desc.canHaveRawViews = true;
		desc.initialState = vertex ? nvrhi::ResourceStates::VertexBuffer : nvrhi::ResourceStates::IndexBuffer;
		desc.keepInitialState = true;
		return m_Device->createBuffer(desc);
	}

	nvrhi::ICommandList* GeometryPool::OpenCommandList()
	{
		if (!m_CommandList)
		{
			// Not an immediate command list: NVRHI's validation lets only one of those be open at a time, and growth under an
			// upload batch opens this list while the batch's graphics list is open (GPUUploadQueue::GetList). On Vulkan the
			// flag changes nothing else: the list still runs where it is executed, ahead of the batch's.
			m_CommandList = m_Device->createCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false));
		}
		if (!m_CommandList)
			return nullptr;
		if (!m_CommandListOpen)
		{
			m_CommandList->open();
			m_CommandListOpen = true;
			m_Timer.emplace(m_CommandList.Get(), RendererGPUStages::Get().Uploads);
		}
		return m_CommandList;
	}

	bool GeometryPool::CreateArenaBuffers(VertexArena& arena, uint32_t vertices, nvrhi::ICommandList* copyFrom)
	{
		const uint64_t count = vertices;
		nvrhi::BufferHandle positions = CreateBuffer(count * c_PositionStride, arena.Skinned ? "GeometryPool.SkinnedPositions" : "GeometryPool.Positions", true);
		nvrhi::BufferHandle attributes = CreateBuffer(count * c_AttributeStride, arena.Skinned ? "GeometryPool.SkinnedAttributes" : "GeometryPool.Attributes", true);
		nvrhi::BufferHandle skin = arena.Skinned ? CreateBuffer(count * c_SkinStride, "GeometryPool.Skin", true) : nullptr;
		if (!positions || !attributes || (arena.Skinned && !skin))
		{
			ST_CORE_ERROR("GeometryPool: failed to create vertex buffers of {} vertices", vertices);
			return false;
		}
		// What the old buffers hold moves over: up to the highest allocation.
		const uint64_t keep = arena.Allocator.GetHighWaterMark();
		if (copyFrom && keep > 0)
		{
			copyFrom->copyBuffer(positions, 0, arena.Positions, 0, keep * c_PositionStride);
			copyFrom->copyBuffer(attributes, 0, arena.Attributes, 0, keep * c_AttributeStride);
			if (arena.Skinned)
				copyFrom->copyBuffer(skin, 0, arena.Skin, 0, keep * c_SkinStride);
		}
		arena.Positions = std::move(positions);
		arena.Attributes = std::move(attributes);
		arena.Skin = std::move(skin);
		return true;
	}

	bool GeometryPool::CreateIndexBuffer(uint32_t indices, nvrhi::ICommandList* copyFrom)
	{
		nvrhi::BufferHandle buffer = CreateBuffer(static_cast<uint64_t>(indices) * c_IndexStride, "GeometryPool.Indices", false);
		if (!buffer)
		{
			ST_CORE_ERROR("GeometryPool: failed to create an index buffer of {} indices", indices);
			return false;
		}
		const uint64_t keep = m_Indices.Allocator.GetHighWaterMark();
		if (copyFrom && keep > 0)
			copyFrom->copyBuffer(buffer, 0, m_Indices.Buffer, 0, keep * c_IndexStride);
		m_Indices.Buffer = std::move(buffer);
		return true;
	}

	GeometryPool::AllocateResult GeometryPool::Allocate(GeometryArena arena, uint32_t vertices, uint32_t indices, uint64_t uploadBatch)
	{
		VertexArena& vertexArena = m_Arenas[static_cast<size_t>(arena)];
		const uint32_t vertexUnits = RoundUp(vertices, c_VertexGranularity);
		const uint32_t indexUnits = RoundUp(indices, c_IndexGranularity);
		// A mesh larger than the largest buffer never fits.
		if (static_cast<uint64_t>(vertexUnits) * c_AttributeStride > c_MaxBufferBytes || static_cast<uint64_t>(indexUnits) * c_IndexStride > c_MaxBufferBytes)
			return {};

		uint32_t firstVertex = RangeAllocator::c_InvalidOffset;
		uint32_t firstIndex = RangeAllocator::c_InvalidOffset;
		const auto tryAllocate = [&]()
		{
			firstVertex = vertexArena.Allocator.Allocate(vertexUnits);
			firstIndex = m_Indices.Allocator.Allocate(indexUnits);
			if (firstVertex != RangeAllocator::c_InvalidOffset && firstIndex != RangeAllocator::c_InvalidOffset)
				return true;
			if (firstVertex != RangeAllocator::c_InvalidOffset)
				vertexArena.Allocator.Free(firstVertex);
			else
				vertexArena.Requested = std::max(vertexArena.Requested, vertexUnits);
			if (firstIndex != RangeAllocator::c_InvalidOffset)
				m_Indices.Allocator.Free(firstIndex);
			else
				m_Indices.Requested = std::max(m_Indices.Requested, indexUnits);
			return false;
		};
		if (!tryAllocate())
		{
			// The ranges the GPU is done with first, then growth: at once while the open batch has not written into the buffers
			// (its later copies go to the new ones), else at the next BeginFrame, between batches; the mesh asks again then.
			CollectFrees();
			vertexArena.Requested = 0;
			m_Indices.Requested = 0;
			if (!tryAllocate())
			{
				if (uploadBatch != 0 && m_LastWriteBatch == uploadBatch)
					return AllocateResult { c_InvalidGeometry, true };
				std::vector<nvrhi::ResourceHandle> replaced;
				GrowRequested(replaced);
				SubmitCommandList(std::move(replaced));
				if (!tryAllocate())
				{
					// Growth failed (logged): it is tried again at the next frame.
					return AllocateResult { c_InvalidGeometry, true };
				}
			}
		}

		GeometryHandle handle = c_InvalidGeometry;
		if (!m_FreeHandles.empty())
		{
			handle = m_FreeHandles.back();
			m_FreeHandles.pop_back();
		}
		else
		{
			m_Entries.emplace_back();
			handle = static_cast<GeometryHandle>(m_Entries.size());
		}
		Entry& entry = m_Entries[handle - 1];
		entry = Entry();
		entry.Allocation = GeometryAllocation { arena, firstVertex, vertexUnits, firstIndex, indexUnits };
		entry.Live = true;
		entry.Uploading = true;
		vertexArena.Owners[firstVertex] = handle;
		m_Indices.Owners[firstIndex] = handle;
		WriteEntry(handle);
		return AllocateResult { handle, false };
	}

	void GeometryPool::Free(GeometryHandle handle)
	{
		if (handle == c_InvalidGeometry)
			return;
		std::scoped_lock<std::mutex> lock(m_FreeMutex);
		m_Freed.push_back(handle);
	}

	void GeometryPool::SetUploading(GeometryHandle handle, bool uploading)
	{
		if (handle != c_InvalidGeometry && handle <= m_Entries.size() && m_Entries[handle - 1].Live)
			m_Entries[handle - 1].Uploading = uploading;
	}

	const GeometryAllocation* GeometryPool::Find(GeometryHandle handle) const
	{
		if (handle == c_InvalidGeometry || handle > m_Entries.size() || !m_Entries[handle - 1].Live)
			return nullptr;
		return &m_Entries[handle - 1].Allocation;
	}

	void GeometryPool::ReleaseHandle(GeometryHandle handle)
	{
		if (handle == c_InvalidGeometry || handle > m_Entries.size() || !m_Entries[handle - 1].Live)
			return;
		Entry& entry = m_Entries[handle - 1];
		VertexArena& arena = m_Arenas[static_cast<size_t>(entry.Allocation.Arena)];
		arena.Allocator.Free(entry.Allocation.FirstVertex);
		arena.Owners.erase(entry.Allocation.FirstVertex);
		m_Indices.Allocator.Free(entry.Allocation.FirstIndex);
		m_Indices.Owners.erase(entry.Allocation.FirstIndex);
		ReleaseLayout(entry);
		entry = Entry();
		WriteEntry(handle);
		m_FreeHandles.push_back(handle);
	}

	void GeometryPool::WriteEntry(GeometryHandle handle)
	{
		m_EntryTable.EnsureSize(handle + 1);
		const Entry& entry = m_Entries[handle - 1];
		RenderData::GeometryEntry& record = m_EntryTable.Records[handle];
		record = RenderData::GeometryEntry {};
		if (entry.Live)
		{
			record.FirstVertex = entry.Allocation.FirstVertex;
			record.FirstIndex = entry.Allocation.FirstIndex;
			record.FirstSubmesh = entry.SubmeshCount > 0 ? entry.FirstSubmesh : 0;
			record.SubmeshCount = entry.SubmeshCount;
		}
		m_EntryTable.MarkDirty(handle);
		m_ContentVersion++;
	}

	void GeometryPool::ReleaseLayout(Entry& entry)
	{
		if (entry.SubmeshCount > 0)
			m_SubmeshAllocator.Free(entry.FirstSubmesh);
		if (entry.LODCount > 0)
			m_LODAllocator.Free(entry.FirstLOD);
		entry.FirstSubmesh = RangeAllocator::c_InvalidOffset;
		entry.SubmeshCount = 0;
		entry.FirstLOD = RangeAllocator::c_InvalidOffset;
		entry.LODCount = 0;
	}

	bool GeometryPool::SetLayout(GeometryHandle handle, std::span<const RenderData::GeometrySubmesh> submeshes, std::span<const RenderData::GeometryLOD> lods)
	{
		if (handle == c_InvalidGeometry || handle > m_Entries.size() || !m_Entries[handle - 1].Live)
			return false;
		Entry& entry = m_Entries[handle - 1];
		ReleaseLayout(entry);
		const uint32_t submeshCount = static_cast<uint32_t>(submeshes.size());
		const uint32_t lodCount = static_cast<uint32_t>(lods.size());
		const auto allocate = [](RangeAllocator& allocator, uint32_t count)
		{
			if (count == 0)
				return RangeAllocator::c_InvalidOffset;
			uint32_t offset = allocator.Allocate(count);
			if (offset == RangeAllocator::c_InvalidOffset)
			{
				// Tables grow by at least twice their size: they are small, and uploaded whole when they grow.
				allocator.Grow(std::max(allocator.GetCapacity() * 2, allocator.GetCapacity() + count + 64));
				offset = allocator.Allocate(count);
			}
			return offset;
		};
		const uint32_t firstSubmesh = allocate(m_SubmeshAllocator, submeshCount);
		const uint32_t firstLOD = allocate(m_LODAllocator, lodCount);
		if ((submeshCount > 0 && firstSubmesh == RangeAllocator::c_InvalidOffset) || (lodCount > 0 && firstLOD == RangeAllocator::c_InvalidOffset))
		{
			if (firstSubmesh != RangeAllocator::c_InvalidOffset)
				m_SubmeshAllocator.Free(firstSubmesh);
			if (firstLOD != RangeAllocator::c_InvalidOffset)
				m_LODAllocator.Free(firstLOD);
			return false;
		}
		entry.FirstSubmesh = firstSubmesh;
		entry.SubmeshCount = submeshCount;
		entry.FirstLOD = firstLOD;
		entry.LODCount = lodCount;
		m_SubmeshTable.EnsureSize(m_SubmeshAllocator.GetCapacity());
		m_LODTable.EnsureSize(m_LODAllocator.GetCapacity());
		for (uint32_t index = 0; index < submeshCount; index++)
		{
			RenderData::GeometrySubmesh submesh = submeshes[index];
			submesh.FirstLOD += firstLOD;
			m_SubmeshTable.Records[firstSubmesh + index] = submesh;
			m_SubmeshTable.MarkDirty(firstSubmesh + index);
		}
		for (uint32_t index = 0; index < lodCount; index++)
		{
			RenderData::GeometryLOD lod = lods[index];
			lod.Mesh = handle;
			m_LODTable.Records[firstLOD + index] = lod;
			m_LODTable.MarkDirty(firstLOD + index);
		}
		WriteEntry(handle);
		return true;
	}

	template<typename Record>
	bool GeometryPool::UploadTable(nvrhi::ICommandList* commandList, Table<Record>& table, const char* name, uint64_t& bytes)
	{
		if (table.Records.empty())
			table.EnsureSize(1);
		if (table.Recreate)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = table.Records.size() * sizeof(Record);
			desc.structStride = sizeof(Record);
			desc.debugName = name;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			nvrhi::BufferHandle buffer = m_Device->createBuffer(desc);
			if (!buffer)
			{
				ST_CORE_ERROR("GeometryPool: failed to create its {} table ({} entries)", name, table.Records.size());
				return false;
			}
			// Frames in flight may still read the replaced table.
			if (table.Buffer)
				Renderer::ReleaseDeferred({ nvrhi::ResourceHandle(table.Buffer.Get()) });
			table.Buffer = std::move(buffer);
			commandList->writeBuffer(table.Buffer, table.Records.data(), table.Records.size() * sizeof(Record));
			bytes += table.Records.size() * sizeof(Record);
			table.Recreate = false;
			m_TableVersion++;
		}
		else if (!table.DirtyList.empty())
		{
			// Runs of consecutive dirty records, one write each.
			std::sort(table.DirtyList.begin(), table.DirtyList.end());
			size_t start = 0;
			for (size_t index = 1; index <= table.DirtyList.size(); index++)
			{
				if (index < table.DirtyList.size() && table.DirtyList[index] == table.DirtyList[index - 1] + 1)
					continue;
				const uint32_t first = table.DirtyList[start];
				const uint32_t end = table.DirtyList[index - 1] + 1;
				commandList->writeBuffer(table.Buffer, table.Records.data() + first, (end - first) * sizeof(Record), first * sizeof(Record));
				bytes += (end - first) * sizeof(Record);
				start = index;
			}
		}
		for (const uint32_t index : table.DirtyList)
			table.Dirty[index] = 0;
		table.DirtyList.clear();
		return true;
	}

	bool GeometryPool::RecordTableUploads(nvrhi::ICommandList* commandList, uint64_t* outBytes)
	{
		uint64_t bytes = 0;
		const bool uploaded = UploadTable(commandList, m_EntryTable, "GeometryPool.Entries", bytes) && UploadTable(commandList, m_SubmeshTable, "GeometryPool.Submeshes", bytes)
			&& UploadTable(commandList, m_LODTable, "GeometryPool.LODs", bytes);
		if (outBytes)
			*outBytes = bytes;
		return uploaded;
	}

	bool GeometryPool::EnsureScratch(uint64_t bytes)
	{
		if (m_Scratch && m_Scratch->getDesc().byteSize >= bytes)
			return true;
		nvrhi::BufferDesc desc;
		desc.byteSize = std::max<uint64_t>(bytes, c_MaxDefragBytesPerFrame);
		desc.debugName = "GeometryPool.Scratch";
		desc.initialState = nvrhi::ResourceStates::CopySource;
		desc.keepInitialState = true;
		nvrhi::BufferHandle scratch = m_Device->createBuffer(desc);
		if (!scratch)
			return false;
		if (m_Scratch)
			Renderer::ReleaseDeferred({ nvrhi::ResourceHandle(m_Scratch.Get()) });
		m_Scratch = std::move(scratch);
		return true;
	}

	void GeometryPool::RecordMove(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer, uint64_t from, uint64_t to, uint64_t bytes)
	{
		// Through the scratch buffer: a slide's old and new ranges overlap, and a copy within one buffer may not.
		commandList->copyBuffer(m_Scratch, 0, buffer, from, bytes);
		commandList->copyBuffer(buffer, to, m_Scratch, 0, bytes);
	}

	bool GeometryPool::CompactArenas(nvrhi::ICommandList* commandList, uint64_t maxBytes, bool force)
	{
		bool moved = false;
		uint64_t budget = maxBytes;
		const auto movable = [this](const std::unordered_map<uint32_t, GeometryHandle>& owners)
		{
			return [this, &owners](uint32_t offset)
			{
				const auto owner = owners.find(offset);
				return owner != owners.end() && !m_Entries[owner->second - 1].Uploading;
			};
		};

		for (VertexArena& arena : m_Arenas)
		{
			if (budget == 0 || (!force && arena.Allocator.GetScatteredFreeShare() <= c_CompactionThreshold))
				continue;
			const uint32_t vertexBytes = GetVertexBytes(arena);
			const uint32_t maxUnits = static_cast<uint32_t>(std::max<uint64_t>(budget / vertexBytes, 1));
			const std::vector<RangeAllocator::Move> moves = arena.Allocator.PlanCompaction(maxUnits, movable(arena.Owners));
			for (const RangeAllocator::Move& move : moves)
			{
				if (!EnsureScratch(static_cast<uint64_t>(move.Size) * c_AttributeStride))
				{
					ST_CORE_ERROR("GeometryPool: no scratch buffer for compaction");
					return moved;
				}
				RecordMove(commandList, arena.Positions, uint64_t(move.From) * c_PositionStride, uint64_t(move.To) * c_PositionStride, uint64_t(move.Size) * c_PositionStride);
				RecordMove(commandList, arena.Attributes, uint64_t(move.From) * c_AttributeStride, uint64_t(move.To) * c_AttributeStride, uint64_t(move.Size) * c_AttributeStride);
				if (arena.Skinned)
					RecordMove(commandList, arena.Skin, uint64_t(move.From) * c_SkinStride, uint64_t(move.To) * c_SkinStride, uint64_t(move.Size) * c_SkinStride);
				const GeometryHandle handle = arena.Owners.at(move.From);
				arena.Owners.erase(move.From);
				arena.Owners[move.To] = handle;
				m_Entries[handle - 1].Allocation.FirstVertex = move.To;
				WriteEntry(handle);
				const uint64_t bytes = static_cast<uint64_t>(move.Size) * vertexBytes;
				budget -= std::min(budget, bytes);
				m_MovedBytes += bytes;
				m_Moves++;
				moved = true;
			}
		}

		if (budget > 0 && (force || m_Indices.Allocator.GetScatteredFreeShare() > c_CompactionThreshold))
		{
			const uint32_t maxUnits = static_cast<uint32_t>(std::max<uint64_t>(budget / c_IndexStride, 1));
			const std::vector<RangeAllocator::Move> moves = m_Indices.Allocator.PlanCompaction(maxUnits, movable(m_Indices.Owners));
			for (const RangeAllocator::Move& move : moves)
			{
				const uint64_t bytes = static_cast<uint64_t>(move.Size) * c_IndexStride;
				if (!EnsureScratch(bytes))
				{
					ST_CORE_ERROR("GeometryPool: no scratch buffer for compaction");
					return moved;
				}
				RecordMove(commandList, m_Indices.Buffer, uint64_t(move.From) * c_IndexStride, uint64_t(move.To) * c_IndexStride, bytes);
				const GeometryHandle handle = m_Indices.Owners.at(move.From);
				m_Indices.Owners.erase(move.From);
				m_Indices.Owners[move.To] = handle;
				m_Entries[handle - 1].Allocation.FirstIndex = move.To;
				WriteEntry(handle);
				m_MovedBytes += bytes;
				m_Moves++;
				moved = true;
			}
		}
		return moved;
	}

	bool GeometryPool::Compact(uint64_t maxBytes)
	{
		nvrhi::ICommandList* commandList = OpenCommandList();
		if (!commandList)
			return false;
		const bool moved = CompactArenas(commandList, maxBytes, true);
		SubmitCommandList({});
		return moved;
	}

	void GeometryPool::CollectFrees()
	{
		// Frees since the last call wait for the work submitted so far: the frames that drew those meshes.
		std::vector<GeometryHandle> freed;
		{
			std::scoped_lock<std::mutex> lock(m_FreeMutex);
			freed.swap(m_Freed);
		}
		if (!freed.empty())
		{
			PendingFree pending;
			pending.Handles = std::move(freed);
			pending.Done = m_Device->createEventQuery();
			if (pending.Done)
				m_Device->setEventQuery(pending.Done, nvrhi::CommandQueue::Graphics);
			m_PendingFrees.push_back(std::move(pending));
		}
		while (!m_PendingFrees.empty() && (!m_PendingFrees.front().Done || m_Device->pollEventQuery(m_PendingFrees.front().Done)))
		{
			for (const GeometryHandle handle : m_PendingFrees.front().Handles)
				ReleaseHandle(handle);
			m_PendingFrees.pop_front();
		}
	}

	void GeometryPool::GrowRequested(std::vector<nvrhi::ResourceHandle>& replaced)
	{
		// At least twice the size, and enough for what was asked.
		for (VertexArena& arena : m_Arenas)
		{
			if (arena.Requested == 0)
				continue;
			const uint32_t capacity = arena.Allocator.GetCapacity();
			const uint64_t wanted = std::max<uint64_t>(uint64_t(capacity) * 2, uint64_t(capacity) + arena.Requested);
			const uint64_t limit = c_MaxBufferBytes / c_AttributeStride;
			const uint32_t grown = static_cast<uint32_t>(std::min(wanted, limit) / c_VertexGranularity * c_VertexGranularity);
			arena.Requested = 0;
			if (grown <= capacity)
				continue;
			nvrhi::ICommandList* commandList = OpenCommandList();
			const std::array<nvrhi::ResourceHandle, 3> old = { arena.Positions.Get(), arena.Attributes.Get(), arena.Skin.Get() };
			if (!commandList || !CreateArenaBuffers(arena, grown, commandList))
				continue;
			for (const nvrhi::ResourceHandle& buffer : old)
			{
				if (buffer)
					replaced.push_back(buffer);
			}
			arena.Allocator.Grow(grown);
			m_Growths++;
			m_BufferVersion++;
		}
		if (m_Indices.Requested > 0)
		{
			const uint32_t capacity = m_Indices.Allocator.GetCapacity();
			const uint64_t wanted = std::max<uint64_t>(uint64_t(capacity) * 2, uint64_t(capacity) + m_Indices.Requested);
			const uint32_t grown = static_cast<uint32_t>(std::min(wanted, c_MaxBufferBytes / c_IndexStride) / c_IndexGranularity * c_IndexGranularity);
			m_Indices.Requested = 0;
			nvrhi::ICommandList* commandList = grown > capacity ? OpenCommandList() : nullptr;
			const nvrhi::ResourceHandle old = m_Indices.Buffer.Get();
			if (commandList && CreateIndexBuffer(grown, commandList))
			{
				replaced.push_back(old);
				m_Indices.Allocator.Grow(grown);
				m_Growths++;
				m_BufferVersion++;
			}
		}
	}

	void GeometryPool::SubmitCommandList(std::vector<nvrhi::ResourceHandle> replaced)
	{
		if (m_CommandListOpen)
		{
			m_Timer.reset(); // Its last timestamp, before the list closes
			m_CommandList->close();
			m_CommandListOpen = false;
			m_Device->executeCommandList(m_CommandList);
		}
		// The replaced buffers go once the GPU ran the copies out of them (and the frames that drew from them).
		if (!replaced.empty())
			Renderer::ReleaseDeferred(std::move(replaced));
	}

	void GeometryPool::BeginFrame()
	{
		CollectFrees();
		std::vector<nvrhi::ResourceHandle> replaced;
		GrowRequested(replaced);

		// Compaction of fragmented arenas, within the frame's budget.
		const bool fragmented = std::any_of(m_Arenas.begin(), m_Arenas.end(), [](const VertexArena& arena) { return arena.Allocator.GetScatteredFreeShare() > c_CompactionThreshold; })
			|| m_Indices.Allocator.GetScatteredFreeShare() > c_CompactionThreshold;
		if (fragmented)
		{
			if (nvrhi::ICommandList* commandList = OpenCommandList())
				CompactArenas(commandList, c_MaxDefragBytesPerFrame, false);
		}
		SubmitCommandList(std::move(replaced));
	}

	GeometryPoolStats GeometryPool::GetStats() const
	{
		GeometryPoolStats stats;
		for (const VertexArena& arena : m_Arenas)
		{
			const uint64_t vertexBytes = GetVertexBytes(arena);
			stats.UsedBytes += arena.Allocator.GetUsedUnits() * vertexBytes;
			stats.CapacityBytes += arena.Allocator.GetCapacity() * vertexBytes;
			stats.Allocations += arena.Allocator.GetAllocationCount();
		}
		stats.UsedBytes += uint64_t(m_Indices.Allocator.GetUsedUnits()) * c_IndexStride;
		stats.CapacityBytes += uint64_t(m_Indices.Allocator.GetCapacity()) * c_IndexStride;
		for (const PendingFree& pending : m_PendingFrees)
			stats.PendingFrees += static_cast<uint32_t>(pending.Handles.size());
		stats.VertexFragmentation = m_Arenas[static_cast<size_t>(GeometryArena::Static)].Allocator.GetFragmentation();
		stats.IndexFragmentation = m_Indices.Allocator.GetFragmentation();
		stats.Growths = m_Growths;
		stats.MovedBytes = m_MovedBytes;
		stats.Moves = m_Moves;
		return stats;
	}

}
