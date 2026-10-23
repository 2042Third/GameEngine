#include <doctest/doctest.h>

#include "Core/SettingsTestUtils.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Project/GameInfo.h"
#include "Strata/Project/Project.h"
#include "TestHelpers.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;

namespace
{

	nlohmann::json ReadJson(const std::filesystem::path& file)
	{
		const std::optional<std::string> text = FileSystem::ReadText(file);
		REQUIRE(text);
		std::optional<nlohmann::json> json = JsonUtils::Parse(*text);
		REQUIRE(json);
		return *json;
	}

	// The member at `path` of a stored document, or null when it is missing (operator[] of a const document asserts then).
	nlohmann::json GetMember(const nlohmann::json& document, std::initializer_list<std::string_view> path)
	{
		const nlohmann::json* member = &document;
		for (std::string_view key : path)
		{
			member = JsonUtils::Find(*member, key);
			if (!member)
				return nullptr;
		}
		return *member;
	}

	// A project file (Test.stproj in `directory`) of the given format version, "Project" object and "Settings" (none when
	// null).
	std::filesystem::path WriteProjectFile(const std::filesystem::path& directory, uint32_t version, const nlohmann::json& project,
		const nlohmann::json& settings = nullptr)
	{
		nlohmann::json document = { { "Strata", { { "Format", "Project" }, { "Version", version } } }, { "Project", project } };
		if (!settings.is_null())
			document["Settings"] = settings;
		const std::filesystem::path file = directory / "Test.stproj";
		REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(document, 1, '\t')));
		return file;
	}

	Ref<Project> LoadProject(const std::filesystem::path& file, const SettingsRegistry& registry)
	{
		std::string error;
		Ref<Project> project = Project::Load(file, &error, &registry);
		REQUIRE_MESSAGE(project, error);
		return project;
	}

}

