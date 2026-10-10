#include "stpch.h"
#include "Strata/Renderer/StagingTexturePool.h"

namespace Strata
{

	namespace
	{

		bool HasShape(const nvrhi::TextureDesc& a, const nvrhi::TextureDesc& b)
		{
			return a.format == b.format && a.width == b.width && a.height == b.height && a.mipLevels == b.mipLevels;
		}

	}

	StagingTexturePool::StagingTexturePool(nvrhi::IDevice* device, uint32_t framesInFlight)
		: m_Device(device), m_FramesInFlight(framesInFlight)
	{
	}

	uint64_t StagingTexturePool::GetByteSize(const nvrhi::TextureDesc& desc)
	{
		const nvrhi::FormatInfo& format = nvrhi::getFormatInfo(desc.format);
		uint64_t bytes = 0;
		for (uint32_t level = 0; level < desc.mipLevels; level++)
		{
			const uint64_t blocksX = (std::max(1u, desc.width >> level) + format.blockSize - 1) / format.blockSize;
			const uint64_t blocksY = (std::max(1u, desc.height >> level) + format.blockSize - 1) / format.blockSize;
			bytes += blocksX * blocksY * format.bytesPerBlock;
		}
		return bytes;
	}

	nvrhi::StagingTextureHandle StagingTexturePool::Acquire(const nvrhi::TextureDesc& desc)
	{
		m_LastAcquireFrame = m_CurrentFrame;
		// The most recently released one of that shape: its memory is the likeliest to be in the caches.
		for (auto it = m_Idle.rbegin(); it != m_Idle.rend(); ++it)
		{
			if (HasShape(it->Texture->getDesc(), desc))
			{
				nvrhi::StagingTextureHandle staging = std::move(it->Texture);
				m_IdleBytes -= it->Bytes;
				m_Idle.erase(std::next(it).base());
				return staging;
			}
		}

		nvrhi::TextureDesc stagingDesc;
		stagingDesc.width = desc.width;
		stagingDesc.height = desc.height;
		stagingDesc.mipLevels = desc.mipLevels;
		stagingDesc.format = desc.format;
		stagingDesc.debugName = "UploadStaging";
		nvrhi::StagingTextureHandle staging = m_Device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Write);
		if (staging)
			m_CreatedCount++;
		return staging;
	}

	void StagingTexturePool::Release(nvrhi::StagingTextureHandle staging)
	{
		if (!staging)
			return;
		// Without frames (BeginFrame) nothing becomes reusable: beyond the limit, staging textures are left to the command
		// lists that read them, which free them once the GPU is done.
		const uint64_t bytes = GetByteSize(staging->getDesc());
		if (m_PendingBytes + bytes > c_MaxPendingBytes)
			return;
		m_PendingBytes += bytes;
		m_Pending.push_back(Entry { std::move(staging), bytes, m_CurrentFrame });
	}

	void StagingTexturePool::BeginFrame(uint64_t frameIndex)
	{
		m_CurrentFrame = frameIndex;
		// The device began frame `frameIndex`, so frame F has completed once frameIndex >= F + framesInFlight + 1 (as for
		// bindless slots, see BindlessTextureTable::BeginFrame).
		while (!m_Pending.empty() && m_Pending.front().Frame + m_FramesInFlight + 1 <= frameIndex)
		{
			Entry entry = std::move(m_Pending.front());
			m_Pending.pop_front();
			m_PendingBytes -= entry.Bytes;
			m_IdleBytes += entry.Bytes;
			m_Idle.push_back(std::move(entry));
		}

		// Uploads stopped a while ago: return everything. Otherwise keep the most recently released up to the limit.
		const bool quiet = frameIndex >= m_LastAcquireFrame + c_IdleReleaseFrames;
		while (!m_Idle.empty() && (quiet || m_IdleBytes > c_MaxIdleBytes))
		{
			m_IdleBytes -= m_Idle.front().Bytes;
			m_Idle.erase(m_Idle.begin());
		}
	}

}
