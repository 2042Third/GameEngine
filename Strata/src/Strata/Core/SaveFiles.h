#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/EngineServices.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Save files of a game: named byte blobs in the game's save directory (IGameSaves, Core/EngineServices.h). Scripts name
// them; the names are checked here, in the engine, before any path is formed: 1 to c_MaxNameLength characters of ASCII
// letters, digits, ' ', '-', '_' and '.', not starting or ending with a space or a dot, without "..", and not one of the
// device names Windows reserves (CON, NUL, COM1, ...; also before a dot). A file is stored as "<name>.save" and written
// atomically (a reader sees the old or the new content, never a part), at most c_MaxFileSize bytes. The calls block on the
// file system: keep saves small (they run where the script calls them, on the main thread).
namespace Strata::SaveFiles
{

	constexpr size_t c_MaxNameLength = 64;
	constexpr uint64_t c_MaxFileSize = 64ull * 1024 * 1024;

	// Why `name` cannot name a save file, or nullopt when it can.
	std::optional<std::string> ValidateName(std::string_view name);
	// Whether the directory holds a save file of that (valid) name.
	bool Exists(const std::filesystem::path& directory, std::string_view name);

	// Each returns false (nullopt) with the reason in outError: an invalid name, a file too large, a missing file (Read,
	// Delete), or what the file system refused.
	bool Write(const std::filesystem::path& directory, std::string_view name, std::span<const uint8_t> bytes, std::string* outError = nullptr);
	std::optional<std::vector<uint8_t>> Read(const std::filesystem::path& directory, std::string_view name, std::string* outError = nullptr);
	bool Delete(const std::filesystem::path& directory, std::string_view name, std::string* outError = nullptr);
	// The names of the save files in the directory, sorted (empty when it does not exist).
	std::vector<std::string> List(const std::filesystem::path& directory);

	// The directory name a game's data goes under (SaveDirectory::ForGame): the game's name with what cannot name a directory
	// everywhere replaced by '_' (path separators, "..", characters Windows refuses, control characters), leading and
	// trailing spaces and dots removed, at most c_MaxNameLength bytes (whole UTF-8 characters), a device name prefixed with
	// '_', and "Game" for a name that leaves nothing.
	std::string MakeGameDirectoryName(std::string_view gameName);

}

namespace Strata
{

	// A game's save files in one directory (IGameSaves): what the program running a game provides while it runs it.
	class SaveDirectory final : public IGameSaves
	{
	public:
		// Saves in `directory` (empty: none).
		explicit SaveDirectory(std::filesystem::path directory = {});
		// Saves of a shipped game: <user data>/<game directory>/Saves (Platform::GetUserDataDirectory, the name made by
		// SaveFiles::MakeGameDirectoryName), found on first use, so that a game that never saves creates nothing.
		static Scope<SaveDirectory> ForGame(std::string gameName);

		void SetDirectory(std::filesystem::path directory);
		std::optional<std::filesystem::path> GetSaveDirectory() override;
	private:
		std::filesystem::path m_Directory;
		std::string m_GameName; // Resolved into m_Directory on first use (ForGame)
	};

}
