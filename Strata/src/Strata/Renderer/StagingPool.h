#pragma once

#include "Strata/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace Strata
{

	// CPU-writable staging memory for uploads, reused rather than allocated for every upload: staging buffers in powers of
	// two of bytes, through which the upload queue copies texture levels and buffer ranges (GPUUploadQueue). Released
	// staging is held until the GPU has run the copies that read it, whatever the command lists that record them reference
	// (copies from buffers into textures reference neither: GraphicsDevice::RecordBufferToTextureCopies), and then becomes
	// reusable: an event query set after the command lists with those copies were executed tells when. The asset managers set it as
	// soon as they executed the frame's uploads (FenceReleased, through GPUUploadQueue::SubmitBatch), and BeginFrame sets one
	// for whatever was released since; the queries are polled, never waited for, at BeginFrame, before staging is acquired
	// and when a budgeted upload finds no room (HasRoomFor). The GPU runs a frame's uploads ahead of that frame's rendering,
	// so with the GPU keeping up the staging of a frame's uploads is reusable in the next frame, and sustained uploads can
	// use the budget again every frame (fenced only at the next BeginFrame, it would wait for the frame's rendering too and
	// be reused two frames after its release: half the budget per frame). Idle staging is kept up to c_MaxIdleBytes, and
	// all of it is released once nothing was acquired for c_IdleReleaseFrames frames, so staging memory follows the uploads
	// in flight instead of staying at the high-water mark of a burst (and a steady stream of uploads allocates nothing).
	//
	// Budgeted uploads bound staging as a whole: they wait while what is in flight leaves no room within their budget
	// (HasRoomFor), and pass the budget to AcquireBuffer, which releases idle staging of other sizes before it creates
	// staging that would put the pool (idle, in flight and new) over it. The graphics driver keeps the
	// memory of freed staging committed for a while, so the process pays for the high-water mark of staging, not for what
	// it holds now: idle staging must not come on top of the budget. Main thread only; staging must be released after the
	// copies that read it are recorded, and those command lists executed before the next FenceReleased or BeginFrame.
	class StagingPool
	{
	public:
		static constexpr uint64_t c_Unlimited = std::numeric_limits<uint64_t>::max();

		// `maxPendingBytes`: how much staging waiting for the GPU is pooled (see c_MaxPendingBytes).
		explicit StagingPool(nvrhi::IDevice* device, uint64_t maxPendingBytes = c_MaxPendingBytes);
		// Drops the staging it holds: the owner waits for the GPU first (Renderer::Shutdown does).
		~StagingPool();

		StagingPool(const StagingPool&) = delete;
		StagingPool& operator=(const StagingPool&) = delete;

		// A CPU-writable buffer of GetBufferByteSize(bytes) bytes to copy from into GPU buffers and textures (a copy source),
		// free for writing: the most recently released idle one of that size, or a new one. Other idle staging is released
		// while the pool (idle, in flight and the buffer handed out) would otherwise hold more than `limit` bytes, also when
		// an idle one is reused (unbudgeted uploads may have left the pool above the limit). Null when one cannot be created.
		nvrhi::BufferHandle AcquireBuffer(uint64_t bytes, uint64_t limit = c_Unlimited);
		// Hands staging back once the copies that read it are recorded, in a command list of `queue` (the graphics queue, or
		// the transfer queue for the upload queue's copies there). The pool holds it until the GPU has run them (the query of
		// the next FenceReleased or BeginFrame of that queue completed), also when it is not pooled (beyond the pending
		// limit): the caller may drop every other reference at once.
		void Release(nvrhi::BufferHandle staging, nvrhi::CommandQueue queue = nvrhi::CommandQueue::Graphics);
		// Once every command list of `queue` that copies from the staging released for it so far was executed: sets the event
		// query on that queue that tells when the GPU has run those copies, so that the staging becomes reusable without
		// waiting for work submitted later (GPUUploadQueue::SubmitBatch calls it after executing a batch). Does nothing when
		// nothing was released for the queue since its latest query.
		void FenceReleased(nvrhi::CommandQueue queue = nvrhi::CommandQueue::Graphics);

		// Once per frame, when the device began frame `frameIndex` (Renderer::BeginFrame): staging whose copies the GPU has
		// finished becomes idle, what was released since the latest query gets one (FenceReleased), and idle staging beyond
		// the limits is released.
		void BeginFrame(uint64_t frameIndex);
		// Releases all idle staging, after making what the GPU is done with idle (polled). Returns the bytes released. For
		// owners that know uploads stopped without device frames (RendererUploadContext::EndFrame).
		uint64_t ReleaseIdle();

		// Whether AcquireBuffer(bytes) would reuse idle staging rather than create a buffer, as far as the latest poll knows
		// (staging the GPU finished with since then counts as not idle yet).
		bool HasIdleBuffer(uint64_t bytes) const;
		uint64_t GetIdleBytes() const { return m_IdleBytes; }
		uint64_t GetPendingBytes() const { return m_PendingBytes; } // Released, but the GPU may still copy from it (pooled or not)
		uint64_t GetMaxPendingBytes() const { return m_MaxPendingBytes; }
		uint64_t GetCreatedCount() const { return m_CreatedCount; }  // Staging buffers created so far
		uint64_t GetAcquiredBytes() const { return m_AcquiredBytes; } // Bytes of the staging handed out so far (reused or new)

		// Whether `bytes` more staging fit beside what the GPU may still copy from, within `limit` bytes; when they would not,
		// staging whose copies the GPU has run since the latest poll is reclaimed first. Budgeted uploads wait for room
		// (Texture and Mesh::FinalizeOnMainThread, within AssetFinalizeContext::GetStagingRoom: the manager's
		// AssetResidencyBudgets::StagingBytes less one step): staging memory stays bounded however fast data arrives.
		bool HasRoomFor(uint64_t bytes, uint64_t limit);

		static constexpr uint64_t c_MaxIdleBytes = 16ull << 20;
		static constexpr uint32_t c_IdleReleaseFrames = 120;
		// By default, at most this much staging waiting for the GPU is pooled (becomes idle once the GPU is done); what is
		// released beyond it is freed then instead, which bounds what the pool keeps for reuse after a burst of uploads that
		// ignore HasRoomFor (unbudgeted: LoadAssetSync, AddMemoryAsset) or run without device frames (tools, tests).
		static constexpr uint64_t c_MaxPendingBytes = 256ull << 20;
		// Staging buffers come in powers of two from this size on, so that the buffers of uploads of similar sizes are reused.
		static constexpr uint64_t c_MinBufferBytes = 64ull << 10;

		// Bytes of the staging buffer AcquireBuffer hands out for `bytes`.
		static uint64_t GetBufferByteSize(uint64_t bytes);
	private:
		struct Entry
		{
			nvrhi::BufferHandle Buffer;
			uint64_t Bytes = 0;
			bool Pooled = true; // Becomes idle once the GPU has run its copies; else it is freed then
		};
		// Staging released before an event query was set: reusable once the query completes.
		struct Batch
		{
			std::vector<Entry> Entries;
			nvrhi::EventQueryHandle Completion;
		};

		// Releases idle staging, least recently released first, until `bytes` of new staging fit beside what the pool holds
		// within `limit`, or nothing idle is left.
		void MakeRoom(uint64_t bytes, uint64_t limit);
		// Holds released staging until the GPU is done with it, pooled while what waits stays within the pending limit.
		void AddPending(Entry entry, nvrhi::CommandQueue queue);
		// The index of a queue's released and fenced staging: the graphics queue's, or the transfer queue's.
		static size_t GetQueueIndex(nvrhi::CommandQueue queue) { return queue == nvrhi::CommandQueue::Copy ? 1 : 0; }
		// For staging the GPU is done with: pooled entries become idle, the others are freed.
		void MakeIdle(std::vector<Entry>& entries);
		// Makes the staging of the fenced batches whose queries completed idle (polled, never waiting).
		void CollectCompleted();
	private:
		nvrhi::IDevice* m_Device;
		uint64_t m_MaxPendingBytes = c_MaxPendingBytes;
		uint64_t m_CurrentFrame = 0;
		uint64_t m_LastAcquireFrame = 0;
		// Per queue (indexed by GetQueueIndex): staging released since the queue's latest query, not covered by one yet, and the
		// fenced batches, oldest first (a batch never completes before an older one of its queue).
		std::array<std::vector<Entry>, 2> m_Released;
		std::array<std::deque<Batch>, 2> m_Fenced;
		std::vector<nvrhi::EventQueryHandle> m_IdleQueries;
		std::vector<Entry> m_Idle;     // Least recently released first
		uint64_t m_PendingBytes = 0;   // Released and not idle or freed yet (m_Released and m_Fenced)
		uint64_t m_IdleBytes = 0;
		uint64_t m_CreatedCount = 0;
		uint64_t m_AcquiredBytes = 0;
	};

}
