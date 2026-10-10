#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/RecentProjects.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Version.h>
#include <Strata/Project/Project.h>

#include <chrono>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// A project file on disk (RecentProjects only needs the file to exist).
	std::filesystem::path CreateProjectFile(const std::filesystem::path& directory, const std::string& name)
	{
		const std::filesystem::path file = directory / FileSystem::FromUTF8(name + ".stproj");
		REQUIRE(FileSystem::CreateDirectories(directory));
		REQUIRE(FileSystem::WriteText(file, "{}"));
		return file;
	}

	size_t CountWarningsSince(uint64_t sequence)
	{
		size_t warnings = 0;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(sequence))
		{
			if (entry.Level == LogLevel::Warn)
				warnings++;
		}
		return warnings;
	}

	bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b)
	{
		std::error_code error;
		return std::filesystem::equivalent(a, b, error);
	}

}

TEST_SUITE("Editor.RecentProjects")
{
	TEST_CASE("The list round-trips through its file, most recent first")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecentRoundTrip");
		const std::filesystem::path file = directory / "Data" / "RecentProjects.json";
		const std::filesystem::path alpha = CreateProjectFile(directory / "Alpha", "Alpha");
		const std::filesystem::path beta = CreateProjectFile(directory / "Beta", "Beta");

		const int64_t before = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		{
			RecentProjects recent(file);
			CHECK(recent.GetProjects().empty()); // No file yet
			recent.Add("Alpha", alpha);
			recent.Add("Beta", beta);
			REQUIRE(recent.GetProjects().size() == 2);
			CHECK(recent.GetProjects()[0].Name == "Beta");
			CHECK(recent.GetProjects()[1].Name == "Alpha");
			CHECK(recent.GetProjects()[0].EngineVersion == c_EngineVersion);
			CHECK(recent.GetProjects()[0].LastOpened >= before);
		}
		REQUIRE(FileSystem::IsRegularFile(file));

		RecentProjects reloaded(file);
		REQUIRE(reloaded.GetProjects().size() == 2);
		CHECK(reloaded.GetProjects()[0].Name == "Beta");
		CHECK(SamePath(reloaded.GetProjects()[0].Path, beta));
		CHECK(SamePath(reloaded.GetProjects()[1].Path, alpha));

		// Opening a project again moves it to the front, also when its path is spelled differently.
		reloaded.Add("Alpha", directory / "Beta" / ".." / "Alpha" / "Alpha.stproj");
		REQUIRE(reloaded.GetProjects().size() == 2);
		CHECK(reloaded.GetProjects()[0].Name == "Alpha");
		CHECK(reloaded.GetProjects()[1].Name == "Beta");

		// The JSON form round-trips exactly.
		const std::vector<RecentProject> projects = reloaded.GetProjects();
		const std::optional<std::vector<RecentProject>> parsed = RecentProjects::FromJson(RecentProjects::ToJson(projects));
		REQUIRE(parsed);
		CHECK(*parsed == projects);

		// Another editor's additions are kept: the file is read again before each change.
		const std::filesystem::path gamma = CreateProjectFile(directory / "Gamma", "Gamma");
		RecentProjects other(file);
		other.Add("Gamma", gamma);
		const std::filesystem::path delta = CreateProjectFile(directory / "Delta", "Delta");
		reloaded.Add("Delta", delta);
		REQUIRE(reloaded.GetProjects().size() == 4);
		CHECK(reloaded.GetProjects()[0].Name == "Delta");
		CHECK(reloaded.GetProjects()[1].Name == "Gamma");
	}

	TEST_CASE("At most twelve projects are kept, the most recent first")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecentCap");
		RecentProjects recent(directory / "RecentProjects.json");
		for (int index = 0; index < 15; index++)
			recent.Add("Project" + std::to_string(index), CreateProjectFile(directory / std::to_string(index), "Project" + std::to_string(index)));
		const std::vector<RecentProject>& projects = recent.GetProjects();
		REQUIRE(projects.size() == RecentProjects::c_MaxProjects);
		CHECK(projects.size() == 12);
		for (size_t index = 0; index < projects.size(); index++)
			CHECK(projects[index].Name == "Project" + std::to_string(14 - index));

		// A file holding more (written by hand, say) is cut to the cap when read.
		std::vector<RecentProject> many(20, projects.front());
		const std::optional<std::vector<RecentProject>> parsed = RecentProjects::FromJson(RecentProjects::ToJson(many));
		REQUIRE(parsed);
		CHECK(parsed->size() == RecentProjects::c_MaxProjects);
	}

	TEST_CASE("Projects that no longer exist are left out")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecentPrune");
		const std::filesystem::path file = directory / "RecentProjects.json";
		const std::filesystem::path kept = CreateProjectFile(directory / "Kept", "Kept");
		const std::filesystem::path gone = CreateProjectFile(directory / "Gone", "Gone");
		RecentProjects recent(file);
		recent.Add("Kept", kept);
		recent.Add("Gone", gone);
		REQUIRE(recent.GetProjects().size() == 2);

		REQUIRE(FileSystem::Remove(directory / "Gone"));
		REQUIRE(recent.GetProjects().size() == 1);
		CHECK(recent.GetProjects()[0].Name == "Kept");
		// Also when read from the file.
		RecentProjects reloaded(file);
		REQUIRE(reloaded.GetProjects().size() == 1);
		CHECK(reloaded.GetProjects()[0].Name == "Kept");
	}

	TEST_CASE("Paths with characters beyond ASCII survive the list")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecentUnicode");
		const std::string name = "Jeu \xC3\xA9t\xC3\xA9 \xE6\xB8\xB8\xE6\x88\x8F \xF0\x9F\x8E\xAE"; // "Jeu été 游戏 🎮"
		const std::filesystem::path projectDirectory = directory / FileSystem::FromUTF8(name);
		const std::filesystem::path project = CreateProjectFile(projectDirectory, name);
		const std::filesystem::path file = directory / FileSystem::FromUTF8("D\xC3\xA4ten") / "RecentProjects.json";
		{
			RecentProjects recent(file);
			recent.Add(name, project);
		}
		RecentProjects reloaded(file);
		REQUIRE(reloaded.GetProjects().size() == 1);
		CHECK(reloaded.GetProjects()[0].Name == name);
		CHECK(SamePath(reloaded.GetProjects()[0].Path, project));
		CHECK(FileSystem::ToUTF8(reloaded.GetProjects()[0].Path).find(name) != std::string::npos);
		// The file holds UTF-8 text.
		const std::optional<std::string> text = FileSystem::ReadText(file);
		REQUIRE(text);
		CHECK(text->find(name) != std::string::npos);
	}

	TEST_CASE("A corrupt list is ignored with exactly one warning and replaced by the next save")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecentCorrupt");
		const std::filesystem::path file = directory / "RecentProjects.json";
		const std::filesystem::path project = CreateProjectFile(directory / "Game", "Game");
		// Besides broken files: times of opening that are no time a project was opened (before 1970, after the year 9999,
		// beyond what 64 bits hold), which would overflow when the launcher tells how long ago they were.
		const auto withLastOpened = [&project](const std::string& lastOpened)
		{
			return "{ \"Strata\": { \"Format\": \"RecentProjects\", \"Version\": 1 }, \"Projects\": [ { \"name\": \"Game\", \"path\": "
				+ nlohmann::json(FileSystem::ToUTF8(project)).dump() + ", \"lastOpened\": " + lastOpened + " } ] }";
		};
		for (const std::string& corrupt : { std::string("{ not json"), std::string("{ \"Strata\": { \"Format\": \"Scene\", \"Version\": 1 } }"),
			std::string("{ \"Strata\": { \"Format\": \"RecentProjects\", \"Version\": 1 }, \"Projects\": [ { \"name\": 3 } ] }"),
			withLastOpened("-1"), withLastOpened("-9223372036854775808"), withLastOpened("253402300800"), withLastOpened("18446744073709551615") })
		{
			CAPTURE(corrupt);
			REQUIRE(FileSystem::WriteText(file, corrupt));
			const uint64_t sequence = Log::GetBuffer().GetLatestSequence();
			RecentProjects recent(file);
			CHECK(recent.GetProjects().empty());
			recent.Reload();
			recent.Add("Game", project); // Reads the file again before adding
			CHECK(CountWarningsSince(sequence) == 1);
			REQUIRE(recent.GetProjects().size() == 1);

			// The next save replaced it with a valid list.
			RecentProjects reloaded(file);
			REQUIRE(reloaded.GetProjects().size() == 1);
			CHECK(reloaded.GetProjects()[0].Name == "Game");
			CHECK(CountWarningsSince(sequence) == 1);
		}
	}

	TEST_CASE("Times of opening from the year 1970 to 9999 are valid")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecentTimes");
		const std::filesystem::path project = CreateProjectFile(directory / "Game", "Game");
		for (const int64_t lastOpened : { int64_t(0), RecentProjects::c_MaxLastOpened })
		{
			const std::vector<RecentProject> projects = { RecentProject { "Game", project, lastOpened, "1.0" } };
			const std::optional<std::vector<RecentProject>> parsed = RecentProjects::FromJson(RecentProjects::ToJson(projects));
			REQUIRE(parsed);
			CHECK(parsed->front().LastOpened == lastOpened);
		}
	}

	TEST_CASE("A read-only list keeps the projects of the session over the file it never writes")
	{
		// Scripted runs (--commands, --frames) read the user's list but never write it; what they open is listed all the
		// same, also after the file was read again.
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecentReadOnlySession");
		const std::filesystem::path file = directory / "RecentProjects.json";
		const std::filesystem::path saved = CreateProjectFile(directory / "Saved", "Saved");
		{
			RecentProjects writable(file);
			writable.Add("Saved", saved);
		}

		const std::filesystem::path alpha = CreateProjectFile(directory / "Alpha", "Alpha");
		const std::filesystem::path beta = CreateProjectFile(directory / "Beta", "Beta");
		RecentProjects readOnly(file, true);
		readOnly.Add("Alpha", alpha);
		readOnly.Add("Beta", beta);
		const auto names = [](const std::vector<RecentProject>& projects)
		{
			std::vector<std::string> result;
			for (const RecentProject& project : projects)
				result.push_back(project.Name);
			return result;
		};
		CHECK(names(readOnly.GetProjects()) == std::vector<std::string> { "Beta", "Alpha", "Saved" });
		readOnly.Reload();
		CHECK(names(readOnly.GetProjects()) == std::vector<std::string> { "Beta", "Alpha", "Saved" });

		// A project of the session can leave the list again; another editor's additions show up under the session's.
		CHECK(readOnly.Remove(alpha));
		const std::filesystem::path gamma = CreateProjectFile(directory / "Gamma", "Gamma");
		{
			RecentProjects other(file);
			other.Add("Gamma", gamma);
		}
		readOnly.Reload();
		CHECK(names(readOnly.GetProjects()) == std::vector<std::string> { "Beta", "Gamma", "Saved" });
		CHECK_FALSE(readOnly.Remove(alpha));

		// The file only holds what the writable lists wrote.
		const std::optional<std::vector<RecentProject>> stored = RecentProjects::ReadFile(file).Projects;
		REQUIRE(stored);
		CHECK(names(*stored) == std::vector<std::string> { "Gamma", "Saved" });
	}

	TEST_CASE("Changes are counted, reads are not")
	{
		// The launcher reads the file on an I/O thread and drops a read that started before a change (which it may miss).
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecentChanges");
		const std::filesystem::path project = CreateProjectFile(directory / "Game", "Game");
		RecentProjects recent(directory / "RecentProjects.json");
		CHECK(recent.GetChangeCount() == 0);
		recent.Add("Game", project);
		CHECK(recent.GetChangeCount() == 1);
		recent.Reload();
		recent.ApplyFile(RecentProjects::ReadFile(recent.GetFile()));
		CHECK(recent.GetChangeCount() == 1);
		CHECK_FALSE(recent.Remove(directory / "Other.stproj"));
		CHECK(recent.GetChangeCount() == 1);
		CHECK(recent.Remove(project));
		CHECK(recent.GetChangeCount() == 2);
		recent.Forget({ project });
		CHECK(recent.GetChangeCount() == 2); // Not on the list any more
	}

	TEST_CASE("Projects a list could not save stay on it until a save succeeds")
	{
		// The list's directory would be inside a file: it cannot be created.
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecentUnsaved");
		REQUIRE(FileSystem::WriteText(directory / "Blocker", "a file"));
		const std::filesystem::path project = CreateProjectFile(directory / "Game", "Game");
		RecentProjects recent(directory / "Blocker" / "RecentProjects.json");
		recent.Add("Game", project);
		recent.Reload();
		REQUIRE(recent.GetProjects().size() == 1);
		CHECK(recent.GetProjects()[0].Name == "Game");
	}

	TEST_CASE("Deleted projects are told apart from projects out of reach")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecentCheck");
		const std::filesystem::path present = CreateProjectFile(directory / "Present", "Present");
		const std::filesystem::path deleted = directory / "Deleted" / "Deleted.stproj";
		// Its place (the folder that held its folder) is not there either: a drive or share that is not connected.
		const std::filesystem::path unreachable = directory / "Offline" / "Game" / "Game.stproj";
		const RecentProjectsCheck check = RecentProjects::CheckProjects({ present, deleted, unreachable });
		CHECK(check.Missing == std::vector<std::filesystem::path> { deleted, unreachable });
		CHECK(check.Gone == std::vector<std::filesystem::path> { deleted });

		// Forgetting takes deleted projects off the list file; projects out of reach stay on it.
		const std::filesystem::path file = directory / "RecentProjects.json";
		const std::vector<RecentProject> projects = { { "Present", present, 3, "1.0" }, { "Deleted", deleted, 2, "1.0" }, { "Game", unreachable, 1, "1.0" } };
		REQUIRE(FileSystem::WriteText(file, RecentProjects::ToJson(projects).dump()));
		RecentProjects recent(file);
		CHECK(recent.GetAllProjects().size() == 3);
		CHECK(recent.GetProjects().size() == 1);
		recent.Forget(check.Gone);
		const std::optional<std::vector<RecentProject>> stored = RecentProjects::ReadFile(file).Projects;
		REQUIRE(stored);
		REQUIRE(stored->size() == 2);
		CHECK((*stored)[0].Name == "Present");
		CHECK((*stored)[1].Name == "Game");
	}

	TEST_CASE("Read-only and in-memory lists never write a file")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecentReadOnly");
		const std::filesystem::path file = directory / "RecentProjects.json";
		const std::filesystem::path project = CreateProjectFile(directory / "Game", "Game");
		RecentProjects readOnly(file, true);
		readOnly.Add("Game", project);
		CHECK(readOnly.GetProjects().size() == 1);
		CHECK_FALSE(FileSystem::Exists(file));

		RecentProjects memory;
		memory.Add("Game", project);
		memory.Add("Game", project);
		CHECK(memory.GetProjects().size() == 1);
		CHECK(memory.GetFile().empty());
	}

	TEST_CASE("The user's list comes from STRATA_RECENT_PROJECTS or the user data directory")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecentDefault");
		{
			Tests::ScopedEnvironmentVariable variable(RecentProjects::c_FileVariable, FileSystem::ToUTF8(directory / "Mine.json"));
			const std::optional<std::filesystem::path> file = RecentProjects::GetDefaultFile();
			REQUIRE(file);
			CHECK(*file == directory / "Mine.json");
		}
		Tests::ScopedEnvironmentVariable unset(RecentProjects::c_FileVariable, "");
		const std::optional<std::filesystem::path> userFile = RecentProjects::GetDefaultFile();
		if (const std::optional<std::filesystem::path> userData = Platform::FindUserDataDirectory("Strata"))
		{
			REQUIRE(userFile);
			CHECK(*userFile == *userData / "RecentProjects.json");
		}
	}

	TEST_CASE("Opened and created projects are listed by editor.recentProjects")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecentCommand");
		const std::filesystem::path file = directory / "RecentProjects.json";
		EditorContextSpecification specification;
		specification.WatchAssetFiles = false;
		specification.RecentProjectsFile = file;
		EditorCommandRegistry commands;
		{
			EditorContext context(specification);
			EditorCommandResult result = commands.Execute(context, "editor.recentProjects", nlohmann::json::object());
			REQUIRE(result.Success);
			CHECK(result.Value["projects"].empty());

			REQUIRE(commands.Execute(context, "project.create", { { "directory", FileSystem::ToUTF8(directory / "First") }, { "name", "First" } }).Success);
			REQUIRE(commands.Execute(context, "project.create", { { "directory", FileSystem::ToUTF8(directory / "Second") }, { "name", "Second" } }).Success);
			REQUIRE(commands.Execute(context, "project.open", { { "path", FileSystem::ToUTF8(directory / "First") } }).Success);
			result = commands.Execute(context, "editor.recentProjects", nlohmann::json::object());
			REQUIRE(result.Success);
			const nlohmann::json& projects = result.Value["projects"];
			REQUIRE(projects.size() == 2);
			CHECK(projects[0]["name"] == "First");
			CHECK(projects[1]["name"] == "Second");
			CHECK(SamePath(FileSystem::FromUTF8(projects[0]["path"].get<std::string>()), context.GetProject()->GetProjectFile()));
			CHECK(projects[0]["engineVersion"] == c_EngineVersion);
			CHECK(projects[0]["lastOpened"].is_number_integer());

			// project.open takes the listed path.
			REQUIRE(commands.Execute(context, "project.open", { { "path", projects[1]["path"] } }).Success);
			CHECK(context.GetProject()->GetConfig().Name == "Second");
		}
		// Another editor of the user sees the same list.
		EditorContext later(specification);
		REQUIRE(later.GetRecentProjects().GetProjects().size() == 2);
		CHECK(later.GetRecentProjects().GetProjects()[0].Name == "Second");

		// Scripted runs read the user's list without writing it, and list the projects of their session over it.
		EditorContextSpecification scripted = specification;
		scripted.RecentProjectsReadOnly = true;
		{
			const std::optional<std::string> before = FileSystem::ReadText(file);
			EditorContext context(scripted);
			REQUIRE(commands.Execute(context, "project.create", { { "directory", FileSystem::ToUTF8(directory / "Third") }, { "name", "Third" } }).Success);
			REQUIRE(commands.Execute(context, "project.create", { { "directory", FileSystem::ToUTF8(directory / "Fourth") }, { "name", "Fourth" } }).Success);
			EditorCommandResult listed = commands.Execute(context, "editor.recentProjects", nlohmann::json::object());
			REQUIRE(listed.Success);
			REQUIRE(listed.Value["projects"].size() == 4);
			CHECK(listed.Value["projects"][0]["name"] == "Fourth");
			CHECK(listed.Value["projects"][1]["name"] == "Third");
			CHECK(listed.Value["projects"][2]["name"] == "Second");
			REQUIRE(commands.Execute(context, "editor.removeRecentProject", { { "path", listed.Value["projects"][1]["path"] } }).Success);
			listed = commands.Execute(context, "editor.recentProjects", nlohmann::json::object());
			REQUIRE(listed.Value["projects"].size() == 3);
			CHECK(listed.Value["projects"][1]["name"] == "Second");
			CHECK(FileSystem::ReadText(file) == before);
		}

		// Editors that keep the list in memory (tests, by default) still list the session's projects.
		EditorContext memoryOnly(EditorContextSpecification { false });
		REQUIRE(commands.Execute(memoryOnly, "project.open", { { "path", FileSystem::ToUTF8(directory / "First") } }).Success);
		EditorCommandResult result = commands.Execute(memoryOnly, "editor.recentProjects", nlohmann::json::object());
		REQUIRE(result.Success);
		REQUIRE(result.Value["projects"].size() == 1);
		CHECK(result.Value["projects"][0]["name"] == "First");
	}
}
