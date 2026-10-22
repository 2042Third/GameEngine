#pragma once

#include <Strata/Asset/AssetTypes.h>

#include <filesystem>
#include <optional>
#include <string>

namespace Strata
{

	class EditorAssetManager;

	// The game's icon (ProjectGameInfo::Icon): a texture of the project whose source file the export converts into the
	// images an exported game shows (Docs/DeveloperGuide.md, "Project settings").
	namespace GameIcon
	{

		// The source file of `icon` when it can be the game's icon: a texture of the project (not built-in) with an image
		// file of its own (not a texture inside a model) in a format the conversion decodes (PNG, JPEG, TGA or BMP). Otherwise
		// nullopt, with the reason, which names the asset. Reads only the asset's metadata (no file), so commands call it in
		// the frame.
		std::optional<std::filesystem::path> FindSource(const EditorAssetManager& assets, AssetHandle icon, std::string* outError = nullptr);

	}

}
