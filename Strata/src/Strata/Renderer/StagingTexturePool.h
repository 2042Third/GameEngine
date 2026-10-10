#pragma once

#include "Strata/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <deque>
#include <vector>

namespace Strata
{

	// CPU-writable staging textures for uploads, reused rather than allocated for every upload: textures upload their pixels
	// through them in bands (Texture::FinalizeOnMainThread). A staging texture the copies of a frame read becomes reusable
	// once that frame cannot be in flight any more. Idle ones are kept up to c_MaxIdleBytes, and all of them are released
	// once nothing was acquired for c_IdleReleaseFrames frames, so staging memory follows the uploads in flight instead of
	// staying at the high-water mark of a burst (and a steady stream of uploads allocates nothing). Main thread only.
	class StagingTexturePool
	{
	public:
		StagingTexturePool(nvrhi::IDevice* device, uint32_t framesInFlight);

		StagingTexturePool(const StagingTexturePool&) = delete;
		StagingTexturePool& operator=(const StagingTexturePool&) = delete;

		// A staging texture with the format, size and mip count of `desc`, free for writing: an idle one of that shape, or a
		// new one. Null when one cannot be created.
		nvrhi::StagingTextureHandle Acquire(const nvrhi::TextureDesc& desc);
		// Hands a staging texture back once the copies that read it are recorded for the current frame.
		void Release(nvrhi::StagingTextureHandle staging);

		// Once per frame, when the device began frame `frameIndex` (Renderer::BeginFrame): staging textures released
		// frames in flight + 1 frames ago become idle, and idle ones beyond the limits are released.
		void BeginFrame(uint64_t frameIndex);

		uint64_t GetIdleBytes() const { return m_IdleBytes; }
		uint64_t GetPendingBytes() const { return m_PendingBytes; } // Released, but frames in flight may still read them
		uint64_t GetCreatedCount() const { return m_CreatedCount; }  // Staging textures created so far

		// Whether `bytes` more staging fit beside what frames in flight may still read, within `limit` bytes. Budgeted uploads
		// wait for room (Texture::FinalizeOnMainThread, with the manager's AssetResidencyBudgets::StagingBytes): staging
		// memory stays bounded however fast data arrives.
		bool HasRoomFor(uint64_t bytes, uint64_t limit) const { return m_PendingBytes <= limit && bytes <= limit - m_PendingBytes; }

		static constexpr uint64_t c_MaxIdleBytes = 16ull << 20;
		static constexpr uint32_t c_IdleReleaseFrames = 120;
		// At most this much waits for frames in flight to become reusable; more is not pooled (it is freed once the GPU is
		// done), which bounds the pool when frames do not advance (tools, tests) or uploads ignore HasRoomFor (unbudgeted).
		static constexpr uint64_t c_MaxPendingBytes = 256ull << 20;

		// Bytes of a staging texture's pixels (every mip level).
		static uint64_t GetByteSize(const nvrhi::TextureDesc& desc);
	private:
		struct Entry
		{
			nvrhi::StagingTextureHandle Texture;
			uint64_t Bytes = 0;
			uint64_t Frame = 0; // Pending: the frame that released it
		};
	private:
		nvrhi::IDevice* m_Device;
		uint32_t m_FramesInFlight;
		uint64_t m_CurrentFrame = 0;
		uint64_t m_LastAcquireFrame = 0;
		std::deque<Entry> m_Pending;  // Oldest first
		std::vector<Entry> m_Idle;    // Least recently released first
		uint64_t m_PendingBytes = 0;
		uint64_t m_IdleBytes = 0;
		uint64_t m_CreatedCount = 0;
	};

}
