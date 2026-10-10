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

	void AssetResolveCache::BeginFrame(const Ref<AssetManagerBase>& manager, std::vector<AssetHandle>& outChanged, bool& outAllChanged)
	{
		m_Frame++;
		m_Stats = {};
		// The previous frame's references go: what this frame does not use again becomes evictable.
		for (Entry* entry : m_Used)
			entry->Asset = nullptr;
		m_Used.clear();
		if (m_Frame % c_ForgetFrames == 0)
			std::erase_if(m_Entries, [this](const auto& entry) { return entry.second.Frame + c_ForgetFrames < m_Frame; });

		outAllChanged = false;
		if (manager != m_Manager)
		{
			m_Manager = manager;
			m_Entries.clear();
			m_ContentVersion = manager ? manager->GetContentVersion() : 0;
			outAllChanged = true;
			return;
		}
		if (!manager)
			return;
		const uint64_t version = manager->GetContentVersion();
		if (version == m_ContentVersion)
			return;
		if (!manager->GetContentChanges(m_ContentVersion, outChanged))
			outAllChanged = true;
		m_ContentVersion = version;
	}

	AssetResolveCache::Entry& AssetResolveCache::GetEntry(AssetHandle handle)
	{
		Entry& entry = m_Entries[handle];
		if (entry.Frame != m_Frame)
		{
			entry.Frame = m_Frame;
			entry.Asset = nullptr;
			entry.Kind = Lookup::None;
		}
		return entry;
	}

	const Ref<Asset>& AssetResolveCache::ResolveAsset(AssetHandle handle, AssetPriority priority, bool residentOnly)
	{
		if (!m_Manager || !handle.IsValid())
			return s_None;
		Entry& entry = GetEntry(handle);
		// Answered this frame: a request answers everything, a resident lookup answers other lookups and requests that
		// found the asset loaded.
		if (entry.Kind == Lookup::Request || (entry.Kind == Lookup::Resident && (residentOnly || entry.Asset)))
			return entry.Asset;

		if (residentOnly)
		{
			if (entry.Kind == Lookup::None)
				m_Stats.ResidentLookups++;
			entry.Kind = Lookup::Resident;
			if (m_Manager->GetAssetState(handle) == AssetState::Ready)
			{
				entry.Asset = m_Manager->GetAsset(handle, priority);
				m_Stats.GetAssetCalls++;
				if (entry.Asset)
					m_Used.push_back(&entry);
			}
			return entry.Asset;
		}

		entry.Kind = Lookup::Request;
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

	void AssetResolveCache::Prefetch(AssetHandle handle)
	{
		if (!m_Manager || !handle.IsValid())
			return;
		Entry& entry = GetEntry(handle);
		if (entry.Kind != Lookup::None)
			return;
		entry.Kind = Lookup::Prefetch;
		m_Manager->RequestLoad(handle, AssetPriority::Low);
		m_Stats.Prefetched++;
	}

}
