#include "stpch.h"
#include "Strata/Asset/AssetStreamingQueue.h"

#include <cmath>

namespace Strata
{

	AssetStreamingQueue::AssetStreamingQueue(DispatchFunction dispatch)
		: m_Dispatch(std::move(dispatch))
	{
	}

	void AssetStreamingQueue::InsertLocked(const AssetStreamingRequest& request)
	{
		const Key key = MakeKey(request.Priority, request.Score, request.Order);
		m_Waiting.emplace(key, request);
		m_WaitingKeys.insert_or_assign(request.Handle, key);
		m_WaitingPerPriority[static_cast<size_t>(request.Priority)]++;
	}

	void AssetStreamingQueue::EraseLocked(std::map<Key, AssetStreamingRequest>::iterator node)
	{
		m_WaitingPerPriority[static_cast<size_t>(node->second.Priority)]--;
		m_WaitingKeys.erase(node->second.Handle);
		m_Waiting.erase(node);
	}

	void AssetStreamingQueue::Enqueue(AssetHandle handle, AssetPriority priority, float score, uint64_t storedSize, uint64_t generation)
	{
		AssetStreamingRequest request { handle, priority, std::isfinite(score) ? score : 0.0f, 0, storedSize > 0 ? storedSize : c_UnknownSizeBytes, generation };

		std::scoped_lock<std::mutex> lock(m_Mutex);
		if (m_Closed)
			return;
		auto waiting = m_WaitingKeys.find(handle);
		if (waiting != m_WaitingKeys.end())
		{
			auto node = m_Waiting.find(waiting->second);
			request.Order = node->second.Order;
			EraseLocked(node);
		}
		else
		{
			request.Order = m_NextOrder++;
		}
		InsertLocked(request);
	}

	bool AssetStreamingQueue::Raise(AssetHandle handle, AssetPriority priority, float score)
	{
		const float sanitizedScore = std::isfinite(score) ? score : 0.0f;

		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto waiting = m_WaitingKeys.find(handle);
		if (waiting == m_WaitingKeys.end())
			return false;

		auto node = m_Waiting.find(waiting->second);
		AssetStreamingRequest request = node->second;
		const bool higherPriority = priority < request.Priority;
		const bool higherScore = priority == request.Priority && sanitizedScore > request.Score;
		if (!higherPriority && !higherScore)
			return false;

		// The request keeps its place in the request order: it was made when it was first queued.
		request.Priority = priority;
		request.Score = sanitizedScore;
		EraseLocked(node);
		InsertLocked(request);
		return true;
	}

	bool AssetStreamingQueue::Cancel(AssetHandle handle)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto waiting = m_WaitingKeys.find(handle);
		if (waiting == m_WaitingKeys.end())
			return false;
		EraseLocked(m_Waiting.find(waiting->second));
		return true;
	}

	bool AssetStreamingQueue::IsQueued(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return m_WaitingKeys.find(handle) != m_WaitingKeys.end();
	}

	bool AssetStreamingQueue::CanDispatchLocked() const
	{
		if (m_Closed || m_Waiting.empty())
			return false;
		// With nothing in flight one load always starts, however large: an asset above the limit must still load.
		if (m_InFlightLoads == 0)
			return true;
		return m_InFlightBytes < m_Limits.MaxInFlightBytes && m_OutstandingReads < std::max(1u, m_Limits.MaxOutstandingReads);
	}

	void AssetStreamingQueue::Pump()
	{
		std::unique_lock<std::mutex> lock(m_Mutex);
		// Another pump is running (maybe further up this thread's stack, inside the dispatch function): it checks the
		// limits again under the lock after each dispatch, so it sees whatever made this pump necessary.
		if (m_Pumping)
			return;
		m_Pumping = true;
		while (CanDispatchLocked())
		{
			const AssetStreamingRequest request = m_Waiting.begin()->second;
			EraseLocked(m_Waiting.begin());
			m_InFlightLoads++;
			m_OutstandingReads++;
			m_InFlightBytes += request.Bytes;
			m_InFlightBytesHighWater = std::max(m_InFlightBytesHighWater, m_InFlightBytes);
			m_Dispatched++;

			lock.unlock();
			// The dispatch function reports its own failures by returning false; an exception (allocation failure while
			// starting a job) must not leave the queue pumping forever.
			bool dispatched = false;
			try
			{
				dispatched = m_Dispatch(request);
			}
			catch (const std::exception& exception)
			{
				ST_CORE_ERROR("Starting the load of asset {} failed: {}", request.Handle.ToString(), exception.what());
			}
			catch (...)
			{
				ST_CORE_ERROR("Starting the load of asset {} failed with an unknown exception", request.Handle.ToString());
			}
			lock.lock();

			if (!dispatched)
			{
				m_InFlightLoads--;
				m_OutstandingReads--;
				m_InFlightBytes -= request.Bytes;
				m_Dispatched--;
			}
		}
		m_Pumping = false;
	}

	void AssetStreamingQueue::OnReadFinished()
	{
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			ST_CORE_ASSERT(m_OutstandingReads > 0, "AssetStreamingQueue: a read finished that was never dispatched");
			if (m_OutstandingReads > 0)
				m_OutstandingReads--;
		}
		Pump();
	}

	void AssetStreamingQueue::OnLoadFinished(uint64_t bytes)
	{
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			ST_CORE_ASSERT(m_InFlightLoads > 0 && m_InFlightBytes >= bytes, "AssetStreamingQueue: a load finished that was never dispatched");
			if (m_InFlightLoads > 0)
				m_InFlightLoads--;
			m_InFlightBytes -= std::min(bytes, m_InFlightBytes);
		}
		Pump();
	}

	void AssetStreamingQueue::SetLimits(const AssetStreamingLimits& limits)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		m_Limits = limits;
	}

	void AssetStreamingQueue::Close()
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		m_Closed = true;
		m_Waiting.clear();
		m_WaitingKeys.clear();
		m_WaitingPerPriority = {};
	}

	bool AssetStreamingQueue::HasWork() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return !m_Waiting.empty() || m_InFlightLoads > 0;
	}

	size_t AssetStreamingQueue::GetQueuedCount() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return m_Waiting.size();
	}

	AssetStreamingQueueStats AssetStreamingQueue::GetStats() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		AssetStreamingQueueStats stats;
		stats.Queued = m_WaitingPerPriority;
		stats.InFlightLoads = m_InFlightLoads;
		stats.OutstandingReads = m_OutstandingReads;
		stats.InFlightBytes = m_InFlightBytes;
		stats.InFlightBytesHighWater = m_InFlightBytesHighWater;
		stats.Dispatched = m_Dispatched;
		return stats;
	}

}
