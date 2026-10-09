#pragma once

#include "Strata/Core/UUID.h"

#include <optional>
#include <string_view>

namespace Strata
{

	// Assets are referenced by handle (a UUID that stays stable across renames and moves), never by path.
	using AssetHandle = UUID;

	enum class AssetType : uint16_t
	{
		None = 0,
		Scene,
		Prefab,
		Model,    // Imported model file (e.g. glTF): owns mesh/material/texture sub-assets, instantiates as a hierarchy
		Mesh,
		Material,
		Texture,
		AudioClip,
		Font
	};

	const char* AssetTypeToString(AssetType type);
	std::optional<AssetType> AssetTypeFromString(std::string_view text);

	// File extension of the asset types the engine writes itself (".stscene", ".stprefab", ".stmat"); empty for types
	// that are always imported from other formats.
	std::string_view GetNativeAssetExtension(AssetType type);

}
