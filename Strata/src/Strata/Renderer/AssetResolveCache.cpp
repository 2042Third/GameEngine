#include "stpch.h"
#include "Strata/Renderer/AssetResolveCache.h"

namespace Strata
{

	namespace
	{

		// Entries unused for this many frames are forgotten, so handles that left the view do not accumulate.
		constexpr uint64_t c_ForgetFrames = 256;

	}

	const Ref<Asset> AssetResolveCache::s_None;

	void AssetResolveCache::BeginFrame(const Ref<AssetManagerBase>& manager)
	{
		// A frame that was not ended lets go of its references now.
		EndFrame();
		m_Frame++;
		m_Stats = {};
		if (m_Frame % c_ForgetFrames == 0)
			std::erase_if(m_Entries, [this](const auto& entry) { return entry.second.Frame + c_ForgetFrames < m_Frame; });

		// Compared by ownership: a manager that took the place (and the address) of a destroyed one is another manager.
		const bool sameManager = !m_LastManager.owner_before(manager) && !manager.owner_before(m_LastManager);
		if (!sameManager)
			m_Entries.clear();
		m_LastManager = manager;
		m_Manager = manager;
	}

	void AssetResolveCache::EndFrame()
	{
		for (Entry* entry : m_Used)
			entry->Asset = nullptr;
		m_Used.clear();
		m_Manager = nullptr;
	}

	AssetResolveCache::Entry& AssetResolveCache::GetEntry(AssetHandle handle)
	{
		Entry& entry = m_Entries[handle];
		if (entry.Frame != m_Frame)
		{
			entry.Frame = m_Frame;
			entry.Asset = nullptr;
			entry.Lookups = 0;
		}
		return entry;
	}

	const Ref<Asset>& AssetResolveCache::ResolveAsset(AssetHandle handle, AssetPriority priority, bool residentOnly)
	{
		if (!m_Manager || !handle.IsValid())
			return s_None;
		Entry& entry = GetEntry(handle);
		// Answered this frame: a request answers everything; a resident lookup answers other resident lookups, and requests
		// when it found the asset loaded.
		if ((entry.Lookups & c_Requested) != 0 || ((entry.Lookups & c_LookedUp) != 0 && (residentOnly || entry.Asset)))
			return entry.Asset;

		if (residentOnly)
		{
			entry.Lookups |= c_LookedUp;
			m_Stats.ResidentLookups++;
			if (m_Manager->GetAssetState(handle) == AssetState::Ready)
			{
				entry.Asset = m_Manager->GetAsset(handle, priority);
				m_Stats.GetAssetCalls++;
				if (entry.Asset)
					m_Used.push_back(&entry);
			}
			return entry.Asset;
		}

		entry.Lookups |= c_Requested;
		entry.Asset = m_Manager->GetAsset(handle, priority);
		m_Stats.GetAssetCalls++;
		m_Stats.Requested++;
		if (entry.Asset)
		{
			m_Used.push_back(&entry);
		}
		else if (m_Manager->GetAssetState(handle) == AssetState::Loading)
		{
			// Unknown handles stay Unloaded and failed ones Failed: only assets on their way are pending.
			m_Stats.Pending++;
		}
		return entry.Asset;
	}

	void AssetResolveCache::CountPending(AssetHandle handle)
	{
		if (!m_Manager || !handle.IsValid())
			return;
		Entry& entry = GetEntry(handle);
		if ((entry.Lookups & c_CountedPending) != 0)
			return;
		entry.Lookups |= c_CountedPending;
		m_Stats.Pending++;
	}

	void AssetResolveCache::Prefetch(AssetHandle handle)
	{
		if (!m_Manager || !handle.IsValid())
			return;
		Entry& entry = GetEntry(handle);
		// Requested already, or found loaded by a resident lookup (which stamped it as used); a resident lookup that found
		// nothing requested nothing, so the prefetch still has to.
		if ((entry.Lookups & (c_Requested | c_Prefetched)) != 0 || entry.Asset)
			return;
		entry.Lookups |= c_Prefetched;
		m_Manager->RequestLoad(handle, AssetPriority::Low);
		m_Stats.Prefetched++;
	}

}
