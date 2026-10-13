#include "stpch.h"
#include "Strata/Renderer/AssetResolveCache.h"

#include "Strata/Core/Assert.h"

#include <algorithm>

namespace Strata
{

	namespace
	{

		// Entries unused for this many frames are forgotten, so handles that left the view do not accumulate.
		constexpr uint64_t c_ForgetFrames = 256;

	}

	const Ref<Asset> AssetResolveCache::s_None;

	void AssetResolveCache::Recording::Clear()
	{
		m_Handles.clear();
		m_Assets.clear();
		m_Counted.clear();
	}

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
			entry->Resolved = nullptr;
		m_Used.clear();
		m_Manager = nullptr;
		m_Replay = nullptr;
		// A recording does not span frames: the entries it points at are reset by the next frame.
		m_Recording = false;
		m_RecordedPending = false;
		m_Recorded.clear();
	}

	AssetResolveCache::Entry& AssetResolveCache::GetEntry(AssetHandle handle)
	{
		Entry& entry = m_Entries[handle];
		if (entry.Frame != m_Frame)
		{
			entry.Frame = m_Frame;
			entry.Resolved = nullptr;
			entry.Lookups = 0;
			if (m_Replay)
				AdoptReplayed(entry, handle);
		}
		return entry;
	}

	void AssetResolveCache::AdoptReplayed(Entry& entry, AssetHandle handle)
	{
		const std::vector<AssetHandle>& handles = m_Replay->m_Handles;
		const auto found = std::lower_bound(handles.begin(), handles.end(), handle);
		if (found == handles.end() || *found != handle)
			return;
		const size_t index = static_cast<size_t>(found - handles.begin());
		// The replay found it loaded in this frame; should it have been dropped since, the lookup asks the manager again.
		entry.Resolved = m_Replay->m_Assets[index].lock();
		if (!entry.Resolved)
			return;
		entry.Lookups = static_cast<uint8_t>(m_Replay->m_Counted[index] | c_LookedUp);
		m_Used.push_back(&entry);
	}

	const Ref<Asset>& AssetResolveCache::ResolveAsset(AssetHandle handle, AssetPriority priority, bool residentOnly)
	{
		if (!m_Manager || !handle.IsValid())
			return s_None;
		Entry& entry = GetEntry(handle);
		const uint8_t counted = LookUp(entry, handle, priority, residentOnly);
		if (m_Recording)
			m_Recorded.push_back(RecordedCall { handle, &entry, counted, true });
		return entry.Resolved;
	}

	uint8_t AssetResolveCache::LookUp(Entry& entry, AssetHandle handle, AssetPriority priority, bool residentOnly)
	{
		// Answered this frame: a request answers everything; a resident lookup answers other resident lookups, and requests
		// when it found the asset loaded.
		if ((entry.Lookups & c_Requested) != 0 || ((entry.Lookups & c_LookedUp) != 0 && (residentOnly || entry.Resolved)))
			return 0;

		if (residentOnly)
		{
			entry.Lookups |= c_LookedUp;
			m_Stats.ResidentLookups++;
			if (m_Manager->GetAssetState(handle) == AssetState::Ready)
			{
				entry.Resolved = m_Manager->GetAsset(handle, priority);
				m_Stats.GetAssetCalls++;
				if (entry.Resolved)
					m_Used.push_back(&entry);
			}
			return c_LookedUp;
		}

		entry.Lookups |= c_Requested;
		entry.Resolved = m_Manager->GetAsset(handle, priority);
		m_Stats.GetAssetCalls++;
		m_Stats.Requested++;
		if (entry.Resolved)
		{
			m_Used.push_back(&entry);
		}
		else if (m_Manager->GetAssetState(handle) == AssetState::Loading)
		{
			// Unknown handles stay Unloaded and failed ones Failed: only assets on their way are pending.
			m_Stats.Pending++;
		}
		return c_Requested;
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
		if (m_Recording)
			m_RecordedPending = true;
	}

	void AssetResolveCache::Prefetch(AssetHandle handle)
	{
		if (!m_Manager || !handle.IsValid())
			return;
		Entry& entry = GetEntry(handle);
		// Requested already, or found loaded by a resident lookup (which stamped it as used); a resident lookup that found
		// nothing requested nothing, so the prefetch still has to.
		uint8_t counted = 0;
		if ((entry.Lookups & (c_Requested | c_Prefetched)) == 0 && !entry.Resolved)
		{
			entry.Lookups |= c_Prefetched;
			m_Manager->RequestLoad(handle, AssetPriority::Low);
			m_Stats.Prefetched++;
			counted = c_Prefetched;
		}
		if (m_Recording)
			m_Recorded.push_back(RecordedCall { handle, &entry, counted, false });
	}

	void AssetResolveCache::BeginRecording()
	{
		m_Recording = true;
		m_RecordedPending = false;
		m_Recorded.clear();
	}

	bool AssetResolveCache::EndRecording(Recording& out)
	{
		out.Clear();
		bool replayable = m_Recording && !m_RecordedPending;
		m_Recording = false;
		m_RecordedPending = false;

		// One asset per handle, with everything its calls counted; the calls of a handle are adjacent once sorted.
		std::sort(m_Recorded.begin(), m_Recorded.end(), [](const RecordedCall& a, const RecordedCall& b) { return a.Handle < b.Handle; });
		for (size_t first = 0; first < m_Recorded.size() && replayable;)
		{
			const AssetHandle handle = m_Recorded[first].Handle;
			uint8_t counted = 0;
			bool lookedUp = false;
			size_t next = first;
			for (; next < m_Recorded.size() && m_Recorded[next].Handle == handle; next++)
			{
				counted |= m_Recorded[next].Counted;
				lookedUp = lookedUp || m_Recorded[next].Lookup;
			}
			const Entry& entry = *m_Recorded[first].Target;
			first = next;
			if (!lookedUp)
				continue; // Only prefetched
			if (!entry.Resolved)
			{
				replayable = false;
				break;
			}
			out.m_Handles.push_back(handle);
			out.m_Assets.push_back(entry.Resolved);
			out.m_Counted.push_back(counted);
		}
		m_Recorded.clear();
		if (!replayable)
			out.Clear();
		return replayable;
	}

	bool AssetResolveCache::Replay(const Recording& recording)
	{
		ST_CORE_ASSERT(!m_Recording, "AssetResolveCache: replaying while recording");
		if (!m_Manager || m_Replay || m_Recording || recording.IsEmpty())
			return false;
		if (!m_Manager->TouchLoadedAssets(recording.m_Handles, recording.m_Assets))
			return false;
		// What the recorded frame counted; every asset is one GetAsset call of the manager (TouchLoadedAssets counts it).
		for (const uint8_t counted : recording.m_Counted)
		{
			m_Stats.GetAssetCalls++;
			if ((counted & c_Requested) != 0)
				m_Stats.Requested++;
			if ((counted & c_LookedUp) != 0)
				m_Stats.ResidentLookups++;
			if ((counted & c_Prefetched) != 0)
				m_Stats.Prefetched++;
		}
		m_Replay = &recording;
		return true;
	}

}
