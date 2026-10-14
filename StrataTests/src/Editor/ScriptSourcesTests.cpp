#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ScriptSources.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	std::vector<std::string> GetPaths(const ScriptSourceListing& listing)
	{
		std::vector<std::string> paths;
		for (const ScriptSourceFile& file : listing.Files)
			paths.push_back(file.Path);
		return paths;
	}

	const ScriptSourceFile* FindFile(const ScriptSourceListing& listing, std::string_view path)
	{
		for (const ScriptSourceFile& file : listing.Files)
		{
			if (file.Path == path)
				return &file;
		}
		return nullptr;
	}

	// Polls the list until its listing arrived (10 s at most); true when the files changed.
	bool WaitForListing(ScriptSourceList& list)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (list.IsListing() && std::chrono::steady_clock::now() < deadline)
		{
			if (list.Update())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		REQUIRE_FALSE(list.IsListing());
		return false;
	}

}

TEST_SUITE("Editor.ScriptSources")
{
	TEST_CASE("Script classes are found where ST_SCRIPT_CLASS registers them, not in comments, strings or the macro's definition")
	{
		const std::string code = R"CPP(#include "StrataScript/StrataScript.h"
#define ST_SCRIPT_CLASS(Name) something \
	ST_SCRIPT_CLASS(Continued)
// ST_SCRIPT_CLASS(LineComment)
/* ST_SCRIPT_CLASS(BlockComment)
   ST_SCRIPT_CLASS(StillComment) */
const char* text = "ST_SCRIPT_CLASS(InString)";
const char quote = '"';
class Player : public Script {};
ST_SCRIPT_CLASS(Player)
{
	ST_SCRIPT_FIELD(Speed);
}
ST_SCRIPT_CLASS ( Spaced )
{
}
MY_ST_SCRIPT_CLASS(Prefixed)
ST_SCRIPT_CLASS(9Digit)
ST_SCRIPT_CLASS(Unclosed
)CPP";
		const std::vector<std::string> classes = FindScriptClassRegistrations(code);
		CHECK(classes == std::vector<std::string> { "Player", "Spaced" });
		CHECK(FindScriptClassRegistrations("").empty());
		CHECK(FindScriptClassRegistrations("ST_SCRIPT_CLASS").empty());
		CHECK(FindScriptClassRegistrations("\"unterminated string\nST_SCRIPT_CLASS(AfterString)") == std::vector<std::string> { "AfterString" });
		// An unterminated literal ends with its line: the next line may be a preprocessor line again.
		CHECK(FindScriptClassRegistrations("\"unterminated\n#define ST_SCRIPT_CLASS(Parameter)\nST_SCRIPT_CLASS(Real)") == std::vector<std::string> { "Real" });

		// Raw string literals span lines and hold quotes; nothing in them registers a class.
		const std::string raw = "const char* help = R\"HELP(Write \"ST_SCRIPT_CLASS(Quoted)\"\nST_SCRIPT_CLASS(InRaw)\n)\" )HELP\";\n"
			"auto wide = LR\"(ST_SCRIPT_CLASS(InWide))\";\n"
			"auto utf8 = u8R\"x(\nST_SCRIPT_CLASS(InUtf8))x\";\n"
			"ST_SCRIPT_CLASS(AfterRaw)\n";
		CHECK(FindScriptClassRegistrations(raw) == std::vector<std::string> { "AfterRaw" });
		// A quote after an identifier that is no raw string prefix is an ordinary literal; an R"( never closed ends the text.
		CHECK(FindScriptClassRegistrations("Q\"ST_SCRIPT_CLASS(InString)\"\nST_SCRIPT_CLASS(Next)") == std::vector<std::string> { "Next" });
		CHECK(FindScriptClassRegistrations("R\"(open\nST_SCRIPT_CLASS(Swallowed)").empty());
	}

	TEST_CASE("Script code and source files are told by their extension, ignoring case")
	{
		for (const char* name : { "Player.cpp", "Player.CPP", "util.c", "Types.h", "Types.hpp", "Detail.inl", "Detail.ipp", "x.cc", "x.cxx", "x.hh", "x.hxx" })
		{
			CAPTURE(name);
			CHECK(IsScriptCodeFileName(name));
			CHECK(IsScriptSourceFileName(name));
		}
		for (const char* name : { "CMakeLists.txt", "Helpers.cmake", "Helpers.CMake" })
		{
			CAPTURE(name);
			CHECK_FALSE(IsScriptCodeFileName(name));
			CHECK(IsScriptSourceFileName(name));
		}
		for (const char* name : { "Notes.md", "CMakePresets.json", "cpp", "Player.cpp.bak", "", "Readme.txt" })
		{
			CAPTURE(name);
			CHECK_FALSE(IsScriptCodeFileName(name));
			CHECK_FALSE(IsScriptSourceFileName(name));
		}
	}

	TEST_CASE("Script sources are the code and CMake files of the script directory, without hidden folders and build trees")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("ScriptSources") / "Scripts";
		REQUIRE(FileSystem::CreateDirectories(directory / "Gameplay" / "AI"));
		REQUIRE(FileSystem::WriteText(directory / "CMakeLists.txt", "project(Game CXX)\n"));
		REQUIRE(FileSystem::WriteText(directory / "Player.cpp", "ST_SCRIPT_CLASS(Player)\n{\n}\nST_SCRIPT_CLASS(Camera)\n{\n}\n"));
		REQUIRE(FileSystem::WriteText(directory / "Gameplay" / "enemy.h", "struct EnemyState {};\n"));
		REQUIRE(FileSystem::WriteText(directory / "Gameplay" / "AI" / "Brain.cpp", "ST_SCRIPT_CLASS(Brain)\n"));
		REQUIRE(FileSystem::WriteText(directory / "Gameplay" / "Helpers.cmake", "# helpers\n"));
		REQUIRE(FileSystem::WriteText(directory / "Notes.md", "Not a source\n"));
		REQUIRE(FileSystem::WriteText(directory / "CMakePresets.json", "{}\n"));
		// An IDE's settings and a build tree an IDE configured inside the scripts.
		REQUIRE(FileSystem::CreateDirectories(directory / ".vscode"));
		REQUIRE(FileSystem::WriteText(directory / ".vscode" / "Hidden.cpp", "ST_SCRIPT_CLASS(Hidden)\n"));
		REQUIRE(FileSystem::CreateDirectories(directory / "out" / "build" / "CMakeFiles"));
		REQUIRE(FileSystem::WriteText(directory / "out" / "build" / "CMakeCache.txt", "# CMake cache\n"));
		REQUIRE(FileSystem::WriteText(directory / "out" / "build" / "CMakeFiles" / "Generated.cpp", "int main() {}\n"));

		const ScriptSourceListing listing = ListScriptSources(directory);
		CHECK_FALSE(listing.Truncated);
		// Sorted by path ignoring case, with '/' separators.
		CHECK(GetPaths(listing) == std::vector<std::string> { "CMakeLists.txt", "Gameplay/AI/Brain.cpp", "Gameplay/enemy.h", "Gameplay/Helpers.cmake",
			"Player.cpp" });
		const ScriptSourceFile* player = FindFile(listing, "Player.cpp");
		REQUIRE(player);
		CHECK(player->Classes == std::vector<std::string> { "Player", "Camera" });
		CHECK(player->Size == FileSystem::GetFileSize(directory / "Player.cpp").value_or(0));
		CHECK(player->LastWriteTime != 0);
		CHECK(FindFile(listing, "Gameplay/AI/Brain.cpp")->Classes == std::vector<std::string> { "Brain" });
		CHECK(FindFile(listing, "Gameplay/enemy.h")->Classes.empty());
		CHECK(FindFile(listing, "CMakeLists.txt")->Classes.empty()); // CMake files are not read for classes

		// A listing again reads only files that changed: an unchanged file keeps what the earlier listing found.
		ScriptSourceListing previous = listing;
		for (ScriptSourceFile& file : previous.Files)
		{
			if (file.Path == "Player.cpp")
				file.Classes = { "Lent" };
		}
		REQUIRE(FileSystem::WriteText(directory / "Gameplay" / "AI" / "Brain.cpp", "ST_SCRIPT_CLASS(Brain)\nST_SCRIPT_CLASS(Planner)\n"));
		const ScriptSourceListing again = ListScriptSources(directory, &previous);
		CHECK(FindFile(again, "Player.cpp")->Classes == std::vector<std::string> { "Lent" });
		CHECK(FindFile(again, "Gameplay/AI/Brain.cpp")->Classes == std::vector<std::string> { "Brain", "Planner" });

		// No directory, no files.
		const ScriptSourceListing missing = ListScriptSources(directory / "Missing");
		CHECK(missing.Files.empty());
		CHECK_FALSE(missing.Truncated);
	}

	TEST_CASE("A listing whose walk ends on an error says that it is incomplete")
	{
		// A file where the directory should be: the walk fails at once (as one does on a folder removed or replaced while it
		// walks), so the files are unknown rather than none.
		const std::filesystem::path directory = CreateTemporaryDirectory("ScriptSourcesError") / "Scripts";
		REQUIRE(FileSystem::WriteText(directory, "Not a folder\n"));
		const ScriptSourceListing listed = ListScriptSources(directory);
		CHECK(listed.Files.empty());
		CHECK(listed.Truncated);
		const ScriptSourceListing stamped = StampScriptSources(directory);
		CHECK(stamped.Files.empty());
		CHECK(stamped.Truncated);
	}

	TEST_CASE("A script directory with more files or entries than a listing looks at is listed in part, and says so")
	{
		const std::filesystem::path directory = CreateTemporaryDirectory("ScriptSourcesMany") / "Scripts";
		REQUIRE(FileSystem::CreateDirectories(directory / "Assets"));
		for (int index = 0; index < 6; index++)
			REQUIRE(FileSystem::WriteText(directory / fmt::format("Generated{}.h", index), ""));
		for (int index = 0; index < 6; index++)
			REQUIRE(FileSystem::WriteText(directory / "Assets" / fmt::format("Picture{}.png", index), ""));

		const ScriptSourceListing all = ListScriptSources(directory, nullptr, 6, 13);
		CHECK_FALSE(all.Truncated);
		CHECK(all.Files.size() == 6);
		const ScriptSourceListing fewerFiles = ListScriptSources(directory, nullptr, 4, 13);
		CHECK(fewerFiles.Truncated);
		CHECK(fewerFiles.Files.size() == 4);
		// Entries that are no sources count too (the folder and its pictures): the walk ends before it saw everything.
		const ScriptSourceListing fewerEntries = ListScriptSources(directory, nullptr, 6, 12);
		CHECK(fewerEntries.Truncated);
		CHECK(fewerEntries.Files.size() <= 6);
	}

	TEST_CASE("The script source list lists on an I/O thread and changes its version only when the files changed")
	{
		const std::filesystem::path root = CreateTemporaryDirectory("ScriptSourceList");
		const std::filesystem::path directory = root / "Scripts";
		REQUIRE(FileSystem::CreateDirectories(directory));
		REQUIRE(FileSystem::WriteText(directory / "Mover.cpp", "ST_SCRIPT_CLASS(Mover)\n"));

		ScriptSourceList list;
		CHECK_FALSE(list.HasListed());
		const uint64_t initial = list.GetVersion();
		list.Refresh(directory);
		CHECK(list.GetDirectory() == directory);
		CHECK(WaitForListing(list));
		CHECK(list.HasListed());
		CHECK(GetPaths(list.GetListing()) == std::vector<std::string> { "Mover.cpp" });
		const uint64_t listed = list.GetVersion();
		CHECK(listed != initial);

		// Nothing changed: the same version.
		list.Refresh(directory);
		CHECK_FALSE(WaitForListing(list));
		CHECK(list.GetVersion() == listed);

		// A new file: a new version. A refresh asked for while one runs runs after it.
		REQUIRE(FileSystem::WriteText(directory / "Spawner.cpp", "ST_SCRIPT_CLASS(Spawner)\n"));
		list.Refresh(directory);
		list.Refresh(directory);
		WaitForListing(list);
		WaitForListing(list);
		CHECK(list.GetVersion() != listed);
		CHECK(GetPaths(list.GetListing()) == std::vector<std::string> { "Mover.cpp", "Spawner.cpp" });

		// Another directory forgets the files of the first; Clear forgets everything.
		const std::filesystem::path other = root / "Other";
		REQUIRE(FileSystem::CreateDirectories(other));
		list.Refresh(other);
		CHECK_FALSE(list.HasListed());
		CHECK(list.GetListing().Files.empty());
		WaitForListing(list);
		CHECK(list.HasListed());
		CHECK(list.GetListing().Files.empty());
		list.Clear();
		CHECK(list.GetDirectory().empty());
		CHECK_FALSE(list.HasListed());
		CHECK_FALSE(list.IsListing());
	}

	TEST_CASE("script.list names the project's script sources with the classes each registers")
	{
		EditorContext context(EditorContextSpecification { false, false });
		EditorCommandRegistry commands;
		CHECK(commands.Execute(context, "script.list", nlohmann::json::object()).ErrorKind == EditorCommandError::Failed);

		const std::filesystem::path directory = CreateTemporaryDirectory("ScriptList") / "Listed";
		REQUIRE(commands.Execute(context, "project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Listed" } }).Success);
		REQUIRE(commands.Execute(context, "script.create", { { "class", "Opener" }, { "path", "Doors/Opener.cpp" } }).Success);
		const EditorCommandResult result = commands.Execute(context, "script.list", nlohmann::json::object());
		REQUIRE(result.Success);
		CHECK(FileSystem::FromUTF8(result.Value["sourceDirectory"].get<std::string>()) == context.GetProject()->GetScriptSourceDirectory());
		CHECK(result.Value["truncated"] == false);
		std::vector<std::string> paths;
		for (const nlohmann::json& file : result.Value["files"])
		{
			paths.push_back(file["path"].get<std::string>());
			if (file["path"] == "Doors/Opener.cpp")
			{
				CHECK(file["projectPath"] == "Scripts/Doors/Opener.cpp");
				CHECK(file["classes"] == nlohmann::json::array({ "Opener" }));
				CHECK(file["bytes"].get<uint64_t>() > 0);
			}
			if (file["path"] == "Spinner.cpp")
				CHECK(file["classes"] == nlohmann::json::array({ "Spinner" }));
		}
		CHECK(paths == std::vector<std::string> { "CMakeLists.txt", "Doors/Opener.cpp", "Spinner.cpp" });
	}
}
