#pragma once

#include "Strata/Asset/Asset.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>

namespace Strata
{

	// A load waiting in, or dispatched from, an AssetStreamingQueue.
	struct AssetStreamingRequest
	{
		AssetHandle Handle = UUID::Null();
		AssetPriority Priority = AssetPriority::Normal;
		float Score = 0.0f;      // Within a priority, higher scores go first (e.g. how large on screen the asset is needed)
		uint64_t Order = 0;      // Request order: among equal priorities and scores, earlier requests go first
		uint64_t Bytes = 0;      // Counted in flight while the load runs: the stored size, or c_UnknownSizeBytes
		uint64_t Generation = 0; // The owner's token for the request (AssetManagerBase: the entry's generation)
	};

	struct AssetStreamingLimits
	{
		// Dispatching stops while the loads in flight hold this many bytes or more (one is always admitted when none
		// runs, so the bytes in flight stay below this plus the largest asset).
		uint64_t MaxInFlightBytes = 128ull << 20;
		uint32_t MaxOutstandingReads = 4; // Loads still reading (at least 1)
	};

	struct AssetStreamingQueueStats
	{
		std::array<uint32_t, 3> Queued = {}; // Waiting requests per priority (High, Normal, Low)
		uint32_t InFlightLoads = 0;          // Dispatched and not finished
		uint32_t OutstandingReads = 0;       // Dispatched and still reading
		uint64_t InFlightBytes = 0;
		uint64_t InFlightBytesHighWater = 0; // The most bytes in flight at once since the queue was created
		uint64_t Dispatched = 0;             // Loads dispatched since the queue was created
	};

	// The order in which asset loads start, and how many run at once. Requests wait keyed by priority, then score, then
	// request order; a repeated request for a waiting asset can raise it but never lowers it. A load is dispatched while
	// the loads in flight hold fewer bytes than MaxInFlightBytes and fewer than MaxOutstandingReads are still reading, so
	// a burst of requests cannot fill memory with read and decoded data the main thread has not finalized yet. A load is
	// in flight from its dispatch until its owner reports it finished (finalized or discarded); it is reading until the
	// owner reports the read done.
	//
	// Thread-safe. Dispatching calls the dispatch function outside the queue's lock, and Pump never dispatches recursively:
	// a pump requested while another one runs (on any thread, also from inside the dispatch function) is left to the
	// running one, which checks the limits again after every dispatch.
	class AssetStreamingQueue
	{
	public:
		// Counted for loads whose stored size is unknown.
		static constexpr uint64_t c_UnknownSizeBytes = 1ull << 20;

		// Starts a load. Returns false when the request no longer applies (e.g. its asset was unloaded meanwhile): the queue
		// then forgets it. For a load it started, the owner must call OnReadFinished once and OnLoadFinished once, in that
		// order (also when the load fails or is abandoned).
		using DispatchFunction = std::function<bool(const AssetStreamingRequest& request)>;

		explicit AssetStreamingQueue(DispatchFunction dispatch);

		AssetStreamingQueue(const AssetStreamingQueue&) = delete;
		AssetStreamingQueue& operator=(const AssetStreamingQueue&) = delete;

		// Queues a load of the asset (replacing a waiting request for it, which keeps its place in the request order). Does
		// not dispatch: call Pump. A storedSize of 0 counts as c_UnknownSizeBytes; a score that is not finite counts as 0.
		void Enqueue(AssetHandle handle, AssetPriority priority, float score, uint64_t storedSize, uint64_t generation);
		// Moves the waiting request for the asset up to a higher priority, or to a higher score within its priority; it keeps
		// its place in the request order. Returns false when nothing changed (no request waits, or it ranks as high already).
		bool Raise(AssetHandle handle, AssetPriority priority, float score);
		// Removes the waiting request for the asset; false when none waits (never queued, or already dispatched).
		bool Cancel(AssetHandle handle);
		bool IsQueued(AssetHandle handle) const;

		// Dispatches waiting requests in order while the limits admit them.
		void Pump();
		// A dispatched load finished reading (or never read). Pumps.
		void OnReadFinished();
		// A dispatched load is complete: its bytes (AssetStreamingRequest::Bytes) leave the flight. Pumps.
		void OnLoadFinished(uint64_t bytes);

		// Takes effect at the next pump.
		void SetLimits(const AssetStreamingLimits& limits);
		// Drops every waiting request and dispatches nothing from now on (the owner is shutting down).
		void Close();

		bool HasWork() const; // Requests wait or loads are in flight
		size_t GetQueuedCount() const;
		AssetStreamingQueueStats GetStats() const;
	private:
		// Ascending order is dispatch order: priority (High first), then score (highest first), then request order.
		using Key = std::tuple<uint8_t, float, uint64_t>;

		static Key MakeKey(AssetPriority priority, float score, uint64_t order) { return Key(static_cast<uint8_t>(priority), -score, order); }
		bool CanDispatchLocked() const;
		void InsertLocked(const AssetStreamingRequest& request);
		void EraseLocked(std::map<Key, AssetStreamingRequest>::iterator node);
	private:
		DispatchFunction m_Dispatch;

		mutable std::mutex m_Mutex;
		std::map<Key, AssetStreamingRequest> m_Waiting;
		std::unordered_map<AssetHandle, Key> m_WaitingKeys;
		std::array<uint32_t, 3> m_WaitingPerPriority = {};
		AssetStreamingLimits m_Limits;
		uint64_t m_NextOrder = 0;
		uint32_t m_InFlightLoads = 0;
		uint32_t m_OutstandingReads = 0;
		uint64_t m_InFlightBytes = 0;
		uint64_t m_InFlightBytesHighWater = 0;
		uint64_t m_Dispatched = 0;
		bool m_Pumping = false;
		bool m_Closed = false;
	};

}
