#pragma once

#include "Strata/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <deque>
#include <mutex>
#include <vector>

namespace Strata
{

	// Every 2D texture the renderer samples through materials lives in one bindless descriptor array; shaders index
	// it with the slot stored in material data, so materials never rebind textures.
	//
	// Lifetime rules: a slot keeps a strong reference to its texture until the slot is recycled, which happens only
	// once every frame that might still sample it has completed. Descriptors are written only when a slot is
	// (re)allocated, never while frames in flight can access them (required by update-after-bind descriptors).
	// To replace a texture (e.g. after streaming in more mips), allocate a new slot and release the old one.
	//
	// GLSL: layout(set = S, binding = 0) uniform texture2D u_Textures[]; indexed with nonuniformEXT(slot).
	class BindlessTextureTable
	{
	public:
		// Fixed slots holding fallback textures, written once at startup.
		static constexpr uint32_t c_WhiteSlot = 0;
		static constexpr uint32_t c_BlackSlot = 1;
		static constexpr uint32_t c_FlatNormalSlot = 2;
		static constexpr uint32_t c_ReservedSlots = 3;
		static constexpr uint32_t c_InvalidSlot = UINT32_MAX;

		BindlessTextureTable(nvrhi::IDevice* device, uint32_t maxCapacity, uint32_t framesInFlight);
		~BindlessTextureTable();

		BindlessTextureTable(const BindlessTextureTable&) = delete;
		BindlessTextureTable& operator=(const BindlessTextureTable&) = delete;

		// Thread-safe. Returns c_InvalidSlot when the table is full.
		uint32_t Allocate(nvrhi::ITexture* texture);
		// Thread-safe. The texture stays alive (and the slot unused) until frames in flight have completed. Releasing a
		// slot that is not allocated is ignored.
		void Release(uint32_t slot);
		// Startup only (before any frame samples the table).
		void SetReservedTexture(uint32_t slot, nvrhi::ITexture* texture);

		// Main thread, once per frame: recycles slots whose last possible use has completed on the GPU.
		void BeginFrame(uint64_t frameIndex);

		nvrhi::IBindingLayout* GetLayout() const { return m_Layout; }
		nvrhi::IDescriptorTable* GetTable() const { return m_Table; }
		uint32_t GetAllocatedCount() const;
		uint32_t GetCapacity() const;
	private:
		void GrowLocked(uint32_t minimumCapacity);
	private:
		nvrhi::IDevice* m_Device;
		nvrhi::BindingLayoutHandle m_Layout;
		nvrhi::DescriptorTableHandle m_Table;
		uint32_t m_MaxCapacity;
		uint32_t m_FramesInFlight;

		mutable std::mutex m_Mutex;
		std::vector<nvrhi::TextureHandle> m_SlotTextures; // Size = current table capacity
		std::vector<bool> m_SlotAllocated;                // False once released, even while the release is pending
		std::vector<uint32_t> m_FreeSlots;
		uint32_t m_NextSlot = c_ReservedSlots;
		struct PendingRelease
		{
			uint32_t Slot;
			uint64_t Frame;
		};
		std::deque<PendingRelease> m_PendingReleases;
		uint64_t m_CurrentFrame = 0;
		uint32_t m_Allocated = 0;
	};

}
