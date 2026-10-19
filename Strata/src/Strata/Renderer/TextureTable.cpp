#include "stpch.h"
#include "Strata/Renderer/TextureTable.h"

#include "Strata/Renderer/BindlessTextureTable.h"
#include "Strata/Renderer/Renderer.h"
#include "Strata/Renderer/SceneRenderData.h"

#include <algorithm>
#include <chrono>

namespace Strata
{

	static_assert(TextureTable::c_White == BindlessTextureTable::c_WhiteSlot && TextureTable::c_Black == BindlessTextureTable::c_BlackSlot
		&& TextureTable::c_FlatNormal == BindlessTextureTable::c_FlatNormalSlot && TextureTable::c_ReservedIndices == BindlessTextureTable::c_ReservedSlots);
	static_assert(sizeof(TextureTable::GPUEntry) == 8, "TextureTable::GPUEntry must match u_TextureTable's uvec2 (Include/Scene.glsl)");

	TextureTable::TextureTable(nvrhi::IDevice* device, uint32_t framesInFlight)
		: m_Device(device), m_FramesInFlight(framesInFlight)
	{
		// The reserved indices name the reserved bindless slots (unfiltered: the fallbacks are single texels).
		for (uint32_t index = 0; index < c_ReservedIndices; index++)
		{
			m_Entries.push_back(PackEntry(index, 0, false));
			m_MinLods.push_back(0.0f);
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
			m_MinLods.push_back(0.0f);
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
		if (index < c_ReservedIndices || index >= m_Entries.size() || !m_Allocated[index])
			return;
		EndFadeLocked(index);
		if (m_Entries[index] == entry)
			return;
		m_Entries[index] = entry;
		MarkDirtyLocked(index);
	}

	void TextureTable::SetFadeIn(uint32_t index, uint32_t entry, uint32_t gainedLevels)
	{
		const double now = GetClockSeconds();
		std::scoped_lock lock(m_Mutex);
		if (index < c_ReservedIndices || index >= m_Entries.size() || !m_Allocated[index])
			return;
		EndFadeLocked(index);
		m_Entries[index] = entry;
		MarkDirtyLocked(index);
		if (gainedLevels == 0 || m_FadeInSeconds <= 0.0f)
			return;
		const float fromLod = static_cast<float>(gainedLevels);
		m_MinLods[index] = fromLod;
		m_Fades.push_back(Fade { index, fromLod, now });
	}

	void TextureTable::EndFadeLocked(uint32_t index)
	{
		const auto fade = std::find_if(m_Fades.begin(), m_Fades.end(), [index](const Fade& candidate) { return candidate.Index == index; });
		if (fade == m_Fades.end())
			return;
		m_Fades.erase(fade);
		m_MinLods[index] = 0.0f;
		MarkDirtyLocked(index);
	}

	void TextureTable::SetFadeInSeconds(float seconds)
	{
		std::scoped_lock lock(m_Mutex);
		m_FadeInSeconds = std::max(seconds, 0.0f);
	}

	float TextureTable::GetFadeInSeconds() const
	{
		std::scoped_lock lock(m_Mutex);
		return m_FadeInSeconds;
	}

	float TextureTable::GetMinLod(uint32_t index) const
	{
		std::scoped_lock lock(m_Mutex);
		return index < m_MinLods.size() ? m_MinLods[index] : 0.0f;
	}

	uint32_t TextureTable::GetFadeCount() const
	{
		std::scoped_lock lock(m_Mutex);
		return static_cast<uint32_t>(m_Fades.size());
	}

	void TextureTable::UpdateFades(double seconds)
	{
		std::scoped_lock lock(m_Mutex);
		for (auto fade = m_Fades.begin(); fade != m_Fades.end();)
		{
			// Linear in levels of detail: each level takes the same share of the fade.
			const double progress = m_FadeInSeconds > 0.0f ? (seconds - fade->StartSeconds) / static_cast<double>(m_FadeInSeconds) : 1.0;
			const float minLod = progress >= 1.0 ? 0.0f : fade->FromLod * static_cast<float>(1.0 - std::max(progress, 0.0));
			if (m_MinLods[fade->Index] != minLod)
			{
				m_MinLods[fade->Index] = minLod;
				MarkDirtyLocked(fade->Index);
			}
			if (progress >= 1.0)
				fade = m_Fades.erase(fade);
			else
				++fade;
		}
	}

	double TextureTable::GetClockSeconds()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	void TextureTable::Free(uint32_t index)
	{
		std::scoped_lock lock(m_Mutex);
		if (index < c_ReservedIndices || index >= m_Entries.size() || !m_Allocated[index])
			return;
		m_Allocated[index] = 0;
		m_AllocatedCount--;
		EndFadeLocked(index);
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
		UpdateFades(GetClockSeconds());
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
				desc.byteSize = capacity * sizeof(GPUEntry);
				desc.structStride = sizeof(GPUEntry);
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
				m_UploadScratch.resize(m_Entries.size());
				for (size_t index = 0; index < m_Entries.size(); index++)
					m_UploadScratch[index] = GPUEntry { m_Entries[index], m_MinLods[index] };
				m_DirtyScratch.clear();
			}
			else
			{
				m_DirtyScratch.assign(m_DirtyList.begin(), m_DirtyList.end());
				std::sort(m_DirtyScratch.begin(), m_DirtyScratch.end());
				m_UploadScratch.clear();
				for (const uint32_t index : m_DirtyScratch)
					m_UploadScratch.push_back(GPUEntry { m_Entries[index], m_MinLods[index] });
			}
			for (const uint32_t index : m_DirtyList)
				m_Dirty[index] = 0;
			m_DirtyList.clear();
		}

		if (whole)
		{
			commandList->writeBuffer(m_Buffer, m_UploadScratch.data(), m_UploadScratch.size() * sizeof(GPUEntry));
			bytes = m_UploadScratch.size() * sizeof(GPUEntry);
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
				commandList->writeBuffer(m_Buffer, m_UploadScratch.data() + start, count * sizeof(GPUEntry), first * sizeof(GPUEntry));
				bytes += count * sizeof(GPUEntry);
				start = position;
			}
		}
		if (outBytes)
			*outBytes = bytes;
		return true;
	}

}
