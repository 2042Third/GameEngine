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
		if (request.RangeId != 0)
		{
			m_WaitingRangeKeys.insert_or_assign(request.RangeId, key);
			m_WaitingRanges++;
		}
		else
		{
			m_WaitingKeys.insert_or_assign(request.Handle, key);
		}
		m_WaitingPerPriority[static_cast<size_t>(request.Priority)]++;
		m_WaitingBytes += request.Bytes;
	}

	void AssetStreamingQueue::EraseLocked(std::map<Key, AssetStreamingRequest>::iterator node)
	{
		m_WaitingPerPriority[static_cast<size_t>(node->second.Priority)]--;
		m_WaitingBytes -= node->second.Bytes;
		if (node->second.RangeId != 0)
		{
			m_WaitingRangeKeys.erase(node->second.RangeId);
			m_WaitingRanges--;
		}
		else
		{
			m_WaitingKeys.erase(node->second.Handle);
		}
		m_Waiting.erase(node);
	}

	void AssetStreamingQueue::Enqueue(AssetHandle handle, AssetPriority priority, float score, uint64_t storedSize, uint64_t generation, bool partial)
	{
		AssetStreamingRequest request { handle, priority, std::isfinite(score) ? score : 0.0f, 0, storedSize > 0 ? storedSize : c_UnknownSizeBytes, generation };
		request.Partial = partial;

		std::scoped_lock<std::mutex> lock(m_Mutex);
		if (m_Closed)
			return;
		auto waiting = m_WaitingKeys.find(handle);
		if (waiting != m_WaitingKeys.end())
		{
			auto node = m_Waiting.find(waiting->second);
			// A request of a later generation stays: this one is stale (its owner decided to load, was overtaken by an unload
			// and a new request on another thread, and queued afterwards). Replacing the newer request would leave a load
			// nobody dispatches.
			if (node->second.Generation > generation)
				return;
			request.Order = node->second.Order;
			EraseLocked(node);
		}
		else
		{
			request.Order = m_NextOrder++;
		}
		InsertLocked(request);
	}

	void AssetStreamingQueue::EnqueueRange(uint64_t rangeId, AssetHandle handle, AssetPriority priority, float score, uint64_t bytes)
	{
		ST_CORE_ASSERT(rangeId != 0, "Range reads need an id");
		AssetStreamingRequest request { handle, priority, std::isfinite(score) ? score : 0.0f, 0, bytes > 0 ? bytes : 1, 0 };
		request.RangeId = rangeId;

		std::scoped_lock<std::mutex> lock(m_Mutex);
		if (m_Closed || rangeId == 0 || m_WaitingRangeKeys.find(rangeId) != m_WaitingRangeKeys.end())
			return;
		request.Order = m_NextOrder++;
		InsertLocked(request);
	}

	bool AssetStreamingQueue::RaiseLocked(std::map<Key, AssetStreamingRequest>::iterator node, AssetPriority priority, float score)
	{
		const float sanitizedScore = std::isfinite(score) ? score : 0.0f;
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

	bool AssetStreamingQueue::Raise(AssetHandle handle, AssetPriority priority, float score)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto waiting = m_WaitingKeys.find(handle);
		if (waiting == m_WaitingKeys.end())
			return false;
		return RaiseLocked(m_Waiting.find(waiting->second), priority, score);
	}

	bool AssetStreamingQueue::RaiseRange(uint64_t rangeId, AssetPriority priority, float score)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto waiting = m_WaitingRangeKeys.find(rangeId);
		if (waiting == m_WaitingRangeKeys.end())
			return false;
		return RaiseLocked(m_Waiting.find(waiting->second), priority, score);
	}

	bool AssetStreamingQueue::CancelRange(uint64_t rangeId)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto waiting = m_WaitingRangeKeys.find(rangeId);
		if (waiting == m_WaitingRangeKeys.end())
			return false;
		EraseLocked(m_Waiting.find(waiting->second));
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
			if (request.RangeId != 0)
				m_InFlightRanges++;
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
				if (request.RangeId != 0)
					m_InFlightRanges--;
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

	void AssetStreamingQueue::OnLoadFinished(uint64_t bytes, bool range)
	{
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			ST_CORE_ASSERT(m_InFlightLoads > 0 && m_InFlightBytes >= bytes && (!range || m_InFlightRanges > 0),
				"AssetStreamingQueue: a load finished that was never dispatched");
			if (m_InFlightLoads > 0)
				m_InFlightLoads--;
			if (range && m_InFlightRanges > 0)
				m_InFlightRanges--;
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
		m_WaitingRangeKeys.clear();
		m_WaitingRanges = 0;
		m_WaitingPerPriority = {};
		m_WaitingBytes = 0;
	}

	bool AssetStreamingQueue::HasWork() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return !m_Waiting.empty() || m_InFlightLoads > 0;
	}

	bool AssetStreamingQueue::HasLoadWork() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return m_Waiting.size() > m_WaitingRanges || m_InFlightLoads > m_InFlightRanges;
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
		stats.WaitingBytes = m_WaitingBytes;
		stats.InFlightBytes = m_InFlightBytes;
		stats.InFlightBytesHighWater = m_InFlightBytesHighWater;
		stats.Dispatched = m_Dispatched;
		stats.QueuedRanges = m_WaitingRanges;
		stats.InFlightRanges = m_InFlightRanges;
		return stats;
	}

}
