#include "stpch.h"
#include "Strata/Asset/AssetResidency.h"

#include "Strata/Asset/AssetManager.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace Strata
{

	uint64_t GetPoolBytes(const AssetMemoryUsage& usage, AssetMemoryPool pool)
	{
		switch (pool)
		{
			case AssetMemoryPool::Cpu:         return usage.Cpu;
			case AssetMemoryPool::GpuTextures: return usage.GpuTextures;
			case AssetMemoryPool::GpuBuffers:  return usage.GpuBuffers;
		}
		return 0;
	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetResidencyBudgets
	////////////////////////////////////////////////////////////////////////////////

	AssetResidencyBudgets AssetResidencyBudgets::FromDeviceBudget(uint64_t deviceBudgetBytes)
	{
		AssetResidencyBudgets budgets;
		if (deviceBudgetBytes == 0)
			return budgets;
		const double deviceBudget = static_cast<double>(deviceBudgetBytes);
		budgets.GpuTextures = static_cast<uint64_t>(deviceBudget * c_DefaultGpuTextureShare);
		budgets.GpuBuffers = static_cast<uint64_t>(deviceBudget * c_DefaultGpuBufferShare);
		return budgets;
	}

	uint64_t AssetResidencyBudgets::GetPoolBudget(AssetMemoryPool pool) const
	{
		switch (pool)
		{
			case AssetMemoryPool::Cpu:         return Cpu;
			case AssetMemoryPool::GpuTextures: return GpuTextures;
			case AssetMemoryPool::GpuBuffers:  return GpuBuffers;
		}
		return c_Unlimited;
	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetPin
	////////////////////////////////////////////////////////////////////////////////

	AssetPin::AssetPin(std::weak_ptr<AssetManagerBase> manager, AssetHandle handle, uint64_t registration)
		: m_Manager(std::move(manager)), m_Handle(handle), m_Registration(registration)
	{
	}

	AssetPin::~AssetPin()
	{
		Reset();
	}

	AssetPin::AssetPin(AssetPin&& other) noexcept
		: m_Manager(std::move(other.m_Manager)), m_Handle(other.m_Handle), m_Registration(other.m_Registration)
	{
		other.m_Handle = UUID::Null();
		other.m_Registration = 0;
	}

	AssetPin& AssetPin::operator=(AssetPin&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_Manager = std::move(other.m_Manager);
			m_Handle = other.m_Handle;
			m_Registration = other.m_Registration;
			other.m_Handle = UUID::Null();
			other.m_Registration = 0;
		}
		return *this;
	}

	void AssetPin::Reset()
	{
		if (m_Handle.IsValid())
		{
			if (Ref<AssetManagerBase> manager = m_Manager.lock())
				manager->Unpin(m_Handle, m_Registration);
		}
		m_Manager.reset();
		m_Handle = UUID::Null();
		m_Registration = 0;
	}

	////////////////////////////////////////////////////////////////////////////////
	// AssetManagerBase: residency
	////////////////////////////////////////////////////////////////////////////////

	AssetPin AssetManagerBase::Pin(AssetHandle handle, AssetPriority priority)
	{
		std::weak_ptr<AssetManagerBase> self = weak_from_this();
		ST_CORE_ASSERT(!self.expired(), "Asset managers must be owned by a Ref to pin assets");
		if (self.expired())
			return {};

		uint64_t registration = 0;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(handle);
			if (it == m_Entries.end())
				return {};
			it->second.PinCount++;
			registration = it->second.Registration;
		}
		RequestLoad(handle, priority);
		return AssetPin(std::move(self), handle, registration);
	}

	void AssetManagerBase::Unpin(AssetHandle handle, uint64_t registration)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		// A pin of an asset that was unregistered (and maybe registered again since) counts for nothing any more.
		if (it != m_Entries.end() && it->second.Registration == registration && it->second.PinCount > 0)
			it->second.PinCount--;
	}

	uint32_t AssetManagerBase::GetPinCount(AssetHandle handle) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		return it != m_Entries.end() ? it->second.PinCount : 0;
	}

	void AssetManagerBase::SetResidencyBudgets(const AssetResidencyBudgets& budgets)
	{
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			m_Budgets = budgets;
			// A time budget that is not a number allows only the first finalization of each frame.
			if (!std::isfinite(m_Budgets.FinalizeMsPerFrame) || m_Budgets.FinalizeMsPerFrame < 0.0f)
				m_Budgets.FinalizeMsPerFrame = 0.0f;
		}
		UpdateStreamingLimits();
		m_Queue.Pump();
	}

	AssetResidencyBudgets AssetManagerBase::GetResidencyBudgets() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return m_Budgets;
	}

	uint64_t AssetManagerBase::GetFrameIndex() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return m_FrameIndex;
	}

	uint32_t AssetManagerBase::TrimUnused(uint32_t unusedFrames)
	{
		std::vector<Ref<Asset>> released;
		uint32_t evicted = 0;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			evicted = EvictUnusedLocked(unusedFrames, released);
		}
		return evicted; // The evicted objects are destroyed here, after the lock
	}

	void AssetManagerBase::ScheduleTrim(uint32_t delayFrames, uint32_t unusedFrames)
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		m_ScheduledTrim = ScheduledTrim { m_FrameIndex + delayFrames, unusedFrames };
	}

	std::vector<AssetResidencyInfo> AssetManagerBase::GetResidencyInfo() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		std::vector<AssetResidencyInfo> assets;
		for (const auto& [handle, entry] : m_Entries)
		{
			if (entry.State == AssetState::Unloaded && !entry.Loaded && entry.PinCount == 0)
				continue;
			assets.push_back(AssetResidencyInfo { handle, entry.Metadata.Type, entry.Metadata.Path, entry.Metadata.Name, entry.State, entry.Usage,
				entry.LastRequestedFrame, entry.PinCount, entry.IsMemoryAsset });
		}
		return assets;
	}

	void AssetManagerBase::TouchLocked(AssetEntry& entry)
	{
		if (entry.LastRequestedFrame == m_FrameIndex)
			return; // Requested in this frame already: its place in the orders is the frame's
		entry.LastRequestedFrame = m_FrameIndex;
		if (!entry.InRecencyOrders)
			return;

		// The latest request goes last, which is where inserting with the end as hint takes constant time.
		const RecencyKey key { m_FrameIndex, m_NextRecencySequence++ };
		for (AssetMemoryPool pool : c_AssetMemoryPools)
		{
			if (GetPoolBytes(entry.Usage, pool) == 0)
				continue;
			std::map<RecencyKey, AssetHandle>& order = m_RecencyOrders[static_cast<size_t>(pool)];
			auto node = order.extract(entry.Recency);
			ST_CORE_ASSERT(!node.empty(), "Asset {} is missing from a recency order", entry.Metadata.Handle.ToString());
			if (node.empty())
				continue;
			node.key() = key;
			order.insert(order.end(), std::move(node));
		}
		entry.Recency = key;
	}

	void AssetManagerBase::LinkRecencyLocked(AssetEntry& entry)
	{
		// Arriving is no request: the entry goes where its latest request puts it, so an asset whose load outlasted every
		// request for it may go at once.
		entry.Recency = RecencyKey { entry.LastRequestedFrame, m_NextRecencySequence++ };
		for (AssetMemoryPool pool : c_AssetMemoryPools)
		{
			if (GetPoolBytes(entry.Usage, pool) > 0)
				m_RecencyOrders[static_cast<size_t>(pool)].emplace(entry.Recency, entry.Metadata.Handle);
		}
		entry.InRecencyOrders = true;
	}

	void AssetManagerBase::UnlinkRecencyLocked(AssetEntry& entry)
	{
		if (!entry.InRecencyOrders)
			return;
		for (AssetMemoryPool pool : c_AssetMemoryPools)
		{
			if (GetPoolBytes(entry.Usage, pool) > 0)
				m_RecencyOrders[static_cast<size_t>(pool)].erase(entry.Recency);
		}
		entry.InRecencyOrders = false;
	}

	Ref<Asset> AssetManagerBase::SetLoadedLocked(AssetEntry& entry, Ref<Asset> asset)
	{
		Ref<Asset> previous = std::move(entry.Loaded);
		// The orders the entry is in follow from its Usage: leave them before it changes.
		UnlinkRecencyLocked(entry);
		m_Resident -= entry.Usage;
		entry.Usage = {};
		entry.Loaded = std::move(asset);
		if (entry.Loaded)
		{
			entry.Usage = entry.Loaded->GetMemoryUsage();
			m_Resident += entry.Usage;
			if (!entry.IsMemoryAsset)
				LinkRecencyLocked(entry);
		}
		return previous;
	}

	bool AssetManagerBase::AbandonLoadLocked(AssetEntry& entry)
	{
		if (entry.State != AssetState::Loading)
			return false;
		m_Queue.Cancel(entry.Metadata.Handle);
		return true;
	}

	bool AssetManagerBase::WasRequestedWithinLocked(const AssetEntry& entry, uint32_t frames) const
	{
		return m_FrameIndex - std::min(entry.LastRequestedFrame, m_FrameIndex) <= frames;
	}

	bool AssetManagerBase::CanEvictLocked(const AssetEntry& entry) const
	{
		if (entry.State != AssetState::Ready || !entry.Loaded || entry.IsMemoryAsset || entry.Metadata.IsBuiltin() || entry.PinCount > 0)
			return false;
		// Someone outside the manager still holds the object, or its data: evicting it would free nothing now, and the
		// next request would load a second copy.
		return entry.Loaded.use_count() == 1 && !entry.Loaded->IsDataShared();
	}

	void AssetManagerBase::EvictLocked(AssetEntry& entry, std::vector<Ref<Asset>>& released)
	{
		released.push_back(SetLoadedLocked(entry, nullptr));
		entry.State = AssetState::Unloaded;
		entry.Error.clear();
		entry.Generation = m_NextGeneration++;
		PublishContentChange(entry.Metadata.Handle);
		m_Evictions++;
	}

	uint32_t AssetManagerBase::EvictUnusedLocked(uint32_t unusedFrames, std::vector<Ref<Asset>>& released)
	{
		// Every evictable resident asset that holds memory is in the order of each pool it uses.
		uint32_t evicted = 0;
		for (std::map<RecencyKey, AssetHandle>& order : m_RecencyOrders)
		{
			for (auto it = order.begin(); it != order.end();)
			{
				AssetEntry& entry = m_Entries.at(it->second);
				++it; // Eviction removes the entry from the orders
				m_EvictionChecks++;
				if (WasRequestedWithinLocked(entry, unusedFrames))
					break; // Every later entry was requested even more recently
				if (!CanEvictLocked(entry))
					continue;
				EvictLocked(entry, released);
				evicted++;
			}
		}
		return evicted;
	}

	void AssetManagerBase::EvictToBudgets()
	{
		std::vector<Ref<Asset>> released;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			for (AssetMemoryPool pool : c_AssetMemoryPools)
				EvictToFitLocked(pool, 0, released);
		}
		// The evicted objects are destroyed here, after the lock: freeing large assets takes time.
	}

	void AssetManagerBase::EvictToFitLocked(AssetMemoryPool pool, uint64_t extraBytes, std::vector<Ref<Asset>>& released)
	{
		const uint64_t budget = m_Budgets.GetPoolBudget(pool);
		// The sum cannot overflow: it counts bytes of memory that exists.
		const auto isOverBudget = [&]() { return GetPoolBytes(m_Resident, pool) + extraBytes > budget; };

		// Least recently requested first. The order holds only assets with memory of this pool, so a pool that stays over
		// budget (what is in use exceeds it) costs a look at its own stale assets, not at every stale asset.
		std::map<RecencyKey, AssetHandle>& order = m_RecencyOrders[static_cast<size_t>(pool)];
		for (auto it = order.begin(); it != order.end() && isOverBudget();)
		{
			AssetEntry& entry = m_Entries.at(it->second);
			++it; // Eviction removes the entry from the orders
			m_EvictionChecks++;
			if (WasRequestedWithinLocked(entry, m_EvictionGraceFrames))
				break; // Every later entry was requested even more recently
			if (CanEvictLocked(entry))
				EvictLocked(entry, released);
		}
	}

	AssetManagerBase::RoomResult AssetManagerBase::MakeRoomFor(AssetHandle handle, const AssetMemoryUsage& incoming)
	{
		std::vector<Ref<Asset>> released;
		RoomResult room;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(handle);
			// An arrival nobody requested lately is no reason to evict what is in use: it may be the next to go itself.
			if (it == m_Entries.end() || !WasRequestedWithinLocked(it->second, m_EvictionGraceFrames))
				return room;

			// Only the pools the arrival needs: pools over budget for other reasons are Update's business (an arrival must not
			// wait for GPU memory it does not need).
			const uint64_t residentGpuBytes = m_Resident.GetGpu();
			for (AssetMemoryPool pool : c_AssetMemoryPools)
			{
				const uint64_t bytes = GetPoolBytes(incoming, pool);
				if (bytes > 0)
					EvictToFitLocked(pool, bytes + GetPoolBytes(m_Reserved, pool), released);
			}
			room.EvictedGpuBytes = residentGpuBytes - m_Resident.GetGpu();
			room.Reserved = incoming;
			m_Reserved += incoming;
		}
		// The evicted objects are destroyed here, after the lock; their GPU memory follows once no frame in flight can use it.
		return room;
	}

}
