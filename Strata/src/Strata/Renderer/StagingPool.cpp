#include "stpch.h"
#include "Strata/Renderer/StagingPool.h"

#include <bit>

namespace Strata
{

	namespace
	{

		bool HasShape(const nvrhi::TextureDesc& a, const nvrhi::TextureDesc& b)
		{
			return a.format == b.format && a.width == b.width && a.height == b.height && a.mipLevels == b.mipLevels;
		}

	}

	StagingPool::StagingPool(nvrhi::IDevice* device)
		: m_Device(device)
	{
	}

	StagingPool::~StagingPool() = default;

	uint64_t StagingPool::GetByteSize(const nvrhi::TextureDesc& desc)
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

	uint64_t StagingPool::GetBufferByteSize(uint64_t bytes)
	{
		// Beyond the largest power of two a 64-bit size holds, the size itself (no device allocates that much anyway).
		if (bytes > (1ull << 63))
			return bytes;
		return std::max(c_MinBufferBytes, std::bit_ceil(bytes));
	}

	nvrhi::StagingTextureHandle StagingPool::AcquireTexture(const nvrhi::TextureDesc& desc, uint64_t limit)
	{
		m_LastAcquireFrame = m_CurrentFrame;
		// What the GPU has finished copying from since the latest poll is reused rather than made anew.
		CollectCompleted();
		// The most recently released one of that shape: its memory is the likeliest to be in the caches.
		for (auto it = m_Idle.rbegin(); it != m_Idle.rend(); ++it)
		{
			if (it->Texture && HasShape(it->Texture->getDesc(), desc))
			{
				nvrhi::StagingTextureHandle staging = std::move(it->Texture);
				m_IdleBytes -= it->Bytes;
				m_AcquiredBytes += it->Bytes;
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
		const uint64_t bytes = GetByteSize(stagingDesc);
		MakeRoom(bytes, limit);
		nvrhi::StagingTextureHandle staging = m_Device->createStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Write);
		if (staging)
		{
			m_CreatedCount++;
			m_AcquiredBytes += bytes;
		}
		return staging;
	}

	nvrhi::BufferHandle StagingPool::AcquireBuffer(uint64_t bytes, uint64_t limit)
	{
		m_LastAcquireFrame = m_CurrentFrame;
		CollectCompleted();
		const uint64_t size = GetBufferByteSize(bytes);
		for (auto it = m_Idle.rbegin(); it != m_Idle.rend(); ++it)
		{
			if (it->Buffer && it->Bytes == size)
			{
				nvrhi::BufferHandle staging = std::move(it->Buffer);
				m_IdleBytes -= it->Bytes;
				m_AcquiredBytes += it->Bytes;
				m_Idle.erase(std::next(it).base());
				return staging;
			}
		}

		nvrhi::BufferDesc desc;
		desc.byteSize = size;
		desc.cpuAccess = nvrhi::CpuAccessMode::Write;
		desc.debugName = "UploadStagingBuffer";
		// Only ever copied from; the copies that read it are recorded after the CPU wrote it.
		desc.initialState = nvrhi::ResourceStates::CopySource;
		desc.keepInitialState = true;
		MakeRoom(size, limit);
		nvrhi::BufferHandle staging = m_Device->createBuffer(desc);
		if (staging)
		{
			m_CreatedCount++;
			m_AcquiredBytes += size;
		}
		return staging;
	}

	void StagingPool::MakeRoom(uint64_t bytes, uint64_t limit)
	{
		// Written so that nothing overflows, whatever the limit.
		const auto fits = [&]()
		{
			const uint64_t held = m_IdleBytes + m_PendingBytes;
			return held <= limit && bytes <= limit - held;
		};
		while (!m_Idle.empty() && !fits())
		{
			m_IdleBytes -= m_Idle.front().Bytes;
			m_Idle.erase(m_Idle.begin());
		}
	}

	void StagingPool::Release(nvrhi::StagingTextureHandle staging)
	{
		if (!staging)
			return;
		const uint64_t bytes = GetByteSize(staging->getDesc());
		AddPending(Entry { std::move(staging), nullptr, bytes });
	}

	void StagingPool::Release(nvrhi::BufferHandle staging)
	{
		if (!staging)
			return;
		const uint64_t bytes = staging->getDesc().byteSize;
		AddPending(Entry { nullptr, std::move(staging), bytes });
	}

	void StagingPool::AddPending(Entry entry)
	{
		// Without frames (BeginFrame) nothing becomes reusable: beyond the limit, staging is left to the command lists that
		// read it, which free it once the GPU is done.
		if (m_PendingBytes + entry.Bytes > c_MaxPendingBytes)
			return;
		m_PendingBytes += entry.Bytes;
		m_Released.push_back(std::move(entry));
	}

	void StagingPool::MakeIdle(std::vector<Entry>& entries)
	{
		for (Entry& entry : entries)
		{
			m_PendingBytes -= entry.Bytes;
			m_IdleBytes += entry.Bytes;
			m_Idle.push_back(std::move(entry));
		}
		entries.clear();
	}

	bool StagingPool::HasRoomFor(uint64_t bytes, uint64_t limit)
	{
		const auto fits = [&]()
		{
			return m_PendingBytes <= limit && bytes <= limit - m_PendingBytes;
		};
		if (fits())
			return true;
		// The GPU may have run copies since the latest poll: what it is done with no longer takes room.
		CollectCompleted();
		return fits();
	}

	void StagingPool::CollectCompleted()
	{
		// Queries complete in submission order, so the first fenced batch decides whether any later one can be done.
		while (!m_Fenced.empty() && m_Device->pollEventQuery(m_Fenced.front().Completion))
		{
			Batch batch = std::move(m_Fenced.front());
			m_Fenced.pop_front();
			MakeIdle(batch.Entries);
			m_IdleQueries.push_back(std::move(batch.Completion));
		}
	}

	void StagingPool::FenceReleased()
	{
		if (m_Released.empty())
			return;

		// The command lists that copy from what was released were executed: a query set now completes after them.
		nvrhi::EventQueryHandle completion;
		if (!m_IdleQueries.empty())
		{
			completion = std::move(m_IdleQueries.back());
			m_IdleQueries.pop_back();
		}
		else
		{
			completion = m_Device->createEventQuery();
		}
		if (completion)
		{
			m_Device->resetEventQuery(completion);
			m_Device->setEventQuery(completion, nvrhi::CommandQueue::Graphics);
			m_Fenced.push_back(Batch { std::move(m_Released), std::move(completion) });
			m_Released.clear();
			return;
		}

		// Completion cannot be observed without a query: wait for the GPU instead.
		ST_CORE_ERROR("StagingPool: failed to create an event query; waiting for the GPU to reuse staging");
		m_Device->waitForIdle();
		for (Batch& batch : m_Fenced)
			MakeIdle(batch.Entries);
		m_Fenced.clear();
		MakeIdle(m_Released);
	}

	uint64_t StagingPool::ReleaseIdle()
	{
		CollectCompleted();
		const uint64_t released = m_IdleBytes;
		m_Idle.clear();
		m_IdleBytes = 0;
		return released;
	}

	void StagingPool::BeginFrame(uint64_t frameIndex)
	{
		m_CurrentFrame = frameIndex;
		CollectCompleted();
		// The copies of what was released since the latest query were executed by now (staging is released once its copies
		// are recorded, and their command lists are executed within the frame).
		FenceReleased();

		// Uploads stopped a while ago: return everything. Otherwise keep the most recently released up to the limit.
		const bool quiet = frameIndex >= m_LastAcquireFrame + c_IdleReleaseFrames;
		while (!m_Idle.empty() && (quiet || m_IdleBytes > c_MaxIdleBytes))
		{
			m_IdleBytes -= m_Idle.front().Bytes;
			m_Idle.erase(m_Idle.begin());
		}
	}

}