TEST_SUITE("Project.Settings")
{
	TEST_CASE("Sections round-trip through the project file")
	{
		SettingsRegistry registry;
		Tests::RegisterTestSections(registry);
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectSettingsRoundTrip");
		std::string error;
		Ref<Project> project = Project::Create(directory, "Settings", &error);
		REQUIRE_MESSAGE(project, error);
		// A project that sets nothing stores no settings.
		CHECK_FALSE(ReadJson(project->GetProjectFile()).contains("Settings"));

		const nlohmann::json simple = { { "Speed", 2.5 }, { "Mode", "Slow" }, { "Icon", "00000000000000AB" } };
		const nlohmann::json complex = { { "Names", { "A", "B" } }, { "Targets", { "0000000000000007" } } };
		REQUIRE_MESSAGE(project->GetSettings().Set("Test.Simple", simple, registry, &error), error);
		REQUIRE_MESSAGE(project->GetSettings().Set("Test.Complex", complex, registry, &error), error);
		REQUIRE_MESSAGE(project->Save(&error), error);

		const nlohmann::json stored = ReadJson(project->GetProjectFile());
		CHECK(GetMember(stored, { "Strata", "Version" }) == 3);
		CHECK(GetMember(stored, { "Settings", "Test.Simple", "Version" }) == 1);
		CHECK(GetMember(stored, { "Settings", "Test.Complex", "Version" }) == 2);
		CHECK(GetMember(stored, { "Settings", "Test.Complex", "Value" }) == complex);

		Ref<Project> loaded = LoadProject(project->GetProjectFile(), registry);
		CHECK(loaded->GetSettingsProblems().empty());
		REQUIRE(loaded->GetSettings().FindSection("Test.Simple"));
		REQUIRE(loaded->GetSettings().FindSection("Test.Complex"));
		CHECK(*loaded->GetSettings().FindSection("Test.Simple") == *project->GetSettings().FindSection("Test.Simple"));
		CHECK((*loaded->GetSettings().FindSection("Test.Simple"))["Speed"] == 2.5);
		CHECK(*loaded->GetSettings().FindSection("Test.Complex") == complex);
		CHECK(loaded->GetSettings().GetReferences(registry) == std::vector<UUID> { UUID(7), UUID(0xAB) });

		// A section reset is no longer stored.
		CHECK(loaded->GetSettings().Reset("Test.Simple"));
		REQUIRE_MESSAGE(loaded->Save(&error), error);
		CHECK_FALSE(GetMember(ReadJson(project->GetProjectFile()), { "Settings" }).contains("Test.Simple"));
		CHECK_FALSE(LoadProject(project->GetProjectFile(), registry)->GetSettings().FindSection("Test.Simple"));
	}

	TEST_CASE("A section the build does not know survives a save")
	{
		SettingsRegistry registry;
		Tests::RegisterTestSections(registry);
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectSettingsUnknown");
		const nlohmann::json unknown = { { "Version", 3 }, { "Value", { { "Quality", "High" }, { "Levels", { 1, 2, 3 } } } } };
		const std::filesystem::path file = WriteProjectFile(directory, 3, { { "Name", "Unknown" } },
			{ { "Renderer.Quality", unknown }, { "Test.Simple", { { "Version", 1 }, { "Value", { { "Count", 2 } } } } } });

		Ref<Project> project = LoadProject(file, registry);
		CHECK(project->GetSettingsProblems().empty());
		CHECK_FALSE(project->GetSettings().FindSection("Renderer.Quality"));
		CHECK(project->GetSettings().IsStored("Renderer.Quality"));
		std::string error;
		REQUIRE_MESSAGE(project->GetSettings().Set("Test.Simple", { { "Count", 3 } }, registry, &error), error);
		REQUIRE_MESSAGE(project->Save(&error), error);

		const nlohmann::json stored = ReadJson(file);
		CHECK(GetMember(stored, { "Settings", "Renderer.Quality" }) == unknown);
		CHECK(GetMember(stored, { "Settings", "Test.Simple", "Value", "Count" }) == 3);
		// A build that has the section reads it.
		SettingsRegistry withQuality;
		SettingsSection quality = Tests::MakeComplexTestSection();
		quality.Id = "Renderer.Quality";
		quality.Version = 3;
		quality.Read = [](const nlohmann::json& value, uint32_t, std::string*) -> std::optional<nlohmann::json> { return value; };
		REQUIRE(withQuality.Register(quality));
		Ref<Project> reloaded = LoadProject(file, withQuality);
		REQUIRE(reloaded->GetSettings().FindSection("Renderer.Quality"));
		CHECK(*reloaded->GetSettings().FindSection("Renderer.Quality") == unknown["Value"]);
	}

	TEST_CASE("Unknown members of Project survive a save")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectUnknownMembers");
		const nlohmann::json future = { { "Platforms", { "Windows", "Linux" } }, { "Signing", { { "Identity", "Studio" } } } };
		const std::filesystem::path file = WriteProjectFile(directory, 3, { { "Name", "Future" }, { "Future", future }, { "Version2", 7 } });

		SettingsRegistry registry;
		Ref<Project> project = LoadProject(file, registry);
		project->GetConfig().StartScene = UUID(0x1234);
		std::string error;
		REQUIRE_MESSAGE(project->Save(&error), error);

		const nlohmann::json stored = ReadJson(file);
		CHECK(GetMember(stored, { "Project", "Future" }) == future);
		CHECK(GetMember(stored, { "Project", "Version2" }) == 7);
		CHECK(GetMember(stored, { "Project", "Name" }) == "Future");
		CHECK(GetMember(stored, { "Project", "StartScene" }) == "0000000000001234");
		// What this version knows is written from the project, never from the members it kept.
		CHECK(LoadProject(file, registry)->GetConfig().StartScene == UUID(0x1234));
	}

	TEST_CASE("A section that fails Read keeps its stored value and reports it")
	{
		SettingsRegistry registry;
		Tests::RegisterTestSections(registry);
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectSettingsInvalid");
		const nlohmann::json invalid = { { "Version", 1 }, { "Value", { { "Count", 99 }, { "Label", "Kept" } } } };
		const nlohmann::json newer = { { "Version", 5 }, { "Value", { { "Names", { "From", "Later" } } } } };
		const std::filesystem::path file = WriteProjectFile(directory, 3, { { "Name", "Invalid" } },
			{ { "Test.Simple", invalid }, { "Test.Complex", newer } });

		Tests::LogCapture log;
		Ref<Project> project = LoadProject(file, registry);
		const std::vector<SettingsProblem>& problems = project->GetSettingsProblems();
		REQUIRE(problems.size() == 2);
		CHECK(problems[0].Section == "Test.Complex");
		CHECK(problems[0].Message.find("newer") != std::string::npos);
		CHECK(problems[1].Section == "Test.Simple");
		CHECK(problems[1].Message == "'Count' must be a whole number from 1 to 8");
		size_t warnings = 0;
		for (const LogEntry& entry : log.GetEntries())
		{
			if (entry.Level == LogLevel::Warn && entry.Message.find("'Test.Simple' is invalid") != std::string::npos
				&& entry.Message.find("'Count' must be") != std::string::npos)
			{
				warnings++;
			}
		}
		CHECK(warnings == 1);
		// The defaults apply.
		CHECK_FALSE(project->GetSettings().FindSection("Test.Simple"));
		CHECK_FALSE(project->GetSettings().FindSection("Test.Complex"));

		// Saving keeps the stored values until a section is set again.
		std::string error;
		REQUIRE_MESSAGE(project->Save(&error), error);
		CHECK(GetMember(ReadJson(file), { "Settings", "Test.Simple" }) == invalid);
		CHECK(GetMember(ReadJson(file), { "Settings", "Test.Complex" }) == newer);
		REQUIRE_MESSAGE(project->GetSettings().Set("Test.Simple", { { "Count", 5 } }, registry, &error), error);
		REQUIRE_MESSAGE(project->Save(&error), error);
		CHECK(GetMember(ReadJson(file), { "Settings", "Test.Simple", "Value", "Count" }) == 5);
		CHECK(GetMember(ReadJson(file), { "Settings", "Test.Complex" }) == newer);

		// A stored form of the wrong shape refuses the file.
		const std::filesystem::path broken = WriteProjectFile(directory, 3, { { "Name", "Broken" } }, { { "Test.Simple", { { "Value", 1 } } } });
		CHECK_FALSE(Project::Load(broken, &error, &registry));
		CHECK(error.find("Test.Simple") != std::string::npos);
		WriteProjectFile(directory, 3, { { "Name", "Broken" } }, nlohmann::json::array());
		CHECK_FALSE(Project::Load(broken, &error, &registry));
	}

	TEST_CASE("Version 1 and 2 project files load; saving writes version 3")
	{
		CHECK(Project::c_FormatVersion == 3);
		SettingsRegistry registry;
		for (uint32_t version : { 1u, 2u, 3u })
		{
			CAPTURE(version);
			const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectVersions");
			nlohmann::json config = { { "Name", "Old" }, { "StartScene", "0000000000000042" } };
			if (version >= 2)
				config["Scripts"] = { { "SourceDirectory", "Code" }, { "ModuleName", "OldScripts" } };
			const std::filesystem::path file = WriteProjectFile(directory, version, config);
			Ref<Project> project = LoadProject(file, registry);
			CHECK(project->GetConfig().Name == "Old");
			CHECK(project->GetConfig().StartScene == UUID(0x42));
			CHECK(project->GetSettings().IsEmpty());
			std::string error;
			REQUIRE_MESSAGE(project->Save(&error), error);
			const nlohmann::json stored = ReadJson(file);
			CHECK(GetMember(stored, { "Strata", "Version" }) == 3);
			CHECK(GetMember(stored, { "Project", "StartScene" }) == "0000000000000042");
			// Version 1 files derive the module's name from the project's, which saving stores.
			CHECK(GetMember(stored, { "Project", "Scripts", "ModuleName" }) == "OldScripts");
		}
		// Later versions are refused, naming what this engine reads.
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectVersions");
		std::string error;
		CHECK_FALSE(Project::Load(WriteProjectFile(directory, 4, { { "Name", "Later" } }), &error, &registry));
		CHECK(error.find("unsupported project version 4 (this engine reads up to 3)") != std::string::npos);
	}

	TEST_CASE("A project file over 1 MiB is refused")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectLarge");
		SettingsRegistry registry;
		const std::filesystem::path file = directory / "Large.stproj";
		const std::string padding(Project::c_MaxFileBytes, 'x');
		REQUIRE(FileSystem::WriteText(file, "{ \"Strata\": { \"Format\": \"Project\", \"Version\": 3 }, \"Project\": { \"Padding\": \"" + padding + "\" } }"));
		std::string error;
		CHECK_FALSE(Project::Load(file, &error, &registry));
		CHECK(error.find("larger than the 1 MiB") != std::string::npos);

		// Nor does Save write one: settings within their compact bound can still make a larger file once indented.
		Ref<Project> project = Project::Create(directory / "Saved", "Saved", &error);
		REQUIRE_MESSAGE(project, error);
		nlohmann::json levels = nlohmann::json::array();
		for (int index = 0; index < 200000; index++)
			levels.push_back(index % 10);
		std::optional<ProjectSettingsValues> values = ProjectSettingsValues::FromJson({ { "Renderer.Levels", { { "Version", 1 }, { "Value", levels } } } }, &error);
		REQUIRE_MESSAGE(values, error);
		project->GetSettings() = std::move(*values);
		CHECK_FALSE(project->Save(&error));
		CHECK(error.find("more than the 1 MiB a project file may take") != std::string::npos);
		// The file on disk is left as it was.
		CHECK_FALSE(ReadJson(project->GetProjectFile()).contains("Settings"));
	}

	TEST_CASE("Game info validates title, version, company and copyright")
	{
		CHECK_FALSE(ProjectGameInfo().Validate());
		CHECK(ProjectGameInfo().IsDefault());

		// Versions: one to four whole numbers from 0 to 65535, separated by dots.
		for (const char* version : { "1", "1.0.0", "2026.10.22.1", "65535.0", "0", "01.2" })
			CHECK_FALSE_MESSAGE(ProjectGameInfo::ValidateVersion(version), version);
		for (const char* version : { "", "1.", ".1", "1..0", "1.0.0.0.0", "65536", "100000", "1.a", "-1", "+1", " 1", "1.0 ", "v1.0" })
			CHECK_MESSAGE(ProjectGameInfo::ValidateVersion(version), version);
		CHECK(ProjectGameInfo::ParseVersion("1.2") == std::array<uint16_t, 4> { 1, 2, 0, 0 });
		CHECK(ProjectGameInfo::ParseVersion("2026.10.22.65535") == std::array<uint16_t, 4> { 2026, 10, 22, 65535 });
		CHECK_FALSE(ProjectGameInfo::ParseVersion("1.2.3.4.5"));

		// Text: valid UTF-8, at most 128 characters (not bytes), no control characters.
		std::string accents;
		for (int index = 0; index < 128; index++)
			accents += "\xC3\xA9"; // U+00E9, two bytes
		CHECK_FALSE(ProjectGameInfo::ValidateText(accents));
		CHECK_FALSE(ProjectGameInfo::ValidateText(""));
		CHECK_FALSE(ProjectGameInfo::ValidateText("\xE6\x98\x9F\xE9\x99\x85 Adventure (c) 2026"));
		REQUIRE(ProjectGameInfo::ValidateText(accents + "e"));
		CHECK(ProjectGameInfo::ValidateText(accents + "e")->find("129 characters") != std::string::npos);
		for (const char* text : { "Line\nbreak", "Tab\there", "Delete\x7F", "Next line \xC2\x85", "Bad \xFF byte", "Truncated \xE6\x98" })
			CHECK_MESSAGE(ProjectGameInfo::ValidateText(text), text);

		// Validate names the first invalid field.
		ProjectGameInfo game;
		game.Copyright = "(c)\n2026";
		game.Company = "Studio\t";
		REQUIRE(game.Validate());
		CHECK(game.Validate()->rfind("Company: ", 0) == 0);
		game.Company = "Studio";
		CHECK(game.Validate()->rfind("Copyright: ", 0) == 0);
		game.Copyright = "(c) 2026 Studio";
		game.Version = "1.0-beta";
		CHECK(game.Validate()->rfind("Version: ", 0) == 0);

		// The project file keeps them: written once they differ from the defaults.
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectGameInfo");
		SettingsRegistry registry;
		std::string error;
		Ref<Project> project = Project::Create(directory / "Game", "Game", &error);
		REQUIRE_MESSAGE(project, error);
		CHECK_FALSE(ReadJson(project->GetProjectFile())["Project"].contains("Game"));
		game.Version = "2.1.0";
		game.Title = "\xE6\x98\x9F Adventure";
		game.Icon = UUID(0xABC);
		project->GetConfig().Game = game;
		REQUIRE_MESSAGE(project->Save(&error), error);
		CHECK(GetMember(ReadJson(project->GetProjectFile()), { "Project", "Game", "Version" }) == "2.1.0");
		CHECK(LoadProject(project->GetProjectFile(), registry)->GetConfig().Game == game);

		// Save refuses invalid fields, leaving the file as it was; Load refuses a file that holds them.
		project->GetConfig().Game.Title = "Two\nlines";
		CHECK_FALSE(project->Save(&error));
		CHECK(error.find("Title") != std::string::npos);
		CHECK(LoadProject(project->GetProjectFile(), registry)->GetConfig().Game == game);
		for (const nlohmann::json& stored : { nlohmann::json { { "Version", "x" } }, nlohmann::json { { "Title", 7 } },
			nlohmann::json { { "Icon", "not a handle" } }, nlohmann::json("Game") })
		{
			const std::filesystem::path file = WriteProjectFile(directory, 3, { { "Name", "Invalid" }, { "Game", stored } });
			CHECK_FALSE_MESSAGE(Project::Load(file, &error, &registry), stored.dump());
			CHECK(error.find("\"Game") != std::string::npos);
		}
		// Members left out keep their defaults.
		const std::filesystem::path partial = WriteProjectFile(directory, 3, { { "Name", "Partial" }, { "Game", { { "Company", "Studio" } } } });
		const Ref<Project> partialProject = LoadProject(partial, registry);
		const ProjectGameInfo& read = partialProject->GetConfig().Game;
		CHECK(read.Company == "Studio");
		CHECK(read.Version == ProjectGameInfo::c_DefaultVersion);
		CHECK_FALSE(read.Icon.IsValid());
	}
}
