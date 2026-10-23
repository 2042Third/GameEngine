#pragma once

#include "Strata/Core/Window.h"
#include "Strata/Project/GameManifest.h"

#include <filesystem>
#include <string>

namespace Strata
{

	// How StrataRuntime presents the game of a manifest, decided from the manifest alone before anything starts (the runtime's
	// CreateApplication): what its window, log and dialogs show, and where its data goes.
	struct GamePresentation
	{
		std::string Title;  // The window's title and the name dialogs show: the manifest's title, else its name
		std::string Banner; // The log's first line: title, version (when the manifest has one), engine version and platform
		// The directory under the player's data the game's log and saves go to: keyed by the manifest's Name, never its title
		// or company, so that an update that renames the game keeps its players' saves (SaveFiles::MakeGameDirectoryName,
		// as SaveDirectory::ForGame resolves it).
		std::string DataDirectoryName;
		std::filesystem::path IconFile; // The manifest's icon file next to it; empty without one
		std::string AppId;              // The executable's file name without extension (WindowSpecification::AppId)

		// manifestPath: where the manifest was read from; executablePath: the running executable (the exported game).
		static GamePresentation Make(const GameManifest& manifest, const std::filesystem::path& manifestPath, const std::filesystem::path& executablePath);

		// Sets the window's title, icon and application id.
		void ApplyTo(WindowSpecification& window) const;
	};

}
