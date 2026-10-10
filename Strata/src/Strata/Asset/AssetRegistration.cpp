#include "stpch.h"
#include "Strata/Asset/AssetManager.h"

#include "Strata/Audio/AudioClipAsset.h"
#include "Strata/Renderer/Font.h"
#include "Strata/Renderer/Material.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/Texture.h"
#include "Strata/Scene/Prefab.h"

namespace Strata
{

	// Loaders turn stored bytes into asset objects for every built-in asset type. Textures, meshes, models and audio
	// clips are stored in cooked binary/JSON form; materials, prefabs, scenes and fonts are stored as their source.
	void CreateBuiltinAssetLoaders(std::unordered_map<AssetType, AssetLoadFunction>& loaders)
	{
		// Textures and fonts keep their bytes: they take them over instead of copying them.
		loaders.emplace(AssetType::Texture, [](const AssetMetadata&, AssetLoadData& data, std::string* outError) -> Ref<Asset>
		{
			return Texture::Deserialize(data.TakeBytes(), outError);
		});
		loaders.emplace(AssetType::Mesh, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return Mesh::Deserialize(data, outError);
		});
		loaders.emplace(AssetType::Material, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return Material::Deserialize(data, outError);
		});
		loaders.emplace(AssetType::Prefab, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return Prefab::Deserialize(data, outError);
		});
		loaders.emplace(AssetType::Model, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return Model::Deserialize(data, outError);
		});
		loaders.emplace(AssetType::Scene, [](const AssetMetadata&, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return SceneAsset::Deserialize(data, outError);
		});
		loaders.emplace(AssetType::AudioClip, [](const AssetMetadata& metadata, std::span<const uint8_t> data, std::string* outError) -> Ref<Asset>
		{
			return AudioClipAsset::Deserialize(data, metadata.Path.empty() ? metadata.Name : metadata.Path, outError);
		});
		loaders.emplace(AssetType::Font, [](const AssetMetadata&, AssetLoadData& data, std::string* outError) -> Ref<Asset>
		{
			return Font::Create(data.TakeBytes(), outError);
		});
	}

}
