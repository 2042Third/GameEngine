#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Math/AABB.h>

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class EditorAssetManager;

	// The bounds of what an asset draws around its origin (asset.info): a mesh's (the loaded mesh's, else what its import
	// recorded); a model's or prefab's, its template instantiated into a scene of its own with its animators' rest poses
	// (AnimationPreview) and measured by SceneBounds, around its (first) root's origin, as an instance placed at the origin
	// would be. Measuring requests what
	// it needs; while a template, a mesh or a skeleton is not resident the result is pending (with placeholder boxes in it).
	// Results are cached per content stamp (EditorAssetManager::GetContentStamp), so they are worked out again only when
	// the asset or what it uses changed. A pending result is kept too: it measures a pending asset again once something it
	// waits for was published (or the asset itself), or after a second (c_PendingRemeasureSeconds), so that a drag hovering
	// over the view while unrelated assets stream in does not instantiate its template every frame; it answers without
	// working out the stamp, so that asset.place's poll allocates nothing while it waits. Main thread only.
	class AssetBounds
	{
	public:
		struct Result
		{
			AABB Bounds;          // Invalid: the asset draws nothing, or is no mesh, model or prefab
			bool Pending = false; // Something it draws was not resident (requested now): measure again later
		};
		// How long a pending result is answered from the cache at most: a safety net for waits that publish nothing.
		static constexpr double c_PendingRemeasureSeconds = 1.0;

		Result Measure(EditorAssetManager& assets, AssetHandle asset);
		// Measurements taken (not answered from the cache), for tests.
		uint64_t GetMeasureCount() const { return m_MeasureCount; }
		// Content stamps worked out (they allocate; a pending result answers without one), for tests.
		uint64_t GetStampCount() const { return m_StampCount; }
		void Clear() { m_Cache.clear(); }
	private:
		struct CacheEntry
		{
			uint64_t Stamp = 0;
			AABB Bounds;
			bool Pending = false;
			uint64_t ContentVersion = 0;         // The manager's GetContentVersion when a pending result was measured
			std::vector<AssetHandle> Waiting;    // What a pending measurement waited for (loading meshes, waiting skeletons)
			std::chrono::steady_clock::time_point MeasuredAt;
		};
		// Whether a pending entry still answers: nothing it waits for was published since, and it is not too old. Moves the
		// entry's ContentVersion past the unrelated changes it looked at.
		bool IsPendingCurrent(const EditorAssetManager& assets, AssetHandle asset, CacheEntry& entry);

		std::unordered_map<AssetHandle, CacheEntry> m_Cache;
		std::vector<AssetHandle> m_Changes; // GetContentChanges' answer, its capacity reused
		uint64_t m_MeasureCount = 0;
		uint64_t m_StampCount = 0;
	};

}
