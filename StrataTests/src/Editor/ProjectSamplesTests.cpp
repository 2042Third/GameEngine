#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ProjectSamples.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	struct SamplesHarness
	{
		EditorContext Context;
		EditorCommandRegistry Commands;

		explicit SamplesHarness(const std::filesystem::path& samplesDirectory)
			: Context(MakeSpecification(samplesDirectory))
		{
		}

		static EditorContextSpecification MakeSpecification(const std::filesystem::path& samplesDirectory)
		{
			EditorContextSpecification specification;
			specification.WatchAssetFiles = false;
			specification.HotReloadScripts = false;
			specification.SamplesDirectory = samplesDirectory;
			return specification;
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			// Deferred commands complete over editor frames (the project file's write delays project.close, for one).
			const EditorCommandResult result = Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		EditorCommandResult Execute(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Commands.Execute(Context, name, parameters);
		}
	};

	// A private copy of the repository's Tetris sample and an index that lists it, with local editor data in the sample, as if
	// someone had opened it in place.
	std::filesystem::path CopySamples(const std::string& name)
	{
		const std::filesystem::path samples = Tests::CreateTemporaryDirectory(name) / "Samples";
		std::string error;
		REQUIRE_MESSAGE(Tests::CopySamples(samples, { "Tetris" }, &error), error);
		REQUIRE(FileSystem::WriteText(samples / "Tetris" / ".strata" / "Marker.txt", "local editor data"));
		return samples;
	}

	// The directories the repository's samples index lists, in its order.
	std::vector<std::string> GetRepositorySampleIds()
	{
		const std::optional<std::string> text = FileSystem::ReadText(FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "Samples" / "Samples.json");
		REQUIRE(text);
		const nlohmann::json index = nlohmann::json::parse(*text);
		std::vector<std::string> ids;
		for (const nlohmann::json& entry : index["Samples"])
			ids.push_back(entry["Directory"].get<std::string>());
		return ids;
	}

	size_t CountFiles(const std::filesystem::path& directory)
	{
		std::error_code error;
		size_t count = 0;
		for (std::filesystem::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
		{
			if (it->is_regular_file(error))
				count++;
		}
		return count;
	}

	// The files below a directory as sorted relative paths ('/' separated), without local editor data (a .strata directory
	// at any depth), which the build's copy and project.openSample leave out: opening a sample in place to change it must
	// not make its copies look incomplete.
	std::vector<std::string> ListFiles(const std::filesystem::path& directory)
	{
		std::vector<std::string> files;
		std::error_code error;
		for (std::filesystem::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
		{
			if (it->path().filename() == ProjectSamples::c_LocalDataDirectory)
			{
				it.disable_recursion_pending();
				continue;
			}
			if (it->is_regular_file(error))
			{
				std::string file = FileSystem::ToUTF8(it->path().lexically_relative(directory));
				std::replace(file.begin(), file.end(), '\\', '/');
				files.push_back(std::move(file));
			}
		}
		std::sort(files.begin(), files.end());
		return files;
	}

	// Checks that two directories hold the same files (ListFiles), naming the ones only one of them has.
	void CheckSameFiles(const std::filesystem::path& expected, const std::filesystem::path& actual)
	{
		const std::vector<std::string> expectedFiles = ListFiles(expected);
		const std::vector<std::string> actualFiles = ListFiles(actual);
		std::vector<std::string> missing;
		std::vector<std::string> extra;
		std::set_difference(expectedFiles.begin(), expectedFiles.end(), actualFiles.begin(), actualFiles.end(), std::back_inserter(missing));
		std::set_difference(actualFiles.begin(), actualFiles.end(), expectedFiles.begin(), expectedFiles.end(), std::back_inserter(extra));
		std::string missingText;
		for (const std::string& file : missing)
			missingText += " " + file;
		std::string extraText;
		for (const std::string& file : extra)
			extraText += " " + file;
		INFO("Missing in ", FileSystem::ToUTF8(actual), ":", missingText);
		INFO("Not in ", FileSystem::ToUTF8(expected), ":", extraText);
		CHECK_FALSE(expectedFiles.empty());
		CHECK(missing.empty());
		CHECK(extra.empty());
	}

}

TEST_SUITE("Editor.Samples")
{
	TEST_CASE("The build puts the samples next to the executables, without local editor data")
	{
		const std::filesystem::path directory = ProjectSamples::GetDefaultDirectory();
		CHECK(directory == Tests::GetTestExecutablePath().parent_path() / "Samples");
		std::string error;
		const std::optional<std::vector<ProjectSample>> samples = ProjectSamples::List(directory, &error);
		REQUIRE_MESSAGE(samples.has_value(), error);
		// Every sample the repository's index lists, in its order, each with its picture.
		const std::vector<std::string> ids = GetRepositorySampleIds();
		REQUIRE(samples->size() == ids.size());
		for (size_t index = 0; index < ids.size(); index++)
		{
			const ProjectSample& sample = (*samples)[index];
			CAPTURE(sample.Id);
			CHECK(sample.Id == ids[index]);
			CHECK_FALSE(sample.Name.empty());
			CHECK_FALSE(sample.Description.empty());
			CHECK(sample.ProjectFile.parent_path() == directory / FileSystem::FromUTF8(sample.Id));
			CHECK(sample.Picture == directory / FileSystem::FromUTF8(sample.Id) / "Picture.png");
			CHECK_FALSE(FileSystem::Exists(directory / FileSystem::FromUTF8(sample.Id) / ".strata"));
			// The same files as the repository's sample (without its local editor data, should it have been opened in place).
			CheckSameFiles(FileSystem::FromUTF8(STRATA_SOURCE_DIR) / "Samples" / FileSystem::FromUTF8(sample.Id), directory / FileSystem::FromUTF8(sample.Id));
		}
		const auto tetris = std::find_if(samples->begin(), samples->end(), [](const ProjectSample& sample) { return sample.Id == "Tetris"; });
		REQUIRE(tetris != samples->end());
		CHECK(tetris->ProjectFile == directory / "Tetris" / "Tetris.stproj");
		CHECK(FileSystem::IsRegularFile(directory / "Tetris" / "Scripts" / "CMakeLists.txt"));
	}

	TEST_CASE("project.samples lists the samples, and project.openSample opens a copy of one")
	{
		const std::filesystem::path samples = CopySamples("SamplesOpen");
		const size_t sampleFiles = CountFiles(samples);
		SamplesHarness harness(samples);

		const nlohmann::json list = harness.Run("project.samples");
		REQUIRE(list["samples"].size() == 1);
		CHECK(list["samples"][0]["id"] == "Tetris");
		CHECK(list["samples"][0]["name"] == "Tetris");
		CHECK_FALSE(list["samples"][0]["description"].get<std::string>().empty());
		CHECK(FileSystem::FromUTF8(list["samples"][0]["picture"].get<std::string>()) == samples / "Tetris" / "Picture.png");

		// Into a new directory below one that does not exist yet.
		const std::filesystem::path copy = Tests::CreateTemporaryDirectory("SamplesOpenCopy") / "Games" / "My Tetris";
		const nlohmann::json opened = Tests::RunCommandToCompletion(harness.Context, harness.Commands, "project.openSample",
			{ { "sample", "Tetris" }, { "directory", FileSystem::ToUTF8(copy) } });
		CHECK(opened["sample"] == "Tetris");
		CHECK(FileSystem::FromUTF8(opened["projectFile"].get<std::string>()) == copy / "Tetris.stproj");
		REQUIRE(harness.Context.HasProject());
		CHECK(harness.Context.GetProject()->GetProjectFile() == copy / "Tetris.stproj");
		// It opened like any project: its start scene is up, and it is the most recent project.
		CHECK(harness.Context.GetSceneHandle() == harness.Context.GetProject()->GetConfig().StartScene);
		CHECK(harness.Context.GetEditScene()->FindEntityByName("Well"));
		const nlohmann::json recent = harness.Run("editor.recentProjects")["projects"];
		REQUIRE_FALSE(recent.empty());
		std::error_code error;
		CHECK(std::filesystem::equivalent(FileSystem::FromUTF8(recent[0]["path"].get<std::string>()), copy / "Tetris.stproj", error));

		// The copy has the sample's files but not its local data; the sample is unchanged.
		CHECK(FileSystem::IsRegularFile(copy / "Scripts" / "TetrisGame.cpp"));
		CHECK(FileSystem::IsRegularFile(copy / "Assets" / "Scenes" / "Main.stscene"));
		CHECK_FALSE(FileSystem::Exists(copy / ".strata" / "Marker.txt"));
		CHECK(CountFiles(samples) == sampleFiles);
		// Every other file: the sample's local data and the copy's own do not count.
		REQUIRE(FileSystem::IsRegularFile(samples / "Tetris" / ".strata" / "Marker.txt"));
		CheckSameFiles(samples / "Tetris", copy);
		// The copy starts with the sample's picture as its own, so the launcher's card of the copy shows it before a save.
		CHECK(FileSystem::ReadBytes(copy / ".strata" / "Thumbnail.png") == FileSystem::ReadBytes(samples / "Tetris" / "Picture.png"));
		harness.Context.CloseProject();
	}

	TEST_CASE("project.openSample refuses what it cannot do and changes nothing")
	{
		const std::filesystem::path samples = CopySamples("SamplesRefused");
		SamplesHarness harness(samples);
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("SamplesRefusedCopy");

		// An unknown sample names the ones there are.
		EditorCommandResult result = harness.Execute("project.openSample", { { "sample", "Pong" }, { "directory", FileSystem::ToUTF8(root / "Pong") } });
		CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(result.Error.find("Tetris") != std::string::npos);
		CHECK_FALSE(FileSystem::Exists(root / "Pong"));

		// A directory with something in it.
		REQUIRE(FileSystem::WriteText(root / "Taken" / "Notes.txt", "mine"));
		result = harness.Execute("project.openSample", { { "sample", "Tetris" }, { "directory", FileSystem::ToUTF8(root / "Taken") } });
		CHECK(result.ErrorKind == EditorCommandError::Failed);
		CHECK(result.Error.find("not an empty directory") != std::string::npos);
		CHECK(CountFiles(root / "Taken") == 1);

		// A relative directory, and one inside the sample itself.
		result = harness.Execute("project.openSample", { { "sample", "Tetris" }, { "directory", "Relative/Tetris" } });
		CHECK(result.ErrorKind == EditorCommandError::Failed);
		result = harness.Execute("project.openSample", { { "sample", "Tetris" }, { "directory", FileSystem::ToUTF8(samples / "Tetris" / "Copy") } });
		CHECK(result.ErrorKind == EditorCommandError::Failed);
		CHECK_FALSE(FileSystem::Exists(samples / "Tetris" / "Copy"));
		CHECK_FALSE(harness.Context.HasProject());

		// An existing empty directory is fine.
		REQUIRE(FileSystem::CreateDirectories(root / "Empty"));
		Tests::RunCommandToCompletion(harness.Context, harness.Commands, "project.openSample",
			{ { "sample", "Tetris" }, { "directory", FileSystem::ToUTF8(root / "Empty") } });
		CHECK(harness.Context.HasProject());
		harness.Context.CloseProject();

		// Missing parameters.
		CHECK(harness.Execute("project.openSample", { { "sample", "Tetris" } }).ErrorKind == EditorCommandError::InvalidParameters);
	}

	TEST_CASE("A sample whose picture is not a PNG inside it is offered without one")
	{
		const std::filesystem::path samples = CopySamples("SamplesPicture");
		const std::filesystem::path index = samples / ProjectSamples::c_IndexFileName;
		REQUIRE(FileSystem::WriteText(samples / "Tetris" / "Notes.png", "not a picture"));
		REQUIRE(FileSystem::WriteText(samples / "Outside.png", "outside"));
		const auto listWith = [&](const std::string& picture)
		{
			REQUIRE(FileSystem::WriteText(index, R"({ "Strata": { "Format": "Samples", "Version": 1 }, "Samples": [ { "Directory": "Tetris", "Name": "Tetris", )"
				R"("Picture": )" + picture + " } ] }"));
			std::string error;
			const std::optional<std::vector<ProjectSample>> listed = ProjectSamples::List(samples, &error);
			REQUIRE_MESSAGE(listed.has_value(), error);
			REQUIRE(listed->size() == 1);
			return listed->front().Picture;
		};
		CHECK(listWith(R"("Picture.png")") == samples / "Tetris" / "Picture.png");
		const std::string absolute = nlohmann::json(FileSystem::ToUTF8(samples / "Tetris" / "Picture.png")).dump();
		for (const std::string& picture : { absolute, std::string(R"("../Outside.png")"), std::string(R"("Missing.png")"), std::string(R"("Notes.png")"),
			std::string(R"("")"), std::string("5") })
		{
			CAPTURE(picture);
			Tests::LogCapture log;
			CHECK(listWith(picture).empty());
			const std::vector<LogEntry> entries = log.GetEntries();
			CHECK(std::any_of(entries.begin(), entries.end(), [](const LogEntry& entry)
			{
				return entry.Level == LogLevel::Warn && entry.Message.find("The sample 'Tetris' is offered without its picture") != std::string::npos;
			}));
		}

		// A copy of a sample without a picture has no thumbnail until its first save.
		listWith(R"("Missing.png")");
		SamplesHarness harness(samples);
		CHECK(harness.Run("project.samples")["samples"][0]["picture"].is_null());
		const std::filesystem::path copy = Tests::CreateTemporaryDirectory("SamplesPictureCopy") / "Tetris";
		Tests::RunCommandToCompletion(harness.Context, harness.Commands, "project.openSample", { { "sample", "Tetris" }, { "directory", FileSystem::ToUTF8(copy) } });
		CHECK_FALSE(FileSystem::Exists(copy / ".strata" / "Thumbnail.png"));
		harness.Context.CloseProject();
	}

	TEST_CASE("The samples index is validated")
	{
		const std::filesystem::path samples = CopySamples("SamplesIndex");
		const std::filesystem::path index = samples / ProjectSamples::c_IndexFileName;
		std::string error;
		auto listWith = [&](const std::string& entries)
		{
			REQUIRE(FileSystem::WriteText(index, R"({ "Strata": { "Format": "Samples", "Version": 1 }, "Samples": [)" + entries + "] }"));
			error.clear();
			return ProjectSamples::List(samples, &error);
		};

		CHECK(listWith(R"({ "Directory": "Tetris", "Name": "Tetris" })").value().size() == 1);
		// Entries must name one directory of the samples directory.
		for (const char* entry : { R"({ "Directory": "../Tetris", "Name": "Tetris" })", R"({ "Directory": "..", "Name": "Up" })",
			R"({ "Directory": "Tetris/Assets", "Name": "Assets" })", R"({ "Directory": ".strata", "Name": "Data" })", R"({ "Name": "Nameless" })",
			R"({ "Directory": "Tetris", "Name": "" })", R"({ "Directory": "Tetris", "Name": "Tetris", "Description": 5 })",
			R"({ "Directory": "Tetris", "Name": "A" }, { "Directory": "Tetris", "Name": "B" })" })
		{
			CAPTURE(entry);
			CHECK_FALSE(listWith(entry).has_value());
			CHECK_FALSE(error.empty());
		}
		// A sample whose project is missing is left out; the others stay.
		const std::optional<std::vector<ProjectSample>> partial = listWith(R"({ "Directory": "Missing", "Name": "Missing" }, { "Directory": "Tetris", "Name": "Tetris" })");
		REQUIRE(partial.has_value());
		REQUIRE(partial->size() == 1);
		CHECK(partial->front().Id == "Tetris");

		// Not an index at all, or none.
		REQUIRE(FileSystem::WriteText(index, R"({ "Strata": { "Format": "Scene", "Version": 1 } })"));
		CHECK_FALSE(ProjectSamples::List(samples, &error).has_value());
		REQUIRE(FileSystem::WriteText(index, "not json"));
		CHECK_FALSE(ProjectSamples::List(samples, &error).has_value());
		SamplesHarness harness(samples / "Nowhere");
		const EditorCommandResult result = harness.Execute("project.samples");
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("No samples") != std::string::npos);
	}

	TEST_CASE("project.close closes the project")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProjectClose") / "Game";
		SamplesHarness harness({});
		CHECK(harness.Run("project.close")["closed"] == false);
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" }, { "template", "basic3d" } });
		REQUIRE(harness.Context.HasProject());
		CHECK(harness.Run("project.close")["closed"] == true);
		CHECK_FALSE(harness.Context.HasProject());
		CHECK(harness.Context.GetEditScene()->GetEntityCount() == 0);
		// Like after startup, the built-in assets stay available.
		CHECK(harness.Run("project.info")["name"].is_null());
	}

	TEST_CASE("editor.removeRecentProject takes a project off the list")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("RecentRemove");
		SamplesHarness harness({});
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(root / "One") }, { "name", "One" } });
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(root / "Two") }, { "name", "Two" } });
		harness.Context.CloseProject();
		REQUIRE(harness.Run("editor.recentProjects")["projects"].size() == 2);

		harness.Run("editor.removeRecentProject", { { "path", FileSystem::ToUTF8(root / "One" / "One.stproj") } });
		const nlohmann::json projects = harness.Run("editor.recentProjects")["projects"];
		REQUIRE(projects.size() == 1);
		CHECK(projects[0]["name"] == "Two");
		// The project itself stays.
		CHECK(FileSystem::IsRegularFile(root / "One" / "One.stproj"));
		// A project that is not on the list.
		const EditorCommandResult result = harness.Execute("editor.removeRecentProject", { { "path", FileSystem::ToUTF8(root / "One" / "One.stproj") } });
		CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
	}
}
