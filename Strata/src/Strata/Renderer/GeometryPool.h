#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/RangeAllocator.h"
#include "Strata/Renderer/GPUProfiler.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace Strata
{

	// The vertex arena a mesh's vertices live in: positions and attributes (stream 0 and 1), plus the skin stream (stream 2)
	// for skinned meshes.
	enum class GeometryArena : uint8_t
	{
		Static = 0,
		Skinned
	};
	constexpr size_t c_GeometryArenaCount = 2;

	// A mesh's place in the geometry pool, stable for the mesh's life while its ranges may move (GeometryPool::Find).
	using GeometryHandle = uint32_t;
	constexpr GeometryHandle c_InvalidGeometry = 0;

	// Where a mesh's geometry is now: its vertices in an arena (vertex offsets; a submesh's indices are relative to its
	// base vertex, which adds to FirstVertex) and its indices in the index arena.
	struct GeometryAllocation
	{
		GeometryArena Arena = GeometryArena::Static;
		uint32_t FirstVertex = 0;
		uint32_t VertexCount = 0; // Allocated: the mesh's vertices rounded up to GeometryPool::c_VertexGranularity
		uint32_t FirstIndex = 0;
		uint32_t IndexCount = 0;  // Allocated: the mesh's indices rounded up to GeometryPool::c_IndexGranularity

		bool operator==(const GeometryAllocation&) const = default;
	};

	struct GeometryPoolStats
	{
		uint64_t UsedBytes = 0;      // Allocated ranges of every stream
		uint64_t CapacityBytes = 0;  // The pool's buffers
		uint32_t Allocations = 0;
		uint32_t PendingFrees = 0;   // Freed meshes whose ranges wait for the GPU to finish the frames that may read them
		float VertexFragmentation = 0.0f; // RangeAllocator::GetFragmentation of the static arena
		float IndexFragmentation = 0.0f;
		uint32_t Growths = 0;        // Buffers recreated larger (by a GPU copy) since the pool was created
		uint64_t MovedBytes = 0;     // Bytes compaction copied since the pool was created
		uint32_t Moves = 0;
	};

	// Every mesh's GPU geometry, in shared buffers (Renderer::GetGeometryPool): two vertex arenas - Static (positions 12 B
	// and attributes 36 B per vertex, in two buffers) and Skinned (the same plus the 16-byte skin stream) - and one index arena
	// of 32-bit indices. A draw binds the arena's buffers once and reaches any mesh by its offsets, so draws of different
	// meshes share their state (and indirect draws).
	//
	// Meshes get a stable GeometryHandle; the pool's table maps it to the current GeometryAllocation, so moving geometry
	// changes no mesh. Freed ranges become reusable once the GPU finished the work submitted before they were freed (event
	// queries, like DeferredReleaseQueue). The pool grows by creating larger buffers and copying on the GPU (the old ones go
	// through Renderer::ReleaseDeferred), and compacts by GPU copies within c_MaxDefragBytesPerFrame when the scattered free
	// space exceeds c_CompactionThreshold of an arena: both in BeginFrame, between upload batches, never moving geometry
	// whose upload is in progress. Main thread only, but for Free.
	class GeometryPool
	{
	public:
		static constexpr uint32_t c_PositionStride = 12;
		static constexpr uint32_t c_AttributeStride = 36;
		static constexpr uint32_t c_SkinStride = 16;
		static constexpr uint32_t c_IndexStride = 4;
		// Allocations are whole multiples of these (fewer, larger free ranges).
		static constexpr uint32_t c_VertexGranularity = 16;
		static constexpr uint32_t c_IndexGranularity = 64;
		static constexpr uint64_t c_MaxDefragBytesPerFrame = 8ull << 20;
		// An arena compacts while the free space outside its largest free range exceeds this share of its capacity.
		static constexpr float c_CompactionThreshold = 0.25f;
		// The largest buffer the pool creates (per stream).
		static constexpr uint64_t c_MaxBufferBytes = 2ull << 30;

		struct Specification
		{
			uint32_t StaticVertices = 1u << 16;
			uint32_t SkinnedVertices = 1u << 12;
			uint32_t Indices = 1u << 18;
		};

		// What Allocate found.
		struct AllocateResult
		{
			GeometryHandle Handle = c_InvalidGeometry;
			// No handle: whether the pool grows at the next BeginFrame so that it fits (try again then), or it never can.
			bool Retry = false;
		};

		static Ref<GeometryPool> Create(nvrhi::IDevice* device, const Specification& specification);
		static Ref<GeometryPool> Create(nvrhi::IDevice* device);
		~GeometryPool();

		GeometryPool(const GeometryPool&) = delete;
		GeometryPool& operator=(const GeometryPool&) = delete;

		// The bytes an allocation of these counts takes (rounded to the granularities), for memory accounting.
		static uint64_t GetAllocationBytes(GeometryArena arena, uint32_t vertices, uint32_t indices);

		// Ranges for a mesh of `vertices` vertices and `indices` indices. The new allocation counts as uploading (it does not
		// move) until SetUploading(handle, false). `uploadBatch` is the open upload batch (GPUUploadQueue::GetOpenBatch):
		// when they do not fit, the pool takes back the freed ranges the GPU is done with and, unless that batch already
		// wrote into its buffers (NoteUploadWrite), grows at once (a command list executed ahead of the batch's); otherwise
		// it grows at the next BeginFrame (Retry).
		AllocateResult Allocate(GeometryArena arena, uint32_t vertices, uint32_t indices, uint64_t uploadBatch);
		// An upload batch (GPUUploadQueue::GetOpenBatch) records copies into the pool's buffers: the pool does not grow under it.
		void NoteUploadWrite(uint64_t uploadBatch) { m_LastWriteBatch = uploadBatch; }
		// Frees a handle's ranges once the GPU finished the work submitted so far. Any thread (a mesh's last reference may go
		// on a worker); the ranges return in a later BeginFrame.
		void Free(GeometryHandle handle);
		// Whether an upload into the handle's ranges is in progress: such geometry never moves.
		void SetUploading(GeometryHandle handle, bool uploading);
		// The handle's current ranges (null for an invalid or freed handle). Valid until the next BeginFrame or Allocate.
		const GeometryAllocation* Find(GeometryHandle handle) const;

		nvrhi::IBuffer* GetPositionBuffer(GeometryArena arena) const { return m_Arenas[static_cast<size_t>(arena)].Positions; }
		nvrhi::IBuffer* GetAttributeBuffer(GeometryArena arena) const { return m_Arenas[static_cast<size_t>(arena)].Attributes; }
		nvrhi::IBuffer* GetSkinBuffer() const { return m_Arenas[static_cast<size_t>(GeometryArena::Skinned)].Skin; }
		nvrhi::IBuffer* GetIndexBuffer() const { return m_Indices.Buffer; }
		// Changes whenever a buffer is replaced (growth): binding sets that reference the buffers are made again.
		uint64_t GetBufferVersion() const { return m_BufferVersion; }

		// Returns freed ranges the GPU is done with, grows the arenas that ran out (Allocate asked for it) and compacts
		// fragmented ones within c_MaxDefragBytesPerFrame, recording the copies into a command list of its own executed on
		// the graphics queue ahead of the frame's uploads (Renderer::BeginFrame). Never waits for the GPU.
		void BeginFrame();
		// Compacts every arena as far as `maxBytes` of copies allow, whatever its fragmentation; false when nothing moved
		// (tests and tools).
		bool Compact(uint64_t maxBytes);

		GeometryPoolStats GetStats() const;
		uint32_t GetVertexCapacity(GeometryArena arena) const { return m_Arenas[static_cast<size_t>(arena)].Allocator.GetCapacity(); }
		uint32_t GetIndexCapacity() const { return m_Indices.Allocator.GetCapacity(); }
	private:
		struct VertexArena
		{
			RangeAllocator Allocator;
			nvrhi::BufferHandle Positions;
			nvrhi::BufferHandle Attributes;
			nvrhi::BufferHandle Skin; // Skinned arena only
			std::unordered_map<uint32_t, GeometryHandle> Owners; // First vertex to handle
			uint32_t Requested = 0; // Vertices an allocation could not get: the next BeginFrame grows by at least this much
			bool Skinned = false;
		};

		struct IndexArena
		{
			RangeAllocator Allocator;
			nvrhi::BufferHandle Buffer;
			std::unordered_map<uint32_t, GeometryHandle> Owners; // First index to handle
			uint32_t Requested = 0;
		};

		struct Entry
		{
			GeometryAllocation Allocation;
			bool Live = false;
			bool Uploading = false;
		};

		struct PendingFree
		{
			std::vector<GeometryHandle> Handles;
			nvrhi::EventQueryHandle Done;
		};

		GeometryPool(nvrhi::IDevice* device, const Specification& specification);
		bool CreateArenaBuffers(VertexArena& arena, uint32_t vertices, nvrhi::ICommandList* copyFrom);
		bool CreateIndexBuffer(uint32_t indices, nvrhi::ICommandList* copyFrom);
		nvrhi::BufferHandle CreateBuffer(uint64_t bytes, const char* name, bool vertex) const;
		static uint32_t GetVertexBytes(const VertexArena& arena);
		void ReleaseHandle(GeometryHandle handle);
		// Copies [from, from + bytes) to [to, to + bytes) of a buffer through the scratch buffer (the ranges may overlap).
		void RecordMove(nvrhi::ICommandList* commandList, nvrhi::IBuffer* buffer, uint64_t from, uint64_t to, uint64_t bytes);
		bool EnsureScratch(uint64_t bytes);
		// Compacts the arenas over the threshold (or all with `force`) within `maxBytes`; true when something moved.
		bool CompactArenas(nvrhi::ICommandList* commandList, uint64_t maxBytes, bool force);
		nvrhi::ICommandList* OpenCommandList();
		// Executes what the pool's command list recorded and lets go of the buffers it replaced.
		void SubmitCommandList(std::vector<nvrhi::ResourceHandle> replaced);
		// Takes the handles freed since the last call into a fenced batch, and returns the ranges of the batches the GPU is
		// done with.
		void CollectFrees();
		// Grows the arenas allocations asked more of (Requested), recording the copies; the buffers replaced are appended.
		void GrowRequested(std::vector<nvrhi::ResourceHandle>& replaced);
	private:
		nvrhi::IDevice* m_Device = nullptr;
		std::array<VertexArena, c_GeometryArenaCount> m_Arenas;
		IndexArena m_Indices;
		std::vector<Entry> m_Entries;               // By handle - 1
		std::vector<GeometryHandle> m_FreeHandles;  // Released handles, reused by later allocations
		std::mutex m_FreeMutex;
		std::vector<GeometryHandle> m_Freed;        // Freed since the last BeginFrame (any thread, under m_FreeMutex)
		std::deque<PendingFree> m_PendingFrees;     // Oldest first
		nvrhi::CommandListHandle m_CommandList;
		bool m_CommandListOpen = false;
		std::optional<GPUPassTimer> m_Timer; // The GPU's time for the copies (GPU.Uploads)
		nvrhi::BufferHandle m_Scratch;
		uint64_t m_BufferVersion = 1;
		uint64_t m_LastWriteBatch = 0; // The upload batch that last wrote into the buffers (NoteUploadWrite)
		uint32_t m_Growths = 0;
		uint64_t m_MovedBytes = 0;
		uint32_t m_Moves = 0;
	};

}
