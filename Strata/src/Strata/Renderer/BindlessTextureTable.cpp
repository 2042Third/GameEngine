#include "stpch.h"
#include "Strata/Renderer/BindlessTextureTable.h"

namespace Strata
{

	BindlessTextureTable::BindlessTextureTable(nvrhi::IDevice* device, uint32_t maxCapacity, uint32_t framesInFlight)
		: m_Device(device), m_MaxCapacity(std::max(maxCapacity, c_ReservedSlots + 1)), m_FramesInFlight(framesInFlight)
	{
		nvrhi::BindlessLayoutDesc layoutDesc;
		layoutDesc.visibility = nvrhi::ShaderType::All;
		layoutDesc.maxCapacity = m_MaxCapacity;
		layoutDesc.addRegisterSpace(nvrhi::BindingLayoutItem::Texture_SRV(0));
		m_Layout = m_Device->createBindlessLayout(layoutDesc);
		m_Table = m_Device->createDescriptorTable(m_Layout);

		std::scoped_lock<std::mutex> lock(m_Mutex);
		GrowLocked(std::min<uint32_t>(1024, m_MaxCapacity));
	}

	BindlessTextureTable::~BindlessTextureTable() = default;

	void BindlessTextureTable::GrowLocked(uint32_t minimumCapacity)
	{
		const uint32_t capacity = static_cast<uint32_t>(m_SlotTextures.size());
		uint32_t newCapacity = std::max(capacity, 64u);
		while (newCapacity < minimumCapacity)
			newCapacity *= 2;
		newCapacity = std::min(newCapacity, m_MaxCapacity);
		if (newCapacity <= capacity)
			return;

		// Existing descriptors are preserved; new slots stay unwritten until allocated (partially bound).
		m_Device->resizeDescriptorTable(m_Table, newCapacity, true);
		m_SlotTextures.resize(newCapacity);
		m_SlotAllocated.resize(newCapacity, false);
	}

	uint32_t BindlessTextureTable::Allocate(nvrhi::ITexture* texture)
	{
		if (!texture)
			return c_InvalidSlot;

		std::scoped_lock<std::mutex> lock(m_Mutex);
		uint32_t slot = c_InvalidSlot;
		if (!m_FreeSlots.empty())
		{
			slot = m_FreeSlots.back();
			m_FreeSlots.pop_back();
		}
		else if (m_NextSlot < m_MaxCapacity)
		{
			slot = m_NextSlot++;
			if (slot >= m_SlotTextures.size())
				GrowLocked(slot + 1);
		}

		if (slot == c_InvalidSlot)
		{
			ST_CORE_ERROR("Bindless texture table is full ({} slots)", m_MaxCapacity);
			return c_InvalidSlot;
		}

		m_SlotTextures[slot] = texture;
		m_SlotAllocated[slot] = true;
		m_Device->writeDescriptorTable(m_Table, nvrhi::BindingSetItem::Texture_SRV(slot, texture));
		m_Allocated++;
		return slot;
	}

	void BindlessTextureTable::Release(uint32_t slot)
	{
		if (slot == c_InvalidSlot || slot < c_ReservedSlots)
			return;

		std::scoped_lock<std::mutex> lock(m_Mutex);
		if (slot >= m_SlotAllocated.size() || !m_SlotAllocated[slot])
		{
			ST_CORE_WARN("Bindless texture slot {} released but not allocated", slot);
			return;
		}
		m_SlotAllocated[slot] = false;
		m_PendingReleases.push_back(PendingRelease { slot, m_CurrentFrame });
		m_Allocated--;
	}

	void BindlessTextureTable::SetReservedTexture(uint32_t slot, nvrhi::ITexture* texture)
	{
		ST_CORE_ASSERT(slot < c_ReservedSlots, "Not a reserved bindless slot");
		std::scoped_lock<std::mutex> lock(m_Mutex);
		m_SlotTextures[slot] = texture;
		m_Device->writeDescriptorTable(m_Table, nvrhi::BindingSetItem::Texture_SRV(slot, texture));
	}

	void BindlessTextureTable::BeginFrame(uint64_t frameIndex)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		m_CurrentFrame = frameIndex;
		// A slot released during frame F may be sampled by frames up to F, which have all completed once the device
		// has started frame F + framesInFlight + 1 (BeginFrame waits for frame slots before we get here).
		while (!m_PendingReleases.empty() && m_PendingReleases.front().Frame + m_FramesInFlight + 1 <= frameIndex)
		{
			const uint32_t slot = m_PendingReleases.front().Slot;
			m_PendingReleases.pop_front();
			m_SlotTextures[slot] = nullptr;
			m_FreeSlots.push_back(slot);
		}
	}

	uint32_t BindlessTextureTable::GetAllocatedCount() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return m_Allocated;
	}

	uint32_t BindlessTextureTable::GetCapacity() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return static_cast<uint32_t>(m_SlotTextures.size());
	}

}
