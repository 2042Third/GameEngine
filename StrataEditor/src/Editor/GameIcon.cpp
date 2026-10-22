#include "Editor/GameIcon.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/StringUtils.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace Strata
{

	namespace
	{

		// The formats the icon is made from: of what the texture importer reads, PSD, GIF and HDR files make no icon (layers,
		// animation frames, HDR pixels).
		constexpr std::array<std::string_view, 5> c_IconExtensions = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };

		void SetError(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
		}

	}

	std::optional<std::filesystem::path> GameIcon::FindSource(const EditorAssetManager& assets, AssetHandle icon, std::string* outError)
	{
		const std::optional<AssetMetadata> metadata = assets.GetMetadata(icon);
		if (!metadata || metadata->IsBuiltin())
		{
			SetError(outError, fmt::format("{} is no asset of the project: the game's icon is a texture of the project", icon.ToString()));
			return std::nullopt;
		}
		if (metadata->Type != AssetType::Texture)
		{
			SetError(outError, fmt::format("'{}' is a {}, not a texture", metadata->Path, AssetTypeToString(metadata->Type)));
			return std::nullopt;
		}
		if (metadata->IsSubAsset())
		{
			SetError(outError, fmt::format("'{}' ({}) is a texture inside '{}', which has no image file of its own: import the image as a texture of "
				"its own (a PNG, JPEG, TGA or BMP file in the asset directory) and use that", metadata->Name, icon.ToString(), metadata->Path));
			return std::nullopt;
		}
		const std::string extension = StringUtils::ToLower(FileSystem::ToUTF8(FileSystem::FromUTF8(metadata->Path).extension()));
		if (std::find(c_IconExtensions.begin(), c_IconExtensions.end(), extension) == c_IconExtensions.end())
		{
			SetError(outError, fmt::format("'{}' is not a PNG, JPEG, TGA or BMP image, which is what the game's icon is made from: convert it to PNG "
				"(at least 256 x 256 pixels)", metadata->Path));
			return std::nullopt;
		}
		std::filesystem::path source = assets.GetAbsolutePath(icon);
		if (source.empty())
		{
			SetError(outError, fmt::format("'{}' has no source file in the project", metadata->Path));
			return std::nullopt;
		}
		return source;
	}

}
