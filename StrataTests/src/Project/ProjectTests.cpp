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
