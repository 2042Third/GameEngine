#pragma once

#include <filesystem>
#include <string>

namespace Strata
{

	class EditorContext;

	struct GameExportOptions
	{
		std::filesystem::path Directory;         // Output directory (created if needed)
		std::filesystem::path RuntimeExecutable; // Empty: StrataRuntime next to the editor executable
		bool IncludeRuntime = true;              // Copy the runtime executable (as "<Game>[.exe]")
		uint32_t WindowWidth = 1280;
		uint32_t WindowHeight = 720;
		bool Fullscreen = false;
	};

	struct GameExportResult
	{
		std::filesystem::path Executable; // Empty without the runtime
		std::filesystem::path Manifest;
		std::filesystem::path AssetPack;
		size_t AssetCount = 0;
	};

	// Exports the open project as a playable game: an asset pack with every project asset, a game manifest pointing
	// at the project's start scene (or the open scene when none is set), and the runtime executable with the
	// third-party notices found next to it. The scene must be saved and not running.
	bool ExportGame(EditorContext& context, const GameExportOptions& options, GameExportResult& outResult, std::string* outError = nullptr);

	// Platform file name of an executable ("Name.exe" on Windows, "Name" elsewhere).
	std::string GetExecutableFileName(const std::string& name);

}
