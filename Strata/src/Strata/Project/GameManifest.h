#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Project/GameInfo.h"

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// Describes an exported game: what the runtime loads and how it presents it. Written by the editor's export next
	// to the asset pack, the script module and the runtime executable ("<Name>.stgame").
	//
	// { "Strata": { "Format": "Game", "Version": 5 },
	//   "Game": { "Name": ..., "Title": ..., "Version": "1.0.0", "Company": ..., "Copyright": ...,
	//             "Icon": "<PNG file next to the manifest, or empty>",
	//             "AssetPack": "<file next to the manifest>", "StartScene": "<handle>",
	//             "LoadingScene": "<handle>" (optional),
	//             "PatchPacks": [ "<file next to the manifest>", ... ],
	//             "ScriptModule": "<file next to the manifest, or empty>",
	//             "Window": { "Width": 1280, "Height": 720, "Fullscreen": false, "VSync": true } },
	//   "Settings": { "<Section id>": { "Version": n, "Value": ... } } }
	// Version 4 manifests (without "Settings" and the identity), version 3 manifests (without "LoadingScene"), version 2
	// manifests (without "PatchPacks") and version 1 manifests (games without scripts) still load.
	struct GameManifest
	{
		static constexpr std::string_view c_FileExtension = ".stgame";
		static constexpr uint32_t c_FormatVersion = 5;
		static constexpr size_t c_MaxPatchPacks = 256;

		// The project's name: it keys the game's log and save directories (SaveFiles::MakeGameDirectoryName), so it stays when
		// the title or the company changes.
		std::string Name = "Game";
		// The game's identity (ProjectGameInfo): what its window, log and dialogs show; checked like the project's. Manifests
		// before version 5 have none: an empty Title shows Name, and their Version is empty (an empty Version names none in
		// any version, so that such a manifest saved again still loads).
		std::string Title;
		std::string Version = std::string(ProjectGameInfo::c_DefaultVersion);
		std::string Company;
		std::string Copyright;
		// The game's icon, a 256 x 256 PNG next to the manifest (the export makes it from the project's icon); empty: none.
		std::string Icon;
		std::string AssetPack;                   // File name relative to the manifest's directory
		// Packs mounted over AssetPack in this order (RuntimeAssetManager::Mount): their entries replace the entries with the
		// same handles (patches, downloadable content). File names relative to the manifest's directory; optional.
		std::vector<std::string> PatchPacks;
		AssetHandle StartScene = UUID::Null();
		// A scene of the pack the runtime plays while the start scene loads (GameRuntime); null: a cleared frame instead.
		AssetHandle LoadingScene = UUID::Null();
		std::string ScriptModule;                // File name relative to the manifest's directory; empty without scripts
		uint32_t WindowWidth = 1280;
		uint32_t WindowHeight = 720;
		bool Fullscreen = false;
		bool VSync = true;
		// The project's settings sections (the project file's "Settings"), kept as stored: the runtime reads the manifest
		// before the engine's modules register their sections, so GameRuntime validates them later. At most
		// ProjectSettingsValues::c_MaxStoredBytes. nullopt for manifests before version 5, whose games provide no project
		// settings.
		std::optional<ProjectSettingsValues> Settings = ProjectSettingsValues();

		// The title the game shows: Title, or Name without one.
		const std::string& GetDisplayTitle() const { return Title.empty() ? Name : Title; }

		nlohmann::json ToJson() const;
		static std::optional<GameManifest> FromJson(const nlohmann::json& json, std::string* outError = nullptr);

		bool Save(const std::filesystem::path& path, std::string* outError = nullptr) const;
		static std::optional<GameManifest> Load(const std::filesystem::path& path, std::string* outError = nullptr);
		// The manifest a runtime executable starts: "<executable stem>.stgame" next to it, otherwise the only
		// ".stgame" file in its directory. Empty when there is none (or several).
		static std::filesystem::path FindForExecutable(const std::filesystem::path& executablePath);
	};

}
