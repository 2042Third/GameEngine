#include "Editor/AssetBounds.h"

#include "Editor/SceneBounds.h"

#include <Strata/Animation/AnimationComponents.h>
#include <Strata/Animation/AnimationPreview.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <vector>

namespace Strata
{

	AssetBounds::Result AssetBounds::Measure(EditorAssetManager& assets, AssetHandle asset)
	{
		const AssetType type = assets.GetAssetType(asset);
		if (type != AssetType::Mesh && type != AssetType::Model && type != AssetType::Prefab)
			return {};
		// A pending result answers before the stamp is worked out: asset.place's wait and a drag's hover ask every frame, and
		// the stamp allocates (GetContentStamp formats the import's state). A re-import that changes the stamp publishes the
		// asset, which IsPendingCurrent sees, and a change that publishes nothing is caught by its safety net.
		const auto cached = m_Cache.find(asset);
		if (cached != m_Cache.end() && cached->second.Pending && IsPendingCurrent(assets, asset, cached->second))
			return Result { cached->second.Bounds, true };
		const uint64_t stamp = assets.GetContentStamp(asset);
		m_StampCount++;
		if (cached != m_Cache.end() && !cached->second.Pending && cached->second.Stamp == stamp)
			return Result { cached->second.Bounds, false };
		m_MeasureCount++;

		// Read before measuring: a load published while it measures counts as a change it may have missed.
		const uint64_t contentVersion = assets.GetContentVersion();
		std::vector<AssetHandle> waiting;
		Result result;
		if (type == AssetType::Mesh)
		{
			// The loaded mesh, else what its import recorded (known before it loads).
			if (const Ref<Mesh> mesh = assets.GetAsset<Mesh>(asset); mesh && mesh->GetBounds().IsValid())
				result.Bounds = mesh->GetBounds();
			else if (const std::optional<AABB> recorded = assets.GetBounds(asset))
				result.Bounds = *recorded;
			else
				result.Pending = assets.GetAssetState(asset) == AssetState::Loading;
		}
		else
		{
			const Ref<Asset> loaded = assets.GetAsset(asset);
			if (const EntityTemplate* entityTemplate = dynamic_cast<const EntityTemplate*>(loaded.get()))
			{
				// As prefab.instantiate places an instance at the origin, its skinned meshes standing in their rest pose as the
				// edited scene shows them.
				Scene scene("Bounds");
				std::string error;
				const std::vector<Entity> roots = entityTemplate->Instantiate(scene, {}, &error);
				if (!roots.empty())
				{
					AnimationPreviewSpecification specification;
					specification.RestPoses = true;
					AnimationPreview poses(scene, specification);
					poses.Update();
					const AABB bounds = SceneBounds::GetSceneBounds(scene, &waiting);
					if (poses.HasWaitingRestPoses())
					{
						// What the rest poses wait for (skeletons, and what plays on start): their arrival is what makes measuring
						// again worthwhile.
						for (const auto [handle, animator] : scene.GetRegistry().view<AnimatorComponent>().each())
						{
							for (const AssetHandle used : { animator.Skeleton, animator.Controller, animator.Clip })
							{
								if (used.IsValid())
									waiting.push_back(used);
							}
						}
						result.Pending = true;
					}
					result.Pending = result.Pending || !waiting.empty();
					// Around the (first) root's origin: an instance's root gets the translation it is placed at instead of its own.
					if (bounds.IsValid())
					{
						const glm::vec3 origin(scene.GetWorldTransform(roots.front())[3]);
						result.Bounds = AABB(bounds.Min - origin, bounds.Max - origin);
					}
				}
			}
			else
			{
				result.Pending = assets.GetAssetState(asset) == AssetState::Loading;
			}
		}

		CacheEntry& entry = m_Cache[asset];
		entry.Stamp = stamp;
		entry.Bounds = result.Bounds;
		entry.Pending = result.Pending;
		entry.ContentVersion = contentVersion;
		entry.Waiting = result.Pending ? std::move(waiting) : std::vector<AssetHandle>();
		entry.MeasuredAt = std::chrono::steady_clock::now();
		return result;
	}

	bool AssetBounds::IsPendingCurrent(const EditorAssetManager& assets, AssetHandle asset, CacheEntry& entry)
	{
		if (std::chrono::steady_clock::now() - entry.MeasuredAt >= std::chrono::duration<double>(c_PendingRemeasureSeconds))
			return false;
		const uint64_t contentVersion = assets.GetContentVersion();
		if (contentVersion == entry.ContentVersion)
			return true;
		m_Changes.clear();
		if (!assets.GetContentChanges(entry.ContentVersion, m_Changes))
			return false;
		for (const AssetHandle changed : m_Changes)
		{
			if (changed == asset || std::find(entry.Waiting.begin(), entry.Waiting.end(), changed) != entry.Waiting.end())
				return false;
		}
		// Nothing it waits for changed: later checks need only look at what is published from now on.
		entry.ContentVersion = contentVersion;
		return true;
	}

}
