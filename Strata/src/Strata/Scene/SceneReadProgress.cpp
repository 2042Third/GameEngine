#include "stpch.h"
#include "Strata/Scene/SceneReadProgress.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Strata
{

	namespace
	{

		// The reads under way by scene asset. Trackers register and unregister under the lock, and Get reads their shares
		// under it, so a tracker is never read after it is gone.
		struct ReadRegistry
		{
			std::mutex Mutex;
			std::unordered_map<AssetHandle, std::vector<const SceneReadProgress::Tracker*>> Reads;
		};

		ReadRegistry& GetRegistry()
		{
			static ReadRegistry s_Registry;
			return s_Registry;
		}

	}

	SceneReadProgress::Tracker::Tracker(AssetHandle scene)
		: m_Scene(scene)
	{
		ReadRegistry& registry = GetRegistry();
		std::lock_guard lock(registry.Mutex);
		registry.Reads[m_Scene].push_back(this);
	}

	SceneReadProgress::Tracker::~Tracker()
	{
		ReadRegistry& registry = GetRegistry();
		std::lock_guard lock(registry.Mutex);
		auto it = registry.Reads.find(m_Scene);
		if (it == registry.Reads.end())
			return;
		std::vector<const Tracker*>& trackers = it->second;
		trackers.erase(std::remove(trackers.begin(), trackers.end(), this), trackers.end());
		if (trackers.empty())
			registry.Reads.erase(it);
	}

	void SceneReadProgress::Tracker::Report(float share)
	{
		// Only the reading thread reports: a plain read and write keep the share from going back.
		const float clamped = std::clamp(share, 0.0f, 1.0f);
		if (clamped > m_Share.load(std::memory_order_relaxed))
			m_Share.store(clamped, std::memory_order_relaxed);
	}

	std::optional<float> SceneReadProgress::Get(AssetHandle scene)
	{
		ReadRegistry& registry = GetRegistry();
		std::lock_guard lock(registry.Mutex);
		auto it = registry.Reads.find(scene);
		if (it == registry.Reads.end() || it->second.empty())
			return std::nullopt;
		float furthest = 0.0f;
		for (const Tracker* tracker : it->second)
			furthest = std::max(furthest, tracker->GetShare());
		return furthest;
	}

}
