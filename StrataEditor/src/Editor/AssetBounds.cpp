#include "Editor/AssetBounds.h"

#include "Editor/SceneBounds.h"

#include <Strata/Animation/AnimationPreview.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/Scene.h>

#include <vector>

namespace Strata
{

	AssetBounds::Result AssetBounds::Measure(EditorAssetManager& assets, AssetHandle asset)
	{
		const AssetType type = assets.GetAssetType(asset);
		if (type != AssetType::Mesh && type != AssetType::Model && type != AssetType::Prefab)
			return {};
		const uint64_t stamp = assets.GetContentStamp(asset);
		if (auto cached = m_Cache.find(asset); cached != m_Cache.end() && cached->second.Stamp == stamp)
			return Result { cached->second.Bounds, false };
		m_MeasureCount++;

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
			const EntityTemplate* entityTemplate = dynamic_cast<const EntityTemplate*>(loaded.get());
			if (!entityTemplate)
				return Result { AABB(), assets.GetAssetState(asset) == AssetState::Loading };
			// As prefab.instantiate places an instance at the origin, its skinned meshes standing in their rest pose as the
			// edited scene shows them.
			Scene scene("Bounds");
			std::string error;
			const std::vector<Entity> roots = entityTemplate->Instantiate(scene, {}, &error);
			if (roots.empty())
				return {};
			AnimationPreviewSpecification specification;
			specification.RestPoses = true;
			AnimationPreview poses(scene, specification);
			poses.Update();
			std::vector<AssetHandle> pending;
			const AABB bounds = SceneBounds::GetSceneBounds(scene, &pending);
			result.Pending = !pending.empty() || poses.HasWaitingRestPoses();
			// Around the (first) root's origin: an instance's root gets the translation it is placed at instead of its own.
			if (bounds.IsValid())
			{
				const glm::vec3 origin(scene.GetWorldTransform(roots.front())[3]);
				result.Bounds = AABB(bounds.Min - origin, bounds.Max - origin);
			}
		}
		if (!result.Pending)
			m_Cache[asset] = CacheEntry { stamp, result.Bounds };
		return result;
	}

}
