#include "stpch.h"
#include "Strata/Renderer/StagingPool.h"

#include <bit>

namespace Strata
{

	StagingPool::StagingPool(nvrhi::IDevice* device)
		: m_Device(device)
	{
	}

	StagingPool::~StagingPool() = default;

	uint64_t StagingPool::GetBufferByteSize(uint64_t bytes)
	{
		// Beyond the largest power of two a 64-bit size holds, the size itself (no device allocates that much anyway).
		if (bytes > (1ull << 63))
			return bytes;
		return std::max(c_MinBufferBytes, std::bit_ceil(bytes));
	}

	nvrhi::BufferHandle StagingPool::AcquireBuffer(uint64_t bytes, uint64_t limit)
	{
		m_LastAcquireFrame = m_CurrentFrame;
		// What the GPU has finished copying from since the latest poll is reused rather than made anew.
		CollectCompleted();
		const uint64_t size = GetBufferByteSize(bytes);
		// The most recently released one of that size: its memory is the likeliest to be in the caches. Idle staging of
		// unbudgeted uploads may have put the pool over this limit: what else is idle goes until it is within it again.
		for (auto it = m_Idle.rbegin(); it != m_Idle.rend(); ++it)
		{
			if (it->Bytes == size)
			{
				nvrhi::BufferHandle staging = std::move(it->Buffer);
				m_IdleBytes -= it->Bytes;
				m_AcquiredBytes += it->Bytes;
				m_Idle.erase(std::next(it).base());
				MakeRoom(size, limit);
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

	void StagingPool::Release(nvrhi::BufferHandle staging, nvrhi::CommandQueue queue)
	{
		if (!staging)
			return;
		const uint64_t bytes = staging->getDesc().byteSize;
		AddPending(Entry { std::move(staging), bytes }, queue);
	}

	void StagingPool::AddPending(Entry entry, nvrhi::CommandQueue queue)
	{
		// Without frames (BeginFrame) nothing becomes reusable: beyond the limit, staging is left to the command lists that
		// read it, which free it once the GPU is done.
		if (m_PendingBytes + entry.Bytes > c_MaxPendingBytes)
			return;
		m_PendingBytes += entry.Bytes;
		m_Released[GetQueueIndex(queue)].push_back(std::move(entry));
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
		// A queue's queries complete in its submission order, so its first fenced batch decides whether any later one can be
		// done; the queues run independently.
		for (std::deque<Batch>& fenced : m_Fenced)
		{
			while (!fenced.empty() && m_Device->pollEventQuery(fenced.front().Completion))
			{
				Batch batch = std::move(fenced.front());
				fenced.pop_front();
				MakeIdle(batch.Entries);
				m_IdleQueries.push_back(std::move(batch.Completion));
			}
		}
	}

	void StagingPool::FenceReleased(nvrhi::CommandQueue queue)
	{
		std::vector<Entry>& released = m_Released[GetQueueIndex(queue)];
		if (released.empty())
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
			m_Device->setEventQuery(completion, queue);
			m_Fenced[GetQueueIndex(queue)].push_back(Batch { std::move(released), std::move(completion) });
			released.clear();
			return;
		}

		// Completion cannot be observed without a query: wait for the GPU instead.
		ST_CORE_ERROR("StagingPool: failed to create an event query; waiting for the GPU to reuse staging");
		m_Device->waitForIdle();
		for (std::deque<Batch>& fenced : m_Fenced)
		{
			for (Batch& batch : fenced)
				MakeIdle(batch.Entries);
			fenced.clear();
		}
		for (std::vector<Entry>& entries : m_Released)
			MakeIdle(entries);
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
		// are recorded, and their command lists are executed within the frame). The transfer queue's staging is fenced by the
		// upload queue whenever it submits.
		FenceReleased(nvrhi::CommandQueue::Graphics);

		// Uploads stopped a while ago: return everything. Otherwise keep the most recently released up to the limit.
		const bool quiet = frameIndex >= m_LastAcquireFrame + c_IdleReleaseFrames;
		while (!m_Idle.empty() && (quiet || m_IdleBytes > c_MaxIdleBytes))
		{
			m_IdleBytes -= m_Idle.front().Bytes;
			m_Idle.erase(m_Idle.begin());
		}
	}

}
