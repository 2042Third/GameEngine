#include "stpch.h"
#include "Strata/Asset/AssetResidency.h"

#include "Strata/Asset/AssetManager.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <span>
#include <unordered_set>
#include <utility>

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
			if (it->second.PinCount++ == 0)
			{
				m_PinnedAssets++;
				UpdateListedLocked(it->second);
			}
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
		if (it != m_Entries.end() && it->second.Registration == registration && it->second.PinCount > 0 && --it->second.PinCount == 0)
		{
			m_PinnedAssets--;
			UpdateListedLocked(it->second);
		}
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
			// The same for the upload time: only the frame's first step.
			if (!std::isfinite(m_Budgets.UploadMsPerFrame) || m_Budgets.UploadMsPerFrame < 0.0f)
				m_Budgets.UploadMsPerFrame = 0.0f;
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

	void AssetManagerBase::ScheduleTrim(uint32_t delayFrames, uint32_t unusedFrames, std::span<const AssetHandle> keep)
	{
		std::unordered_set<AssetHandle> kept(keep.begin(), keep.end());
		std::scoped_lock<std::mutex> lock(m_Mutex);
		m_ScheduledTrim = ScheduledTrim { m_FrameIndex + delayFrames, unusedFrames, std::move(kept) };
	}

	std::vector<AssetResidencyInfo> AssetManagerBase::GetResidencyInfo() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		std::vector<AssetResidencyInfo> assets;
		assets.reserve(m_Listed.size());
		// What is listed, not what is registered: tens of thousands of registered assets cost nothing here.
		for (const AssetHandle handle : m_Listed)
		{
			m_ResidencyVisits++;
			const auto found = m_Entries.find(handle);
			if (found == m_Entries.end())
				continue;
			const AssetEntry& entry = found->second;
			assets.push_back(AssetResidencyInfo { handle, entry.Metadata.Type, entry.Metadata.Path, entry.Metadata.Name, entry.State, entry.Usage,
				entry.LastRequestedFrame, entry.PinCount, entry.IsMemoryAsset });
			AssetDetailLevels detail;
			if (entry.Loaded && entry.Loaded->GetDetailLevels(detail))
				assets.back().Detail = detail;
		}
		return assets;
	}

	void AssetManagerBase::GetResidencySamples(std::vector<AssetResidencySample>& outAssets) const
	{
		outAssets.clear();
		std::scoped_lock<std::mutex> lock(m_Mutex);
		// The same assets as GetResidencyInfo.
		for (const AssetHandle handle : m_Listed)
		{
			m_ResidencyVisits++;
			const auto found = m_Entries.find(handle);
			if (found == m_Entries.end())
				continue;
			const AssetEntry& entry = found->second;
			outAssets.push_back(AssetResidencySample { handle, entry.Metadata.Type, entry.State, entry.Usage, entry.LastRequestedFrame, entry.PinCount,
				entry.IsMemoryAsset });
			AssetDetailLevels detail;
			if (entry.Loaded && entry.Loaded->GetDetailLevels(detail))
				outAssets.back().Detail = detail;
		}
	}

	void AssetManagerBase::SetStateLocked(AssetEntry& entry, AssetState state)
	{
		static_assert(static_cast<size_t>(AssetState::Failed) + 1 == std::tuple_size_v<decltype(m_StateCounts)>, "A count per asset state");
		m_StateCounts[static_cast<size_t>(entry.State)]--;
		entry.State = state;
		m_StateCounts[static_cast<size_t>(state)]++;
		if (state != AssetState::Loading)
			entry.AwaitingImport = false;
		UpdateListedLocked(entry);
	}

	uint64_t AssetManagerBase::GetResidencyVisitCount() const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		return m_ResidencyVisits;
	}

	void AssetManagerBase::UpdateListedLocked(const AssetEntry& entry)
	{
		if (entry.State != AssetState::Unloaded || entry.Loaded || entry.PinCount > 0)
			m_Listed.insert(entry.Handle);
		else
			m_Listed.erase(entry.Handle);
	}

	bool AssetManagerBase::ValidateResidencyIndex(std::string* outError) const
	{
		std::scoped_lock<std::mutex> lock(m_Mutex);
		size_t listed = 0;
		for (const auto& [handle, entry] : m_Entries)
		{
			const bool shouldBe = entry.State != AssetState::Unloaded || entry.Loaded || entry.PinCount > 0;
			if (shouldBe != m_Listed.contains(handle))
			{
				if (outError)
					*outError = fmt::format("Asset {} ({}) is {}listed but should{} be (state {}, {}, {} pins)", handle.ToString(), entry.Metadata.Path,
						shouldBe ? "not " : "", shouldBe ? "" : " not", static_cast<int>(entry.State), entry.Loaded ? "loaded" : "not loaded", entry.PinCount);
				return false;
			}
			listed += shouldBe ? 1 : 0;
		}
		if (listed != m_Listed.size())
		{
			if (outError)
				*outError = fmt::format("{} assets are listed, {} of them registered", m_Listed.size(), listed);
			return false;
		}
		return true;
	}

	AssetManagerBase::AssetEntry& AssetManagerBase::AddEntryLocked(AssetHandle handle, bool& outInserted)
	{
		auto [it, inserted] = m_Entries.try_emplace(handle);
		outInserted = inserted;
		if (inserted)
		{
			it->second.Handle = handle;
			m_StateCounts[static_cast<size_t>(it->second.State)]++;
		}
		return it->second;
	}

	void AssetManagerBase::RemoveEntryLocked(std::unordered_map<AssetHandle, AssetEntry>::iterator it)
	{
		m_StateCounts[static_cast<size_t>(it->second.State)]--;
		if (it->second.PinCount > 0)
			m_PinnedAssets--;
		m_Listed.erase(it->first);
		m_Entries.erase(it);
	}

	void AssetManagerBase::TouchLocked(AssetEntry& entry)
	{
		if (entry.LastRequestedFrame == m_FrameIndex)
			return; // Requested in this frame already: its place in the orders is the frame's
		entry.LastRequestedFrame = m_FrameIndex;
		if (!entry.InRecencyOrders)
			return;

		// The latest request goes last: its key is the largest. Extracting the entry's own node and inserting it with the end
		// as hint both take constant time, where searching the order for the key would take a walk down a tree of every
		// resident asset (a renderer requests thousands of assets each frame).
		const RecencyKey key { m_FrameIndex, m_NextRecencySequence++ };
		for (AssetMemoryPool pool : c_AssetMemoryPools)
		{
			if (GetPoolBytes(entry.Usage, pool) == 0)
				continue;
			const size_t index = static_cast<size_t>(pool);
			RecencyOrder& order = m_RecencyOrders[index];
			ST_CORE_ASSERT(entry.RecencyNodes[index]->second == entry.Metadata.Handle, "Asset {} is missing from a recency order",
				entry.Metadata.Handle.ToString());
			auto node = order.extract(entry.RecencyNodes[index]);
			node.key() = key;
			entry.RecencyNodes[index] = order.insert(order.end(), std::move(node));
		}
	}

	void AssetManagerBase::LinkRecencyLocked(AssetEntry& entry)
	{
		// Arriving is no request: the entry goes where its latest request puts it, so an asset whose load outlasted every
		// request for it may go at once.
		LinkRecencyLocked(entry, RecencyKey { entry.LastRequestedFrame, m_NextRecencySequence++ });
	}

	std::optional<AssetManagerBase::RecencyKey> AssetManagerBase::GetRecencyKeyLocked(const AssetEntry& entry) const
	{
		if (!entry.InRecencyOrders)
			return std::nullopt;
		for (AssetMemoryPool pool : c_AssetMemoryPools)
		{
			if (GetPoolBytes(entry.Usage, pool) > 0)
				return entry.RecencyNodes[static_cast<size_t>(pool)]->first;
		}
		return std::nullopt;
	}

	void AssetManagerBase::LinkRecencyLocked(AssetEntry& entry, const RecencyKey& key)
	{
		for (AssetMemoryPool pool : c_AssetMemoryPools)
		{
			const size_t index = static_cast<size_t>(pool);
			if (GetPoolBytes(entry.Usage, pool) > 0)
				entry.RecencyNodes[index] = m_RecencyOrders[index].emplace(key, entry.Metadata.Handle).first;
		}
		entry.InRecencyOrders = true;
	}

	void AssetManagerBase::UnlinkRecencyLocked(AssetEntry& entry)
	{
		if (!entry.InRecencyOrders)
			return;
		for (AssetMemoryPool pool : c_AssetMemoryPools)
		{
			const size_t index = static_cast<size_t>(pool);
			if (GetPoolBytes(entry.Usage, pool) > 0)
				m_RecencyOrders[index].erase(entry.RecencyNodes[index]);
			entry.RecencyNodes[index] = {};
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
		entry.LoadedRevision = entry.Loaded ? entry.SourceRevision : 0;
		UpdateListedLocked(entry);
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
		SetStateLocked(entry, AssetState::Unloaded);
		entry.Error.clear();
		entry.Generation = m_NextGeneration++;
		PublishContentChange(entry.Metadata.Handle);
		m_Evictions++;
	}

	uint32_t AssetManagerBase::EvictUnusedLocked(uint32_t unusedFrames, std::vector<Ref<Asset>>& released, const std::unordered_set<AssetHandle>* keep)
	{
		// Every evictable resident asset that holds memory is in the order of each pool it uses.
		uint32_t evicted = 0;
		for (RecencyOrder& order : m_RecencyOrders)
		{
			for (auto it = order.begin(); it != order.end();)
			{
				AssetEntry& entry = m_Entries.at(it->second);
				++it; // Eviction removes the entry from the orders
				m_EvictionChecks++;
				if (WasRequestedWithinLocked(entry, unusedFrames))
					break; // Every later entry was requested even more recently
				if (!CanEvictLocked(entry) || (keep && keep->contains(entry.Metadata.Handle)))
					continue;
				EvictLocked(entry, released);
				evicted++;
			}
		}
		return evicted;
	}

	void AssetManagerBase::UpdateMemoryUsage(AssetHandle handle, const Asset& asset)
	{
		const AssetMemoryUsage usage = asset.GetMemoryUsage();
		std::scoped_lock<std::mutex> lock(m_Mutex);
		auto it = m_Entries.find(handle);
		if (it == m_Entries.end() || it->second.Loaded.get() != &asset)
			return;
		AssetEntry& entry = it->second;
		if (entry.Usage == usage)
			return;
		// The entry keeps its place in the recency orders (its latest request decides it, not its size); it moves only between
		// the orders of the pools it holds memory of.
		const std::optional<RecencyKey> key = GetRecencyKeyLocked(entry);
		const bool linked = entry.InRecencyOrders;
		UnlinkRecencyLocked(entry);
		m_Resident -= entry.Usage;
		entry.Usage = usage;
		m_Resident += entry.Usage;
		if (linked)
			LinkRecencyLocked(entry, key ? *key : RecencyKey { entry.LastRequestedFrame, m_NextRecencySequence++ });
	}

	void AssetManagerBase::ReleaseExtensionMemory(const AssetFinalizeContext& context)
	{
		for (AssetMemoryPool pool : c_AssetMemoryPools)
		{
			for (const Scope<AssetManagerExtension>& extension : m_Extensions)
			{
				uint64_t over = 0;
				{
					std::scoped_lock<std::mutex> lock(m_Mutex);
					const uint64_t resident = GetPoolBytes(m_Resident, pool);
					const uint64_t budget = m_Budgets.GetPoolBudget(pool);
					over = resident > budget ? resident - budget : 0;
				}
				if (over == 0)
					break;
				// Called without the manager's lock: the extension reports what it freed (UpdateMemoryUsage).
				extension->ReleaseMemory(pool, over, context);
			}
		}
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
		RecencyOrder& order = m_RecencyOrders[static_cast<size_t>(pool)];
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
