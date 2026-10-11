#pragma once

#include "Strata/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace Strata
{

	// CPU-writable staging memory for uploads, reused rather than allocated for every upload: textures upload their pixels
	// through staging textures in bands (Texture::FinalizeOnMainThread), meshes their geometry through staging buffers in
	// ranges (Mesh::FinalizeOnMainThread). Staging released during a frame becomes reusable once the GPU has finished the
	// work submitted before the next frame began: BeginFrame sets an event query for it and later frames poll it, never
	// waiting, so with the GPU keeping up staging is reused two frames after its release. Idle staging is kept up to
	// c_MaxIdleBytes, and all of it is released once nothing was acquired for c_IdleReleaseFrames frames, so staging memory
	// follows the uploads in flight instead of staying at the high-water mark of a burst (and a steady stream of uploads
	// allocates nothing).
	//
	// Budgeted uploads bound staging as a whole: they wait while what is in flight leaves no room within their budget
	// (HasRoomFor), and pass the budget to AcquireTexture and AcquireBuffer, which release idle staging of other shapes
	// before they create staging that would put the pool (idle, in flight and new) over it. The graphics driver keeps the
	// memory of freed staging committed for a while, so the process pays for the high-water mark of staging, not for what
	// it holds now: idle staging must not come on top of the budget. Main thread only; staging must be released after the
	// copies that read it are recorded, and those command lists executed before the next BeginFrame.
	class StagingPool
	{
	public:
		static constexpr uint64_t c_Unlimited = std::numeric_limits<uint64_t>::max();

		explicit StagingPool(nvrhi::IDevice* device);
		// Drops the staging it holds: the owner waits for the GPU first (Renderer::Shutdown does).
		~StagingPool();

		StagingPool(const StagingPool&) = delete;
		StagingPool& operator=(const StagingPool&) = delete;

		// A staging texture with the format, size and mip count of `desc`, free for writing: an idle one of that shape, or a
		// new one, for which idle staging is released while the pool would otherwise hold more than `limit` bytes. Null when
		// one cannot be created.
		nvrhi::StagingTextureHandle AcquireTexture(const nvrhi::TextureDesc& desc, uint64_t limit = c_Unlimited);
		// A CPU-writable buffer of GetBufferByteSize(bytes) bytes to copy from into GPU buffers (ICommandList::copyBuffer),
		// free for writing: an idle one of that size, or a new one, made room for like a new texture. Null when one cannot be
		// created.
		nvrhi::BufferHandle AcquireBuffer(uint64_t bytes, uint64_t limit = c_Unlimited);
		// Hands staging back once the copies that read it are recorded.
		void Release(nvrhi::StagingTextureHandle staging);
		void Release(nvrhi::BufferHandle staging);

		// Once per frame, when the device began frame `frameIndex` (Renderer::BeginFrame): staging whose copies the GPU has
		// finished becomes idle, what was released since the previous frame gets the event query that tells when, and idle
		// staging beyond the limits is released.
		void BeginFrame(uint64_t frameIndex);

		uint64_t GetIdleBytes() const { return m_IdleBytes; }
		uint64_t GetPendingBytes() const { return m_PendingBytes; } // Released, but the GPU may still copy from it
		uint64_t GetCreatedCount() const { return m_CreatedCount; }  // Staging textures and buffers created so far

		// Whether `bytes` more staging fit beside what the GPU may still copy from, within `limit` bytes. Budgeted uploads
		// wait for room (Texture and Mesh::FinalizeOnMainThread, with the manager's AssetResidencyBudgets::StagingBytes):
		// staging memory stays bounded however fast data arrives.
		bool HasRoomFor(uint64_t bytes, uint64_t limit) const { return m_PendingBytes <= limit && bytes <= limit - m_PendingBytes; }

		static constexpr uint64_t c_MaxIdleBytes = 16ull << 20;
		static constexpr uint32_t c_IdleReleaseFrames = 120;
		// At most this much waits for the GPU to become reusable; more is not pooled (it is freed once the GPU is done),
		// which bounds the pool when frames do not advance (tools, tests) or uploads ignore HasRoomFor (unbudgeted).
		static constexpr uint64_t c_MaxPendingBytes = 256ull << 20;
		// Staging buffers come in powers of two from this size on, so that the buffers of uploads of similar sizes are reused.
		static constexpr uint64_t c_MinBufferBytes = 64ull << 10;

		// Bytes of a staging texture's pixels (every mip level).
		static uint64_t GetByteSize(const nvrhi::TextureDesc& desc);
		// Bytes of the staging buffer AcquireBuffer hands out for `bytes`.
		static uint64_t GetBufferByteSize(uint64_t bytes);
	private:
		struct Entry
		{
			// One of the two
			nvrhi::StagingTextureHandle Texture;
			nvrhi::BufferHandle Buffer;
			uint64_t Bytes = 0;
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
		// Holds released staging until the GPU is done with it (or leaves it to the GPU beyond c_MaxPendingBytes).
		void AddPending(Entry entry);
		void MakeIdle(std::vector<Entry>& entries);
	private:
		nvrhi::IDevice* m_Device;
		uint64_t m_CurrentFrame = 0;
		uint64_t m_LastAcquireFrame = 0;
		std::vector<Entry> m_Released; // Since the latest BeginFrame, not covered by a query yet
		std::deque<Batch> m_Fenced;    // Oldest first: a batch never completes before an older one
		std::vector<nvrhi::EventQueryHandle> m_IdleQueries;
		std::vector<Entry> m_Idle;     // Least recently released first
		uint64_t m_PendingBytes = 0;   // Released and fenced
		uint64_t m_IdleBytes = 0;
		uint64_t m_CreatedCount = 0;
	};

}
