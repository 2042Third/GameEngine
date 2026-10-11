#include "stpch.h"
#include "Strata/Animation/AnimationRegistration.h"

#include "Strata/Animation/AnimationClip.h"
#include "Strata/Animation/AnimationController.h"
#include "Strata/Animation/Skeleton.h"
#include "Strata/Asset/AssetManager.h"

namespace Strata
{

	void RegisterAnimationModule()
	{
		// Skeletons and clips are stored in their cooked binary forms, controllers as their JSON source.
		AssetLoaderRegistry::Register(AssetType::Skeleton, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return Skeleton::Deserialize(data, outError);
		});
		AssetLoaderRegistry::Register(AssetType::AnimationClip, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return AnimationClip::Deserialize(data, outError);
		});
		AssetLoaderRegistry::Register(AssetType::AnimationController, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return AnimationController::Deserialize(data, outError);
		});
	}

}
