#include "stpch.h"
#include "Strata/Asset/AssetTypes.h"

namespace Strata
{

	const char* AssetTypeToString(AssetType type)
	{
		switch (type)
		{
			case AssetType::None:      return "None";
			case AssetType::Scene:     return "Scene";
			case AssetType::Prefab:    return "Prefab";
			case AssetType::Model:     return "Model";
			case AssetType::Mesh:      return "Mesh";
			case AssetType::Material:  return "Material";
			case AssetType::Texture:   return "Texture";
			case AssetType::AudioClip: return "AudioClip";
			case AssetType::Font:      return "Font";
		}
		return "None";
	}

	std::optional<AssetType> AssetTypeFromString(std::string_view text)
	{
		constexpr AssetType types[] = {
			AssetType::None, AssetType::Scene, AssetType::Prefab, AssetType::Model, AssetType::Mesh,
			AssetType::Material, AssetType::Texture, AssetType::AudioClip, AssetType::Font
		};
		for (AssetType type : types)
		{
			if (text == AssetTypeToString(type))
				return type;
		}
		return std::nullopt;
	}

}
