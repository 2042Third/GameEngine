#pragma once

#include "Strata/Asset/AssetTypes.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	// Describes an exported game: what the runtime loads and how it presents it. Written by the editor's export next
	// to the asset pack and the runtime executable ("<Name>.stgame").
	//
	// { "Strata": { "Format": "Game", "Version": 1 },
	//   "Game": { "Name": ..., "AssetPack": "<file next to the manifest>", "StartScene": "<handle>",
	//             "Window": { "Width": 1280, "Height": 720, "Fullscreen": false, "VSync": true } } }
	struct GameManifest
	{
		static constexpr std::string_view c_FileExtension = ".stgame";
		static constexpr uint32_t c_FormatVersion = 1;

		std::string Name = "Game";
		std::string AssetPack;                   // File name relative to the manifest's directory
		AssetHandle StartScene = UUID::Null();
		uint32_t WindowWidth = 1280;
		uint32_t WindowHeight = 720;
		bool Fullscreen = false;
		bool VSync = true;

		nlohmann::json ToJson() const;
		static std::optional<GameManifest> FromJson(const nlohmann::json& json, std::string* outError = nullptr);

		bool Save(const std::filesystem::path& path, std::string* outError = nullptr) const;
		static std::optional<GameManifest> Load(const std::filesystem::path& path, std::string* outError = nullptr);
		// The manifest a runtime executable starts: "<executable stem>.stgame" next to it, otherwise the only
		// ".stgame" file in its directory. Empty when there is none (or several).
		static std::filesystem::path FindForExecutable(const std::filesystem::path& executablePath);
	};

}
