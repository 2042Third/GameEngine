#include "stpch.h"
#include "Strata/Asset/AssetResidency.h"

#include "Strata/Asset/AssetManager.h"

#include <algorithm>
#include <cmath>
#include <iterator>

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
		entry.LastRequestedFrame = m_FrameIndex;
		// The recency order stays sorted by the frame of the latest request: the most recent one goes last.
		if (entry.InRecencyOrder)
			m_RecencyOrder.splice(m_RecencyOrder.end(), m_RecencyOrder, entry.RecencyPosition);
	}

	Ref<Asset> AssetManagerBase::SetLoadedLocked(AssetEntry& entry, Ref<Asset> asset)
	{
		Ref<Asset> previous = std::move(entry.Loaded);
		m_Resident -= entry.Usage;
		entry.Usage = {};
		entry.Loaded = std::move(asset);
		if (entry.Loaded)
		{
			entry.Usage = entry.Loaded->GetMemoryUsage();
			m_Resident += entry.Usage;
		}

		const bool evictable = entry.Loaded && !entry.IsMemoryAsset;
		if (evictable && !entry.InRecencyOrder)
		{
			// Arriving is no request: an asset whose load outlasted every request for it may go at once. The order stays
			// sorted by the frame of the latest request; arrivals were mostly requested recently, so the search starts last.
			auto position = m_RecencyOrder.end();
			while (position != m_RecencyOrder.begin())
			{
				const auto before = std::prev(position);
				if (m_Entries.at(*before).LastRequestedFrame <= entry.LastRequestedFrame)
					break;
				position = before;
			}
			entry.RecencyPosition = m_RecencyOrder.insert(position, entry.Metadata.Handle);
			entry.InRecencyOrder = true;
		}
		else if (!evictable && entry.InRecencyOrder)
		{
			m_RecencyOrder.erase(entry.RecencyPosition);
			entry.InRecencyOrder = false;
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
		uint32_t evicted = 0;
		for (auto it = m_RecencyOrder.begin(); it != m_RecencyOrder.end();)
		{
			AssetEntry& entry = m_Entries.at(*it);
			++it; // Eviction removes the entry from the order
			if (WasRequestedWithinLocked(entry, unusedFrames))
				break; // Every later entry was requested even more recently
			if (!CanEvictLocked(entry))
				continue;
			EvictLocked(entry, released);
			evicted++;
		}
		return evicted;
	}

	void AssetManagerBase::EvictToBudgets()
	{
		std::vector<Ref<Asset>> released;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			EvictToFitLocked(AssetMemoryUsage {}, released);
		}
		// The evicted objects are destroyed here, after the lock: freeing large assets takes time.
	}

	void AssetManagerBase::EvictToFitLocked(const AssetMemoryUsage& incoming, std::vector<Ref<Asset>>& released)
	{
		// The sums cannot overflow: they count bytes of memory that exists.
		const auto isOverBudget = [this, &incoming](AssetMemoryPool pool)
		{
			return GetPoolBytes(m_Resident, pool) + GetPoolBytes(incoming, pool) > m_Budgets.GetPoolBudget(pool);
		};
		const auto anyOverBudget = [&isOverBudget]()
		{
			return std::any_of(c_AssetMemoryPools.begin(), c_AssetMemoryPools.end(), isOverBudget);
		};
		if (!anyOverBudget())
			return;

		// Least recently requested first; an asset goes when it holds memory of a pool that is still over budget.
		for (auto it = m_RecencyOrder.begin(); it != m_RecencyOrder.end();)
		{
			AssetEntry& entry = m_Entries.at(*it);
			++it; // Eviction removes the entry from the order
			if (WasRequestedWithinLocked(entry, m_EvictionGraceFrames))
				break; // Every later entry was requested even more recently
			const bool relieves = std::any_of(c_AssetMemoryPools.begin(), c_AssetMemoryPools.end(), [&](AssetMemoryPool pool)
			{
				return isOverBudget(pool) && GetPoolBytes(entry.Usage, pool) > 0;
			});
			if (!relieves || !CanEvictLocked(entry))
				continue;
			EvictLocked(entry, released);
			if (!anyOverBudget())
				break;
		}
	}

	uint64_t AssetManagerBase::MakeRoomFor(AssetHandle handle, const AssetMemoryUsage& incoming)
	{
		std::vector<Ref<Asset>> released;
		uint64_t evictedGpuBytes = 0;
		{
			std::scoped_lock<std::mutex> lock(m_Mutex);
			auto it = m_Entries.find(handle);
			// An arrival nobody requested lately is no reason to evict what is in use: it may be the next to go itself.
			if (it == m_Entries.end() || !WasRequestedWithinLocked(it->second, m_EvictionGraceFrames))
				return 0;
			const uint64_t residentGpuBytes = m_Resident.GetGpu();
			EvictToFitLocked(incoming, released);
			evictedGpuBytes = residentGpuBytes - m_Resident.GetGpu();
		}
		// The evicted objects are destroyed here, after the lock; their GPU memory follows once no frame in flight can use it.
		return evictedGpuBytes;
	}

}
