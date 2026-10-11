#include <doctest/doctest.h>

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/SaveFiles.h"
#include "TestHelpers.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	std::span<const uint8_t> AsBytes(std::string_view text)
	{
		return { reinterpret_cast<const uint8_t*>(text.data()), text.size() };
	}

	std::string AsText(const std::vector<uint8_t>& bytes)
	{
		return std::string(bytes.begin(), bytes.end());
	}

	struct FixedSaves final : public IGameSaves
	{
		explicit FixedSaves(std::filesystem::path directory)
			: Directory(std::move(directory))
		{
		}

		std::optional<std::filesystem::path> GetSaveDirectory() override { return Directory; }

		std::filesystem::path Directory;
	};

}

TEST_SUITE("Core.SaveFiles")
{
	TEST_CASE("Save file names are checked before any path is formed")
	{
		CHECK_FALSE(SaveFiles::ValidateName("Slot1"));
		CHECK_FALSE(SaveFiles::ValidateName("slot 1"));
		CHECK_FALSE(SaveFiles::ValidateName("profile.v2"));
		CHECK_FALSE(SaveFiles::ValidateName("high-scores_2"));
		CHECK_FALSE(SaveFiles::ValidateName(std::string(SaveFiles::c_MaxNameLength, 'a')));

		CHECK(SaveFiles::ValidateName(""));
		CHECK(SaveFiles::ValidateName(std::string(SaveFiles::c_MaxNameLength + 1, 'a')));
		CHECK(SaveFiles::ValidateName("a/b"));
		CHECK(SaveFiles::ValidateName("a\\b"));
		CHECK(SaveFiles::ValidateName(".."));
		CHECK(SaveFiles::ValidateName("../escape"));
		CHECK(SaveFiles::ValidateName("a..b"));
		CHECK(SaveFiles::ValidateName("C:save"));
		CHECK(SaveFiles::ValidateName(".hidden"));
		CHECK(SaveFiles::ValidateName("trailing."));
		CHECK(SaveFiles::ValidateName(" padded"));
		CHECK(SaveFiles::ValidateName("tab\there"));
		CHECK(SaveFiles::ValidateName("star*"));
		CHECK(SaveFiles::ValidateName("NUL"));
		CHECK(SaveFiles::ValidateName("con.txt"));
		CHECK(SaveFiles::ValidateName("Com1"));
		CHECK(SaveFiles::ValidateName("lpt9.save"));
		CHECK_FALSE(SaveFiles::ValidateName("Console"));
		CHECK_FALSE(SaveFiles::ValidateName("COM10"));
		// Non-ASCII names are refused rather than normalized differently per file system.
		CHECK(SaveFiles::ValidateName("Spielstand\xC3\xA4"));

		const std::optional<std::string> separator = SaveFiles::ValidateName("a/b");
		REQUIRE(separator);
		CHECK(separator->find("separator") != std::string::npos);
		const std::optional<std::string> dots = SaveFiles::ValidateName("a..b");
		REQUIRE(dots);
		CHECK(dots->find("..") != std::string::npos);
	}

	TEST_CASE("Save files round-trip, list, replace and delete")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("SaveFiles") / "Saves";
		CHECK(SaveFiles::List(directory).empty()); // Not created yet

		std::string error;
		REQUIRE(SaveFiles::Write(directory, "Slot 2", AsBytes("second"), &error));
		REQUIRE(SaveFiles::Write(directory, "Slot1", AsBytes("first"), &error));
		CHECK(FileSystem::IsRegularFile(directory / "Slot1.save"));
		CHECK(SaveFiles::Exists(directory, "Slot1"));
		CHECK_FALSE(SaveFiles::Exists(directory, "Slot3"));
		CHECK_FALSE(SaveFiles::Exists(directory, "../Slot1"));

		std::optional<std::vector<uint8_t>> read = SaveFiles::Read(directory, "Slot1", &error);
		REQUIRE(read);
		CHECK(AsText(*read) == "first");
		CHECK(SaveFiles::List(directory) == std::vector<std::string> { "Slot 2", "Slot1" });

		// Writing again replaces the content as a whole; empty files are saves too.
		REQUIRE(SaveFiles::Write(directory, "Slot1", AsBytes("replaced"), &error));
		read = SaveFiles::Read(directory, "Slot1", &error);
		REQUIRE(read);
		CHECK(AsText(*read) == "replaced");
		REQUIRE(SaveFiles::Write(directory, "Empty", {}, &error));
		read = SaveFiles::Read(directory, "Empty", &error);
		REQUIRE(read);
		CHECK(read->empty());

		// Files that are not saves of valid names are not listed.
		REQUIRE(FileSystem::WriteBytes(directory / "notes.txt", AsBytes("x")));
		REQUIRE(FileSystem::WriteBytes(directory / ".hidden.save", AsBytes("x")));
		REQUIRE(FileSystem::CreateDirectories(directory / "Folder.save"));
		CHECK(SaveFiles::List(directory) == std::vector<std::string> { "Empty", "Slot 2", "Slot1" });

		REQUIRE(SaveFiles::Delete(directory, "Slot 2", &error));
		CHECK_FALSE(SaveFiles::Exists(directory, "Slot 2"));
		CHECK_FALSE(SaveFiles::Delete(directory, "Slot 2", &error));
		CHECK(error.find("no save file") != std::string::npos);
		CHECK_FALSE(SaveFiles::Read(directory, "Slot 2", &error));
		CHECK(error.find("no save file") != std::string::npos);
		CHECK_FALSE(SaveFiles::Read(directory, "Folder", &error)); // A directory is no save file
	}

	TEST_CASE("Invalid names write, read and delete nothing outside the directory")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("SaveFilesEscape");
		const std::filesystem::path directory = root / "Saves";
		REQUIRE(FileSystem::WriteBytes(root / "Outside.save", AsBytes("keep")));

		std::string error;
		CHECK_FALSE(SaveFiles::Write(directory, "../Outside", AsBytes("overwritten"), &error));
		CHECK(error.find("separator") != std::string::npos);
		CHECK_FALSE(SaveFiles::Write(directory, "..", AsBytes("x"), &error));
		CHECK_FALSE(SaveFiles::Read(directory, "../Outside", &error));
		CHECK_FALSE(SaveFiles::Delete(directory, "..\\Outside", &error));
		CHECK_FALSE(FileSystem::Exists(directory)); // Nothing was created for refused names
		const std::optional<std::vector<uint8_t>> kept = FileSystem::ReadBytes(root / "Outside.save");
		REQUIRE(kept);
		CHECK(AsText(*kept) == "keep");
	}

	TEST_CASE("Game names become directory names that stay in the user data directory")
	{
		CHECK(SaveFiles::MakeGameDirectoryName("Tetris") == "Tetris");
		CHECK(SaveFiles::MakeGameDirectoryName("My Game 2") == "My Game 2");
		CHECK(SaveFiles::MakeGameDirectoryName("../../Windows") == "______Windows");
		CHECK(SaveFiles::MakeGameDirectoryName("a/b\\c") == "a_b_c");
		CHECK(SaveFiles::MakeGameDirectoryName("Part 2: Return?") == "Part 2_ Return_");
		CHECK(SaveFiles::MakeGameDirectoryName("  .Dots.  ") == "Dots");
		CHECK(SaveFiles::MakeGameDirectoryName("") == "Game");
		CHECK(SaveFiles::MakeGameDirectoryName(" . ") == "Game");
		CHECK(SaveFiles::MakeGameDirectoryName("NUL") == "_NUL");
		CHECK(SaveFiles::MakeGameDirectoryName("line\nbreak") == "line_break");
		CHECK(SaveFiles::MakeGameDirectoryName("Spiel \xC3\xA4") == "Spiel \xC3\xA4");

		// Long names are cut before a character that would not fit whole.
		const std::string longName = std::string(SaveFiles::c_MaxNameLength - 1, 'a') + "\xC3\xA4" + "tail";
		CHECK(SaveFiles::MakeGameDirectoryName(longName) == std::string(SaveFiles::c_MaxNameLength - 1, 'a'));
		CHECK(SaveFiles::MakeGameDirectoryName(std::string(100, 'b')).size() == SaveFiles::c_MaxNameLength);
	}

	TEST_CASE("A save directory finds a game's directory on first use")
	{
		SaveDirectory none;
		CHECK_FALSE(none.GetSaveDirectory());

		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("SaveDirectory");
		SaveDirectory fixed(directory);
		REQUIRE(fixed.GetSaveDirectory());
		CHECK(*fixed.GetSaveDirectory() == directory);

		Scope<SaveDirectory> game = SaveDirectory::ForGame("Strata Test ../Saves");
		const std::optional<std::filesystem::path> gameDirectory = game->GetSaveDirectory();
		REQUIRE(gameDirectory);
		CHECK(gameDirectory->filename() == "Saves");
		CHECK(FileSystem::ToUTF8(gameDirectory->parent_path().filename()) == "Strata Test ___Saves");
		CHECK_FALSE(FileSystem::Exists(*gameDirectory)); // Created by the first save only
		std::error_code ignored;
		std::filesystem::remove(gameDirectory->parent_path(), ignored); // GetUserDataDirectory created the game's directory

		game->SetDirectory(directory / "Elsewhere");
		REQUIRE(game->GetSaveDirectory());
		CHECK(*game->GetSaveDirectory() == directory / "Elsewhere");
	}
}

TEST_SUITE("Core.EngineServices")
{
	TEST_CASE("Scoped providers nest and restore the provider they replaced")
	{
		IGameSaves* before = EngineServices::Get<IGameSaves>();
		FixedSaves outer("Outer");
		FixedSaves inner("Inner");
		{
			EngineServices::ScopedProvider<IGameSaves> outerScope(outer);
			CHECK(EngineServices::Get<IGameSaves>() == &outer);
			{
				EngineServices::ScopedProvider<IGameSaves> innerScope(inner);
				CHECK(EngineServices::Get<IGameSaves>() == &inner);
			}
			CHECK(EngineServices::Get<IGameSaves>() == &outer);
		}
		CHECK(EngineServices::Get<IGameSaves>() == before);
	}
}
