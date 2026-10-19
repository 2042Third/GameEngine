#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Math/AABB.h>

#include <cstdint>
#include <unordered_map>

namespace Strata
{

	class EditorAssetManager;

	// The bounds of what an asset draws around its origin (asset.info): a mesh's (the loaded mesh's, else what its import
	// recorded); a model's or prefab's, its template instantiated into a scene of its own with its animators' rest poses
	// (AnimationPreview) and measured by SceneBounds, around its (first) root's origin, as an instance placed at the origin
	// would be. Measuring requests what
	// it needs; while a template, a mesh or a skeleton is not resident the result is pending (with placeholder boxes in it).
	// Complete results are cached per content stamp (EditorAssetManager::GetContentStamp), so they are worked out again
	// only when the asset or what it uses changed. Main thread only.
	class AssetBounds
	{
	public:
		struct Result
		{
			AABB Bounds;          // Invalid: the asset draws nothing, or is no mesh, model or prefab
			bool Pending = false; // Something it draws was not resident (requested now): measure again later
		};

		Result Measure(EditorAssetManager& assets, AssetHandle asset);
		// Measurements taken (not answered from the cache), for tests.
		uint64_t GetMeasureCount() const { return m_MeasureCount; }
		void Clear() { m_Cache.clear(); }
	private:
		struct CacheEntry
		{
			uint64_t Stamp = 0;
			AABB Bounds;
		};
		std::unordered_map<AssetHandle, CacheEntry> m_Cache;
		uint64_t m_MeasureCount = 0;
	};

}
