#include "stpch.h"
#include "Strata/Scene/SceneRegistration.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Scene/Prefab.h"

namespace Strata
{

	void RegisterSceneModule()
	{
		RegisterSceneComponents();

		// Prefabs and scenes are stored as their JSON source, models in their cooked JSON form.
		AssetLoaderRegistry::Register(AssetType::Prefab, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return Prefab::Deserialize(data, outError);
		});
		AssetLoaderRegistry::Register(AssetType::Model, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return Model::Deserialize(data, outError);
		});
		AssetLoaderRegistry::Register(AssetType::Scene, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return SceneAsset::Deserialize(data, outError);
		});
	}

}
