#include "stpch.h"
#include "Strata/Renderer/TextureTable.h"

#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/SceneRenderData.h"

#include <algorithm>

namespace Strata
{

	static_assert(TextureTable::c_White == BindlessTextureTable::c_WhiteSlot && TextureTable::c_Black == BindlessTextureTable::c_BlackSlot
		&& TextureTable::c_FlatNormal == BindlessTextureTable::c_FlatNormalSlot && TextureTable::c_ReservedIndices == BindlessTextureTable::c_ReservedSlots);

	TextureTable::TextureTable(nvrhi::IDevice* device, uint32_t framesInFlight)
		: m_Device(device), m_FramesInFlight(framesInFlight)
	{
		// The reserved indices name the reserved bindless slots (unfiltered: the fallbacks are single texels).
		for (uint32_t index = 0; index < c_ReservedIndices; index++)
		{
			m_Entries.push_back(PackEntry(index, 0, false));
			m_Allocated.push_back(1);
			m_Dirty.push_back(1);
			m_DirtyList.push_back(index);
		}
	}

	uint32_t TextureTable::PackEntry(uint32_t bindlessSlot, uint32_t samplerIndex, bool grayscale)
	{
		return (bindlessSlot & 0xFFFFFFu) | ((samplerIndex & 0x7Fu) << 24) | (grayscale ? RenderData::c_TextureSlotGrayscale : 0u);
	}

	void TextureTable::MarkDirtyLocked(uint32_t index)
	{
		if (m_Dirty[index])
			return;
		m_Dirty[index] = 1;
		m_DirtyList.push_back(index);
	}

	uint32_t TextureTable::Allocate(uint32_t entry)
	{
		std::scoped_lock lock(m_Mutex);
		uint32_t index = 0;
		if (!m_FreeIndices.empty())
		{
			index = m_FreeIndices.back();
			m_FreeIndices.pop_back();
		}
		else
		{
			index = static_cast<uint32_t>(m_Entries.size());
			m_Entries.push_back(0);
			m_Allocated.push_back(0);
			m_Dirty.push_back(0);
		}
		m_Entries[index] = entry;
		m_Allocated[index] = 1;
		m_AllocatedCount++;
		MarkDirtyLocked(index);
		return index;
	}

	void TextureTable::Set(uint32_t index, uint32_t entry)
	{
		std::scoped_lock lock(m_Mutex);
		if (index < c_ReservedIndices || index >= m_Entries.size() || !m_Allocated[index] || m_Entries[index] == entry)
			return;
		m_Entries[index] = entry;
		MarkDirtyLocked(index);
	}

	void TextureTable::Free(uint32_t index)
	{
		std::scoped_lock lock(m_Mutex);
		if (index < c_ReservedIndices || index >= m_Entries.size() || !m_Allocated[index])
			return;
		m_Allocated[index] = 0;
		m_AllocatedCount--;
		// Material data may still name it until its materials are resolved again: white meanwhile, never another texture.
		m_Entries[index] = PackEntry(BindlessTextureTable::c_WhiteSlot, 0, false);
		MarkDirtyLocked(index);
		m_PendingFrees.push_back(PendingFree { index, m_CurrentFrame });
	}

	uint32_t TextureTable::GetEntry(uint32_t index) const
	{
		std::scoped_lock lock(m_Mutex);
		return index < m_Entries.size() ? m_Entries[index] : PackEntry(BindlessTextureTable::c_WhiteSlot, 0, false);
	}

	uint32_t TextureTable::GetAllocatedCount() const
	{
		std::scoped_lock lock(m_Mutex);
		return m_AllocatedCount;
	}

	void TextureTable::BeginFrame(uint64_t frameIndex)
	{
		std::scoped_lock lock(m_Mutex);
		m_CurrentFrame = frameIndex;
		// As BindlessTextureTable: an index freed during frame F may be read by frames up to F, all completed once the device
		// started frame F + framesInFlight + 1.
		while (!m_PendingFrees.empty() && m_PendingFrees.front().Frame + m_FramesInFlight + 1 <= frameIndex)
		{
			m_FreeIndices.push_back(m_PendingFrees.front().Index);
			m_PendingFrees.pop_front();
		}
	}

	bool TextureTable::RecordUploads(nvrhi::ICommandList* commandList, uint64_t* outBytes)
	{
		uint64_t bytes = 0;
		bool whole = false;
		{
			std::scoped_lock lock(m_Mutex);
			if (m_Entries.size() > m_BufferCapacity || !m_Buffer)
			{
				// Geometric growth; the new buffer is written whole.
				uint64_t capacity = std::max<uint64_t>(m_BufferCapacity, 256);
				while (capacity < m_Entries.size())
					capacity *= 2;
				nvrhi::BufferDesc desc;
				desc.byteSize = capacity * sizeof(uint32_t);
				desc.structStride = sizeof(uint32_t);
				desc.debugName = "TextureTable";
				desc.initialState = nvrhi::ResourceStates::ShaderResource;
				desc.keepInitialState = true;
				nvrhi::BufferHandle buffer = m_Device->createBuffer(desc);
				if (!buffer)
				{
					ST_CORE_ERROR("TextureTable: failed to create its buffer ({} entries)", capacity);
					return false;
				}
				// Frames in flight may still read the replaced buffer.
				if (m_Buffer)
					Renderer::ReleaseDeferred({ nvrhi::ResourceHandle(m_Buffer.Get()) });
				m_Buffer = std::move(buffer);
				m_BufferCapacity = capacity;
				m_BufferVersion++;
				whole = true;
			}
			if (whole)
			{
				m_UploadScratch.assign(m_Entries.begin(), m_Entries.end());
				m_DirtyScratch.clear();
			}
			else
			{
				m_DirtyScratch.assign(m_DirtyList.begin(), m_DirtyList.end());
				std::sort(m_DirtyScratch.begin(), m_DirtyScratch.end());
				m_UploadScratch.clear();
				for (const uint32_t index : m_DirtyScratch)
					m_UploadScratch.push_back(m_Entries[index]);
			}
			for (const uint32_t index : m_DirtyList)
				m_Dirty[index] = 0;
			m_DirtyList.clear();
		}

		if (whole)
		{
			commandList->writeBuffer(m_Buffer, m_UploadScratch.data(), m_UploadScratch.size() * sizeof(uint32_t));
			bytes = m_UploadScratch.size() * sizeof(uint32_t);
		}
		else
		{
			// Runs of consecutive entries, one write each.
			size_t start = 0;
			for (size_t position = 1; position <= m_DirtyScratch.size(); position++)
			{
				if (position < m_DirtyScratch.size() && m_DirtyScratch[position] == m_DirtyScratch[position - 1] + 1)
					continue;
				const uint64_t first = m_DirtyScratch[start];
				const uint64_t count = position - start;
				commandList->writeBuffer(m_Buffer, m_UploadScratch.data() + start, count * sizeof(uint32_t), first * sizeof(uint32_t));
				bytes += count * sizeof(uint32_t);
				start = position;
			}
		}
		if (outBytes)
			*outBytes = bytes;
		return true;
	}

}
