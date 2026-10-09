#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"
#include "Strata/Project/Project.h"
#include "TestHelpers.h"

#include <string>

using namespace Strata;

TEST_SUITE("Project")
{
	TEST_CASE("Projects are created with their directory layout and reload")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("Project") / "My Game";
		std::string error;
		Ref<Project> project = Project::Create(directory, "MyGame", &error);
		REQUIRE_MESSAGE(project, error);

		CHECK(FileSystem::IsRegularFile(directory / "MyGame.stproj"));
		CHECK(FileSystem::IsDirectory(project->GetAssetDirectory()));
		CHECK(FileSystem::IsDirectory(project->GetCacheDirectory()));
		CHECK(project->GetAssetDirectory() == (directory / "Assets").lexically_normal());
		std::optional<std::string> gitIgnore = FileSystem::ReadText(directory / ".gitignore");
		REQUIRE(gitIgnore);
		CHECK(gitIgnore->find("/.strata/") != std::string::npos);

		project->GetConfig().StartScene = UUID(0xABCDEF);
		project->GetConfig().AssetDirectory = "Content/Assets";
		REQUIRE(project->Save(&error));

		CHECK(Project::FindProjectFile(directory, &error) == project->GetProjectFile());
		Ref<Project> loaded = Project::Load(project->GetProjectFile(), &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetConfig().Name == "MyGame");
		CHECK(loaded->GetConfig().StartScene == UUID(0xABCDEF));
		CHECK(loaded->GetConfig().AssetDirectory == "Content/Assets");
		CHECK(loaded->GetAssetDirectory() == (directory / "Content" / "Assets").lexically_normal());
		CHECK(loaded->GetProjectDirectory() == project->GetProjectDirectory());

		// A directory holds one project.
		CHECK_FALSE(Project::Create(directory, "Other", &error));
		CHECK_FALSE(error.empty());
	}

	TEST_CASE("Invalid project names and files are rejected")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectInvalid");
		std::string error;
		for (const char* name : { "", "Bad/Name", "Bad:Name", " Padded", "Trailing.", "Quote\"", "CON", "nul.game", "Lpt1" })
		{
			CAPTURE(name);
			CHECK_FALSE(Project::Create(directory / "Invalid", name, &error));
			CHECK_FALSE(error.empty());
		}

		CHECK_FALSE(Project::Load(directory / "Missing.stproj", &error));

		auto writeAndLoad = [&](const std::string& text)
		{
			const std::filesystem::path path = directory / "Test.stproj";
			REQUIRE(FileSystem::WriteText(path, text));
			error.clear();
			Ref<Project> project = Project::Load(path, &error);
			CHECK((project || !error.empty()));
			return project;
		};
		CHECK_FALSE(writeAndLoad("not json"));
		CHECK_FALSE(writeAndLoad("{ \"Strata\": { \"Format\": \"Scene\", \"Version\": 1 }, \"Project\": {} }"));
		CHECK_FALSE(writeAndLoad("{ \"Strata\": { \"Format\": \"Project\", \"Version\": 7 }, \"Project\": {} }"));
		CHECK_FALSE(writeAndLoad("{ \"Strata\": { \"Format\": \"Project\", \"Version\": 1 } }"));
		CHECK_FALSE(writeAndLoad("{ \"Strata\": { \"Format\": \"Project\", \"Version\": 1 }, \"Project\": { \"AssetDirectory\": \"../Outside\" } }"));
		CHECK_FALSE(writeAndLoad("{ \"Strata\": { \"Format\": \"Project\", \"Version\": 1 }, \"Project\": { \"AssetDirectory\": \"/Absolute\" } }"));
		for (const char* assetDirectory : { ".", "Assets/..", ".strata/Assets", "Content/.hidden" })
		{
			CAPTURE(assetDirectory);
			const std::string document = std::string("{ \"Strata\": { \"Format\": \"Project\", \"Version\": 1 }, \"Project\": { \"AssetDirectory\": \"")
				+ assetDirectory + "\" } }";
			CHECK_FALSE(writeAndLoad(document));
		}
		CHECK_FALSE(writeAndLoad("{ \"Strata\": { \"Format\": \"Project\", \"Version\": 1 }, \"Project\": { \"StartScene\": [1, 2] } }"));

		Ref<Project> defaults = writeAndLoad("{ \"Strata\": { \"Format\": \"Project\", \"Version\": 1 }, \"Project\": {} }");
		REQUIRE(defaults);
		CHECK(defaults->GetConfig().Name == "Test");
		CHECK(defaults->GetConfig().AssetDirectory == "Assets");
		CHECK_FALSE(defaults->GetConfig().StartScene.IsValid());
	}

	TEST_CASE("Project files are found only when unambiguous")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectFind");
		std::string error;
		CHECK(Project::FindProjectFile(directory, &error).empty());
		CHECK_FALSE(error.empty());

		REQUIRE(FileSystem::WriteText(directory / "A.stproj", "{}"));
		CHECK(Project::FindProjectFile(directory) == directory / "A.stproj");
		REQUIRE(FileSystem::WriteText(directory / "B.stproj", "{}"));
		CHECK(Project::FindProjectFile(directory, &error).empty());
		CHECK(error.find("2") != std::string::npos);
	}

	TEST_CASE("Projects store their script settings")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectScripts") / "Space Game";
		std::string error;
		Ref<Project> project = Project::Create(directory, "Space Game!", &error);
		REQUIRE_MESSAGE(project, error);
		CHECK(project->GetConfig().Scripts.SourceDirectory == "Scripts");
		CHECK(project->GetConfig().Scripts.ModuleName == "SpaceGameScripts");
		CHECK(project->GetScriptModuleName() == "SpaceGameScripts");
		CHECK(project->GetScriptSourceDirectory() == (directory / "Scripts").lexically_normal());
		CHECK(project->GetScriptBuildDirectory() == project->GetIntermediateDirectory() / "Scripts" / "Build");
		CHECK(project->GetScriptModulePath().parent_path() == project->GetScriptBinaryDirectory());
		CHECK(FileSystem::ToUTF8(project->GetScriptModulePath().filename()).rfind("SpaceGameScripts.", 0) == 0);

		// Renaming the project keeps the stored module name.
		project->GetConfig().Name = "Renamed";
		project->GetConfig().Scripts.SourceDirectory = "Code/Scripts";
		REQUIRE_MESSAGE(project->Save(&error), error);
		Ref<Project> loaded = Project::Load(project->GetProjectFile(), &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetConfig().Scripts.SourceDirectory == "Code/Scripts");
		CHECK(loaded->GetScriptModuleName() == "SpaceGameScripts");

		// Invalid settings are refused when saving and loading.
		project->GetConfig().Scripts.ModuleName = "Not an identifier";
		CHECK_FALSE(project->Save(&error));
		project->GetConfig().Scripts.ModuleName.clear();
		project->GetConfig().Scripts.SourceDirectory = "../Outside";
		CHECK_FALSE(project->Save(&error));

		const std::filesystem::path file = directory / "Other.stproj";
		auto load = [&](const std::string& scripts)
		{
			REQUIRE(FileSystem::WriteText(file, "{ \"Strata\": { \"Format\": \"Project\", \"Version\": 2 }, \"Project\": { \"Scripts\": " + scripts + " } }"));
			return Project::Load(file, &error);
		};
		CHECK_FALSE(load("[]"));
		CHECK_FALSE(load("{ \"SourceDirectory\": \".strata/Scripts\" }"));
		CHECK_FALSE(load("{ \"ModuleName\": \"1Bad\" }"));
		CHECK_FALSE(load("{ \"ModuleName\": \"" + std::string(Project::c_MaxScriptModuleNameSize + 1, 'A') + "\" }"));
		Ref<Project> custom = load("{ \"SourceDirectory\": \"Gameplay\", \"ModuleName\": \"Game_Logic\" }");
		REQUIRE_MESSAGE(custom, error);
		CHECK(custom->GetScriptModuleName() == "Game_Logic");
		CHECK(custom->GetScriptSourceDirectory() == (directory / "Gameplay").lexically_normal());
	}

	TEST_CASE("Version 1 projects load with the default script settings")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectVersion1");
		const std::filesystem::path file = directory / "Old Game.stproj";
		REQUIRE(FileSystem::WriteText(file, "{ \"Strata\": { \"Format\": \"Project\", \"Version\": 1 }, \"Project\": { \"Name\": \"Old Game\" } }"));
		std::string error;
		Ref<Project> project = Project::Load(file, &error);
		REQUIRE_MESSAGE(project, error);
		CHECK(project->GetConfig().Scripts.SourceDirectory == "Scripts");
		CHECK(project->GetConfig().Scripts.ModuleName.empty());
		CHECK(project->GetScriptModuleName() == "OldGameScripts");

		// Saving upgrades the file to the current version with the derived module name.
		REQUIRE_MESSAGE(project->Save(&error), error);
		const std::optional<std::string> saved = FileSystem::ReadText(file);
		REQUIRE(saved);
		CHECK(saved->find("\"Version\": 2") != std::string::npos);
		CHECK(saved->find("\"ModuleName\": \"OldGameScripts\"") != std::string::npos);
	}

	TEST_CASE("Script module names are derived from project names")
	{
		CHECK(Project::MakeScriptModuleName("MyGame") == "MyGameScripts");
		CHECK(Project::MakeScriptModuleName("my game!") == "MyGameScripts");
		CHECK(Project::MakeScriptModuleName("2D Shooter") == "Game2DShooterScripts");
		CHECK(Project::MakeScriptModuleName("") == "GameScripts");
		CHECK(Project::MakeScriptModuleName("\xC3\xA9t\xC3\xA9") == "TScripts"); // Non-ASCII letters separate words
		CHECK(Project::MakeScriptModuleName(std::string(200, 'x')).size() == Project::c_MaxScriptModuleNameSize);
		for (const char* name : { "MyGameScripts", "_Private", "Game2D" })
			CHECK(Project::IsValidScriptModuleName(name));
		for (const char* name : { "", "2D", "My Game", "Bad-Name", "\xC3\xA9" })
			CHECK_FALSE(Project::IsValidScriptModuleName(name));
	}

	TEST_CASE("The active project can be set and cleared")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectActive");
		Ref<Project> project = Project::Create(directory, "Active");
		REQUIRE(project);
		Project::SetActive(project);
		CHECK(Project::GetActive() == project);
		Project::SetActive(nullptr);
		CHECK(Project::GetActive() == nullptr);
	}
}
