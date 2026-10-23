#include <doctest/doctest.h>

#include "Core/SettingsTestUtils.h"
#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/SettingsFileWriter.h"
#include "Editor/UndoStack.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/EngineServices.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/PropertyJson.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A project made from the basic3d template (Scenes/Main.stscene, its start scene) and a second saved scene, Scenes/Second.stscene,
	// which is the edited scene; the project file holds what the template wrote.
	// A job system of the test's own, so that the project file is written on a thread of its I/O pool, as in the editor.
	struct WriterJobSystem
	{
		WriterJobSystem()
		{
			JobSystemSpecification specification;
			specification.WorkerThreadCount = 2;
			specification.IOThreadCount = 2;
			specification.BackgroundThreadCount = 1;
			JobSystem::Init(specification);
		}

		~WriterJobSystem()
		{
			JobSystem::Shutdown();
		}

		WriterJobSystem(const WriterJobSystem&) = delete;
		WriterJobSystem& operator=(const WriterJobSystem&) = delete;
	};

	EditorContextSpecification MakeSpecification(const SettingsRegistry* settings)
	{
		EditorContextSpecification specification;
		specification.WatchAssetFiles = false;
		specification.Settings = settings;
		return specification;
	}

	struct SettingsHarness
	{
		WriterJobSystem Jobs; // Before the context, which it outlives
		EditorContext Context;
		EditorCommandRegistry Commands;
		std::filesystem::path ProjectFile;
		AssetHandle Main = UUID::Null();
		AssetHandle Second = UUID::Null();

		// settings: the sections the project's settings are validated with (null: the process's).
		explicit SettingsHarness(const std::string& name, const SettingsRegistry* settings = nullptr)
			: Context(MakeSpecification(settings))
		{
			const std::filesystem::path directory = CreateTemporaryDirectory(name) / "Game";
			Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" }, { "template", "basic3d" } });
			Main = Context.GetProject()->GetConfig().StartScene;
			Run("scene.saveAs", { { "path", "Scenes/Second.stscene" } });
			Second = Context.GetSceneHandle();
			ProjectFile = Context.GetProject()->GetProjectFile();
			REQUIRE(WaitForWrites());
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		EditorCommandResult Execute(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return CompleteCommand(Context, Commands.Execute(Context, name, parameters));
		}

		// Editor frames until no write of the project file is in flight; false when one still is after `timeout`.
		bool WaitForWrites(std::chrono::milliseconds timeout = std::chrono::seconds(30))
		{
			const auto deadline = std::chrono::steady_clock::now() + timeout;
			while (Context.GetProjectFileState() == SettingsFileState::Writing)
			{
				if (std::chrono::steady_clock::now() > deadline)
					return false;
				Context.Update(Timestep(1.0f / 60.0f));
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return true;
		}

		// The project as its file holds it now.
		Ref<Project> ReadProjectFile() const
		{
			std::string error;
			Ref<Project> project = Project::Load(ProjectFile, &error);
			INFO(error);
			REQUIRE(project);
			return project;
		}

		// Makes the project file's writes fail: a directory takes its place, which no file replaces.
		void BlockProjectFile() const
		{
			REQUIRE(FileSystem::Remove(ProjectFile));
			REQUIRE(FileSystem::CreateDirectories(ProjectFile));
		}

		void UnblockProjectFile() const
		{
			REQUIRE(FileSystem::Remove(ProjectFile));
		}

		// A texture asset of the project, imported from a PNG made for it.
		std::string ImportTexture(const std::string& name)
		{
			const std::filesystem::path source = CreateTemporaryDirectory("ProjectSettingsTexture") / (name + ".png");
			REQUIRE(FileSystem::WriteBytes(source, CreateSolidPNG(4, 4, 40, 200, 40)));
			return Run("asset.import", { { "file", FileSystem::ToUTF8(source) }, { "directory", "Textures" } })["asset"].get<std::string>();
		}

		// The project file's stored settings, as written.
		nlohmann::json ReadStoredSettings() const
		{
			return ReadProjectFile()->GetSettings().ToJson();
		}
	};

	// The test sections (SettingsTestUtils.h) and a preference, Test.Preference.
	void RegisterHarnessSections(SettingsRegistry& registry)
	{
		RegisterTestSections(registry);
		SettingsSection preference = MakeSimpleTestSection();
		preference.Id = "Test.Preference";
		preference.Scope = SettingsScope::User;
		REQUIRE(registry.Register(std::move(preference)));
	}

}

TEST_SUITE("Editor.ProjectSettings")
{
	TEST_CASE("setStartScene and setExportSettings record undo steps and no longer write on the main thread")
	{
		SettingsHarness project("ProjectSettingsUndoFields");
		REQUIRE_FALSE(project.Context.IsSceneModified());
		auto writingThread = std::make_shared<std::atomic<std::thread::id>>();
		project.Context.GetProjectFileWriter()->SetTestHook([writingThread]() { writingThread->store(std::this_thread::get_id()); });
		UndoStack& undo = project.Context.GetUndoStack();
		const size_t steps = undo.GetCount();

		// Each command is one step that leaves the scene saved; its file is written by the writer's job.
		project.Run("project.setStartScene", { { "scene", "Scenes/Second.stscene" } });
		CHECK(undo.GetCount() == steps + 1);
		CHECK(undo.GetUndoName() == "Set Start Scene");
		CHECK_FALSE(project.Context.IsSceneModified());
		REQUIRE(project.WaitForWrites());
		CHECK(writingThread->load() != std::thread::id());
		CHECK(writingThread->load() != std::this_thread::get_id());
		CHECK(project.ReadProjectFile()->GetConfig().StartScene == project.Second);

		project.Run("project.setExportSettings", { { "loadingScene", "Scenes/Main.stscene" }, { "alwaysInclude", { "Scenes" } } });
		CHECK(undo.GetCount() == steps + 2);
		CHECK(undo.GetUndoName() == "Set Export Settings");
		CHECK_FALSE(project.Context.IsSceneModified());
		REQUIRE(project.WaitForWrites());
		CHECK(project.ReadProjectFile()->GetConfig().Export.LoadingScene == project.Main);
		CHECK(project.ReadProjectFile()->GetConfig().Export.AlwaysInclude == std::vector<std::string> { "Scenes" });

		// A change that changes nothing records nothing.
		project.Run("project.setStartScene", { { "scene", "Scenes/Second.stscene" } });
		CHECK(undo.GetCount() == steps + 2);

		// Undo and redo set the values again and write them.
		REQUIRE(project.Context.Undo());
		CHECK_FALSE(project.Context.GetProject()->GetConfig().Export.LoadingScene.IsValid());
		REQUIRE(project.WaitForWrites());
		CHECK_FALSE(project.ReadProjectFile()->GetConfig().Export.LoadingScene.IsValid());
		CHECK(project.ReadProjectFile()->GetConfig().Export.AlwaysInclude.empty());
		REQUIRE(project.Context.Undo());
		CHECK(project.Context.GetProject()->GetConfig().StartScene == project.Main);
		REQUIRE(project.WaitForWrites());
		CHECK(project.ReadProjectFile()->GetConfig().StartScene == project.Main);
		REQUIRE(project.Context.Redo());
		REQUIRE(project.WaitForWrites());
		CHECK(project.ReadProjectFile()->GetConfig().StartScene == project.Second);
		CHECK_FALSE(project.Context.IsSceneModified());

		// A change the project file could not be written with is refused naming its bound, and changes nothing: export settings
		// that name the scenes' folder often enough to take the file over 1 MiB.
		const size_t largeSteps = undo.GetCount();
		const uint64_t writes = project.Context.GetProjectFileWriter()->GetWriteCount();
		const std::vector<std::string> included = project.Context.GetProject()->GetConfig().Export.AlwaysInclude;
		const nlohmann::json many(100000, "Scenes");
		const EditorCommandResult refused = project.Execute("project.setExportSettings", { { "alwaysInclude", many } });
		CHECK_FALSE(refused.Success);
		CHECK(refused.Error.find("1 MiB") != std::string::npos);
		CHECK(undo.GetCount() == largeSteps);
		CHECK(project.Context.GetProject()->GetConfig().Export.AlwaysInclude == included);
		CHECK(project.Context.GetProjectFileWriter()->GetWriteCount() == writes);
		CHECK(project.Context.GetProjectFileState() == SettingsFileState::Written);
	}

	TEST_CASE("The project file is written by a job, never by the main thread")
	{
		SettingsHarness project("ProjectSettingsWriterThread");
		auto writingThread = std::make_shared<std::atomic<std::thread::id>>();
		project.Context.GetProjectFileWriter()->SetTestHook([writingThread]() { writingThread->store(std::this_thread::get_id()); });
		const uint64_t writes = project.Context.GetProjectFileWriter()->GetWriteCount();

		project.Run("project.setExportSettings", { { "alwaysInclude", { "Scenes" } } });
		// Polled until the write is published (Update), which is where the main thread meets it.
		REQUIRE(project.WaitForWrites());
		CHECK(project.Context.GetProjectFileWriter()->GetWriteCount() == writes + 1);
		CHECK(project.Context.GetProjectFileState() == SettingsFileState::Written);
		CHECK(writingThread->load() != std::thread::id());
		CHECK(writingThread->load() != std::this_thread::get_id());
		CHECK(project.ReadProjectFile()->GetConfig().Export.AlwaysInclude == std::vector<std::string> { "Scenes" });
	}

	TEST_CASE("A failed write keeps the change and reports it; the next change retries")
	{
		SettingsHarness project("ProjectSettingsFailedWrite");
		project.BlockProjectFile();
		const size_t notifications = project.Context.GetNotifications().GetActive().size();

		// The command succeeds: the change is the project's, in memory, while its file is not written.
		project.Run("project.setExportSettings", { { "alwaysInclude", { "Scenes" } } });
		REQUIRE(project.WaitForWrites());
		CHECK(project.Context.GetProjectFileState() == SettingsFileState::Unwritten);
		CHECK(project.Context.HasUnwrittenProjectSettings());
		CHECK_FALSE(project.Context.GetProjectFileError().empty());
		CHECK(project.Context.GetProject()->GetConfig().Export.AlwaysInclude == std::vector<std::string> { "Scenes" });
		const nlohmann::json info = project.Run("project.info");
		CHECK(info["fileState"] == "unwritten");
		CHECK(info["fileError"] == project.Context.GetProjectFileError());
		CHECK(project.Run("editor.status")["project"]["fileState"] == "unwritten");
		REQUIRE(project.Context.GetNotifications().GetActive().size() == notifications + 1);
		const EditorNotification& notification = project.Context.GetNotifications().GetActive().back();
		CHECK(notification.Level == NotificationLevel::Error);
		CHECK(notification.Title.find("Game.stproj") != std::string::npos);

		// project.save fails with the reason while the cause stays.
		const EditorCommandResult failedSave = project.Execute("project.save");
		CHECK_FALSE(failedSave.Success);
		CHECK(failedSave.Error.find("Game.stproj") != std::string::npos);

		// The next change writes both.
		project.UnblockProjectFile();
		project.Run("project.setStartScene", { { "scene", "Scenes/Second.stscene" } });
		REQUIRE(project.WaitForWrites());
		CHECK(project.Context.GetProjectFileState() == SettingsFileState::Written);
		CHECK(project.Run("project.info")["fileError"].is_null());
		const Ref<Project> written = project.ReadProjectFile();
		CHECK(written->GetConfig().Export.AlwaysInclude == std::vector<std::string> { "Scenes" });
		CHECK(written->GetConfig().StartScene == project.Second);

		// So does project.save, which answers once the file is written.
		project.BlockProjectFile();
		project.Run("project.setExportSettings", { { "alwaysInclude", nlohmann::json::array() } });
		REQUIRE(project.WaitForWrites());
		REQUIRE(project.Context.HasUnwrittenProjectSettings());
		project.UnblockProjectFile();
		CHECK(project.Run("project.save")["projectFile"] == FileSystem::ToUTF8(project.ProjectFile));
		CHECK_FALSE(project.Context.HasUnwrittenProjectSettings());
		CHECK(project.ReadProjectFile()->GetConfig().Export.AlwaysInclude.empty());
	}

	TEST_CASE("project.close after a failed write fails unless force")
	{
		SettingsHarness project("ProjectSettingsCloseUnwritten");
		project.BlockProjectFile();
		project.Run("project.setExportSettings", { { "alwaysInclude", { "Scenes" } } });
		REQUIRE(project.WaitForWrites());
		REQUIRE(project.Context.HasUnwrittenProjectSettings());

		const EditorCommandResult refused = project.Execute("project.close");
		CHECK_FALSE(refused.Success);
		CHECK(refused.Error.find("Game.stproj") != std::string::npos);
		CHECK(refused.Error.find("project.save") != std::string::npos);
		CHECK(project.Context.HasProject());
		// So do the other commands that release the project; opening another scene keeps it and loses nothing.
		CHECK_FALSE(project.Execute("editor.quit").Success);
		CHECK_FALSE(project.Context.IsQuitRequested());
		CHECK(project.Execute("scene.open", { { "scene", "Scenes/Main.stscene" } }).Success);
		// With unsaved scene changes as well, the refusal names both, which force would discard together.
		project.Run("entity.create", { { "name", "Unsaved" } });
		const EditorCommandResult both = project.Execute("project.close");
		CHECK_FALSE(both.Success);
		CHECK(both.Error.find("'Main' has unsaved changes") != std::string::npos);
		CHECK(both.Error.find("Game.stproj") != std::string::npos);
		CHECK(project.Context.HasProject());

		const nlohmann::json closed = project.Run("project.close", { { "force", true } });
		CHECK(closed["closed"] == true);
		CHECK(closed["discardedSettings"] == true);
		CHECK(closed["discardedChanges"] == true);
		CHECK_FALSE(project.Context.HasProject());
	}

	TEST_CASE("project.close waits for a write in flight")
	{
		SettingsHarness project("ProjectSettingsCloseWaits");
		// The write is held on its job until the test lets it go.
		auto release = std::make_shared<std::atomic<bool>>(false);
		project.Context.GetProjectFileWriter()->SetTestHook([release]()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!release->load() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		});
		project.Run("project.setExportSettings", { { "alwaysInclude", { "Scenes" } } });
		REQUIRE(project.Context.GetProjectFileState() == SettingsFileState::Writing);

		EditorCommandResult close = project.Commands.Execute(project.Context, "project.close", nlohmann::json::object());
		REQUIRE(close.IsPending());
		const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
		while (std::chrono::steady_clock::now() < until)
		{
			project.Context.Update(Timestep(1.0f / 60.0f));
			PollCommandResult(close, project.Context);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		CHECK(close.IsPending());
		CHECK(project.Context.HasProject());

		release->store(true);
		const EditorCommandResult closed = CompleteCommand(project.Context, std::move(close));
		REQUIRE(closed.Success);
		CHECK(closed.Value["discardedSettings"] == false);
		CHECK_FALSE(project.Context.HasProject());
		CHECK(project.ReadProjectFile()->GetConfig().Export.AlwaysInclude == std::vector<std::string> { "Scenes" });
	}

	TEST_CASE("Changes while playing persist, report no RunningCopy and are undone after play stops")
	{
		SettingsHarness project("ProjectSettingsWhilePlaying");
		project.Run("play.start", { { "build", false } });
		REQUIRE(project.Context.IsPlaying());

		const EditorCommandResult changed = project.Execute("project.setExportSettings", { { "alwaysInclude", { "Scenes" } } });
		REQUIRE(changed.Success);
		CHECK_FALSE(changed.RunningCopy);
		CHECK_FALSE(changed.Value.contains("warning"));
		CHECK(project.Context.GetUndoStack().GetUndoName() == "Set Export Settings");
		// Undo waits for play mode to end.
		CHECK_FALSE(project.Context.Undo());

		project.Run("play.stop");
		CHECK(project.Context.GetProject()->GetConfig().Export.AlwaysInclude == std::vector<std::string> { "Scenes" });
		REQUIRE(project.WaitForWrites());
		CHECK(project.ReadProjectFile()->GetConfig().Export.AlwaysInclude == std::vector<std::string> { "Scenes" });
		CHECK_FALSE(project.Context.IsSceneModified());

		project.Run("edit.undo");
		CHECK(project.Context.GetProject()->GetConfig().Export.AlwaysInclude.empty());
		REQUIRE(project.WaitForWrites());
		CHECK(project.ReadProjectFile()->GetConfig().Export.AlwaysInclude.empty());
	}

	TEST_CASE("setSettings rejects a bad value and changes nothing")
	{
		SettingsRegistry registry;
		RegisterHarnessSections(registry);
		// Two more sections like Test.Complex, which together can reach the project file's bound.
		for (const char* id : { "Test.Large", "Test.Larger" })
		{
			SettingsSection large = MakeComplexTestSection();
			large.Id = id;
			REQUIRE(registry.Register(std::move(large)));
		}
		SettingsHarness project("ProjectSettingsRejects", &registry);
		project.Run("project.setSettings", { { "section", "Test.Simple" }, { "value", { { "Count", 2 } } } });
		REQUIRE(project.WaitForWrites());
		const UndoStack& undo = project.Context.GetUndoStack();
		const size_t steps = undo.GetCount();
		const uint64_t writes = project.Context.GetProjectFileWriter()->GetWriteCount();
		const uint64_t version = project.Context.GetProject()->GetSettings().GetVersion();
		const nlohmann::json stored = project.ReadStoredSettings();

		auto refuse = [&project](const nlohmann::json& parameters, EditorCommandError kind)
		{
			const EditorCommandResult result = project.Execute("project.setSettings", parameters);
			CHECK_FALSE(result.Success);
			CHECK(result.ErrorKind == kind);
			return result.Error;
		};
		// The first bad field is named, also when a valid one comes before it.
		CHECK(refuse({ { "section", "Test.Simple" }, { "value", { { "Speed", 2.0 }, { "Count", 20 } } } }, EditorCommandError::InvalidParameters)
			.find("'Count' must be a whole number from 1 to 8") != std::string::npos);
		CHECK(refuse({ { "section", "Test.Simple" }, { "value", { { "Nope", 1 } } } }, EditorCommandError::InvalidParameters).find("Nope")
			!= std::string::npos);
		CHECK(refuse({ { "section", "Test.Complex" }, { "value", { { "Names", { 1 } } } } }, EditorCommandError::InvalidParameters).find("Names")
			!= std::string::npos);
		const std::string unknown = refuse({ { "section", "Test.Unknown" }, { "value", nlohmann::json::object() } }, EditorCommandError::InvalidParameters);
		CHECK(unknown.find("Test.Simple, Test.Complex") != std::string::npos);
		CHECK(refuse({ { "section", "Test.Preference" }, { "value", nlohmann::json::object() } }, EditorCommandError::InvalidParameters).find("preference")
			!= std::string::npos);
		refuse({ { "section", "Test.Simple" } }, EditorCommandError::InvalidParameters);
		refuse({ { "section", "Test.Simple" }, { "value", nlohmann::json::object() }, { "reset", true } }, EditorCommandError::InvalidParameters);

		// Nothing changed: the values, the history, the file and its writes.
		CHECK(undo.GetCount() == steps);
		CHECK(project.Context.GetProject()->GetSettings().GetVersion() == version);
		CHECK((*project.Context.GetProject()->GetSettings().FindSection("Test.Simple"))["Count"] == 2);
		CHECK(project.Context.GetProjectFileWriter()->GetWriteCount() == writes);
		CHECK(project.Context.GetProjectFileState() == SettingsFileState::Written);
		CHECK(project.ReadStoredSettings() == stored);

		// A value its section accepts that would take the project file over its 1 MiB is refused naming the bound: two sections
		// of almost 512 KiB each, then a third that keeps the settings just under their own bound (compact JSON) and takes the file
		// (indented, with the project's fields) over its.
		const std::string quarter(127 * 1024, 'a');
		project.Run("project.setSettings", { { "section", "Test.Complex" }, { "value", { { "Names", { quarter, quarter, quarter, quarter } } } } });
		project.Run("project.setSettings", { { "section", "Test.Large" }, { "value", { { "Names", { quarter, quarter, quarter, quarter } } } } });
		REQUIRE(project.WaitForWrites());
		REQUIRE(project.Context.GetProjectFileState() == SettingsFileState::Written);
		ProjectSettingsValues probe = project.Context.GetProject()->GetSettings();
		REQUIRE(probe.Set("Test.Larger", { { "Names", { "" } } }, registry));
		const size_t compact = JsonUtils::Dump(probe.ToJson()).size();
		REQUIRE(compact + 8 < ProjectSettingsValues::c_MaxStoredBytes);
		const std::string rest(ProjectSettingsValues::c_MaxStoredBytes - compact - 8, 'b');
		REQUIRE(probe.Set("Test.Larger", { { "Names", { rest } } }, registry));
		const size_t largeSteps = undo.GetCount();
		const uint64_t largeWrites = project.Context.GetProjectFileWriter()->GetWriteCount();
		CHECK(refuse({ { "section", "Test.Larger" }, { "value", { { "Names", { rest } } } } }, EditorCommandError::Failed).find("1 MiB")
			!= std::string::npos);
		CHECK(undo.GetCount() == largeSteps);
		CHECK_FALSE(project.Context.GetProject()->GetSettings().IsStored("Test.Larger"));
		CHECK(project.Context.GetProjectFileWriter()->GetWriteCount() == largeWrites);
		CHECK(project.Context.GetProjectFileState() == SettingsFileState::Written);
	}

	TEST_CASE("One undo step per change, and the scene stays saved")
	{
		SettingsRegistry registry;
		RegisterHarnessSections(registry);
		SettingsHarness project("ProjectSettingsSteps", &registry);
		const UndoStack& undo = project.Context.GetUndoStack();
		const size_t steps = undo.GetCount();
		REQUIRE_FALSE(project.Context.IsSceneModified());

		const nlohmann::json set = project.Run("project.setSettings", { { "section", "Test.Simple" }, { "value", { { "Count", 2 }, { "Mode", "Slow" } } } });
		CHECK(set["section"] == "Test.Simple");
		CHECK(set["value"]["Count"] == 2);
		CHECK(set["value"]["Speed"] == 1.0); // Fields left out take their defaults
		CHECK(set["isDefault"] == false);
		CHECK(undo.GetCount() == steps + 1);
		CHECK(undo.GetUndoName() == "Set Simple");
		CHECK_FALSE(project.Context.IsSceneModified());

		project.Run("project.setSettings", { { "section", "Test.Complex" }, { "value", { { "Names", { "One" } } } } });
		CHECK(undo.GetCount() == steps + 2);
		CHECK_FALSE(project.Context.IsSceneModified());
		// The same value again changes nothing and records nothing.
		project.Run("project.setSettings", { { "section", "Test.Complex" }, { "value", { { "Names", { "One" } } } } });
		CHECK(undo.GetCount() == steps + 2);

		const nlohmann::json reset = project.Run("project.setSettings", { { "section", "Test.Simple" }, { "reset", true } });
		CHECK(reset["isDefault"] == true);
		CHECK(reset["value"]["Count"] == 4);
		CHECK(undo.GetCount() == steps + 3);
		CHECK(undo.GetUndoName() == "Reset Simple");
		CHECK_FALSE(project.Context.IsSceneModified());
		CHECK(project.Run("editor.status")["scene"]["modified"] == false);

		// project.settings describes what applies.
		const nlohmann::json listed = project.Run("project.settings");
		REQUIRE(listed["sections"].size() == 2); // Not the preference
		CHECK(listed["sections"][0]["id"] == "Test.Simple");
		CHECK(listed["sections"][0]["isDefault"] == true);
		CHECK(listed["sections"][0]["schema"]["type"] == "object");
		CHECK(listed["sections"][1]["value"]["Names"] == nlohmann::json { "One" });
		CHECK(listed["sections"][1]["problem"].is_null());
		CHECK(listed["project"]["startScene"] == UUIDToJson(project.Main));
		CHECK(listed["unclaimed"].empty());
		const nlohmann::json one = project.Run("project.settings", { { "section", "Test.Complex" } });
		REQUIRE(one["sections"].size() == 1);
		CHECK(one["sections"][0]["version"] == 2);
		CHECK_FALSE(project.Execute("project.settings", { { "section", "Test.Preference" } }).Success);

		// The editor provides the open project's sections to the engine's modules.
		IProjectSettings* provided = EngineServices::Get<IProjectSettings>();
		REQUIRE(provided);
		REQUIRE(provided->FindSection("Test.Complex"));
		CHECK((*provided->FindSection("Test.Complex"))["Names"] == nlohmann::json { "One" });
		CHECK_FALSE(provided->FindSection("Test.Simple"));
	}

	TEST_CASE("Undo and redo restore the section and rewrite the file")
	{
		SettingsRegistry registry;
		RegisterHarnessSections(registry);
		const std::filesystem::path projectFile = SettingsHarness("ProjectSettingsUndoSections", &registry).ProjectFile;
		// The file stores a value its section refuses, and one no section of this editor claims: both stay as stored.
		{
			std::optional<std::string> text = FileSystem::ReadText(projectFile);
			REQUIRE(text);
			std::optional<nlohmann::json> document = JsonUtils::Parse(*text);
			REQUIRE(document);
			(*document)["Settings"] = { { "Test.Simple", { { "Version", 1 }, { "Value", { { "Count", 20 } } } } },
				{ "Other.Section", { { "Version", 1 }, { "Value", 7 } } } };
			REQUIRE(FileSystem::WriteText(projectFile, JsonUtils::Dump(*document, 1, '\t')));
		}
		WriterJobSystem jobs;
		EditorContext context(MakeSpecification(&registry));
		EditorCommandRegistry commands;
		const nlohmann::json opened = RunCommandToCompletion(context, commands, "project.open", { { "path", FileSystem::ToUTF8(projectFile) } });
		REQUIRE(opened["settingsProblems"].size() == 1);
		CHECK(opened["settingsProblems"][0]["section"] == "Test.Simple");
		CHECK(opened["settingsProblems"][0]["message"].get<std::string>().find("Count") != std::string::npos);
		const nlohmann::json described = RunCommandToCompletion(context, commands, "project.settings", { { "section", "Test.Simple" } });
		CHECK(described["sections"][0]["problem"].get<std::string>().find("Count") != std::string::npos);
		CHECK(described["sections"][0]["isDefault"] == true);
		CHECK(RunCommandToCompletion(context, commands, "project.settings")["unclaimed"] == nlohmann::json { "Other.Section" });
		const nlohmann::json original = context.GetProject()->GetSettings().ToJson();

		auto waitForWrites = [&context]()
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (context.GetProjectFileState() == SettingsFileState::Writing && std::chrono::steady_clock::now() < deadline)
			{
				context.Update(Timestep(1.0f / 60.0f));
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			REQUIRE(context.GetProjectFileState() == SettingsFileState::Written);
		};
		auto stored = [&projectFile]()
		{
			std::string error;
			const Ref<Project> project = Project::Load(projectFile, &error);
			REQUIRE_MESSAGE(project, error);
			return project->GetSettings().ToJson();
		};

		RunCommandToCompletion(context, commands, "project.setSettings", { { "section", "Test.Simple" }, { "value", { { "Count", 6 } } } });
		waitForWrites();
		CHECK(stored()["Test.Simple"]["Value"]["Count"] == 6);
		CHECK(stored()["Other.Section"]["Value"] == 7);

		// Undo puts back the value as it was stored, refused by its section again.
		REQUIRE(context.Undo());
		CHECK_FALSE(context.GetProject()->GetSettings().FindSection("Test.Simple"));
		waitForWrites();
		CHECK(stored() == original);
		CHECK(RunCommandToCompletion(context, commands, "project.settings", { { "section", "Test.Simple" } })["sections"][0]["problem"].is_string());

		REQUIRE(context.Redo());
		CHECK((*context.GetProject()->GetSettings().FindSection("Test.Simple"))["Count"] == 6);
		waitForWrites();
		CHECK(stored()["Test.Simple"]["Value"]["Count"] == 6);

		// A reset removes the section from the file, and its undo brings it back.
		RunCommandToCompletion(context, commands, "project.setSettings", { { "section", "Test.Simple" }, { "reset", true } });
		waitForWrites();
		CHECK_FALSE(stored().contains("Test.Simple"));
		REQUIRE(context.Undo());
		waitForWrites();
		CHECK(stored()["Test.Simple"]["Value"]["Count"] == 6);
	}

	TEST_CASE("Deleting the start or loading scene is refused while the project names it")
	{
		SettingsHarness project("ProjectSettingsReferencedScenes");
		project.Run("scene.saveAs", { { "path", "Scenes/Loading.stscene" } });
		project.Run("scene.open", { { "scene", "Scenes/Second.stscene" } });
		project.Run("project.setExportSettings", { { "loadingScene", "Scenes/Loading.stscene" } });

		for (const char* scene : { "Scenes/Main.stscene", "Scenes/Loading.stscene" })
		{
			CAPTURE(scene);
			const EditorCommandResult refused = project.Execute("asset.delete", { { "asset", scene } });
			REQUIRE_FALSE(refused.Success);
			CHECK(refused.Error.find("Game.stproj") != std::string::npos);
			const nlohmann::json references = project.Run("asset.findReferences", { { "asset", scene } })["referencedBy"];
			REQUIRE_FALSE(references.empty());
			CHECK(references[0]["type"] == "Project");
			CHECK(references[0]["project"] == true);
			CHECK(references[0]["path"] == "Game.stproj");
			CHECK(references[0]["asset"].is_null());
		}
		CHECK(project.Run("asset.findReferences", { { "asset", "Scenes/Main.stscene" } })["referencedBy"][0]["settings"] == nlohmann::json { "startScene" });
		CHECK(project.Run("asset.findReferences", { { "asset", "Scenes/Loading.stscene" } })["referencedBy"][0]["settings"]
			== nlohmann::json { "loadingScene" });

		// Once the project no longer names it, it can go.
		project.Run("project.setExportSettings", { { "loadingScene", nullptr } });
		project.Run("asset.delete", { { "asset", "Scenes/Loading.stscene" } });
	}

	TEST_CASE("project.setGameInfo sets the game's identity as one undo step")
	{
		SettingsHarness project("ProjectSettingsGameInfo");
		const std::string icon = project.ImportTexture("Icon");
		const UndoStack& undo = project.Context.GetUndoStack();
		const size_t steps = undo.GetCount();
		const nlohmann::json game = project.Run("project.setGameInfo", { { "title", "Star Adventure" }, { "version", "1.2.0" }, { "company", "Studio" },
			{ "copyright", "(c) 2026 Studio" }, { "icon", icon } })["game"];
		CHECK(game == nlohmann::json { { "title", "Star Adventure" }, { "version", "1.2.0" }, { "company", "Studio" }, { "copyright", "(c) 2026 Studio" },
			{ "icon", icon } });
		CHECK(undo.GetCount() == steps + 1);
		CHECK_FALSE(project.Context.IsSceneModified());
		CHECK(project.Run("project.info")["game"] == game);
		CHECK(project.Run("project.settings")["project"]["game"] == game);
		REQUIRE(project.WaitForWrites());
		CHECK(project.ReadProjectFile()->GetConfig().Game.Title == "Star Adventure");

		// Fields left out keep their values; an invalid field changes nothing, also beside a valid one.
		CHECK(project.Run("project.setGameInfo", { { "version", "1.3" } })["game"]["title"] == "Star Adventure");
		for (const nlohmann::json& parameters : { nlohmann::json { { "version", "2.0" }, { "title", "Two\nlines" } }, nlohmann::json { { "version", "1.0-beta" } },
			nlohmann::json { { "company", std::string(129, 'c') } }, nlohmann::json { { "icon", "Textures/Missing.png" } }, nlohmann::json { { "icon", "Scenes/Main.stscene" } },
			nlohmann::json::object(), nlohmann::json { { "title", 7 } } })
		{
			CAPTURE(parameters.dump());
			const EditorCommandResult refused = project.Execute("project.setGameInfo", parameters);
			CHECK_FALSE(refused.Success);
			CHECK(refused.ErrorKind == EditorCommandError::InvalidParameters);
		}
		CHECK(project.Context.GetProject()->GetConfig().Game.Version == "1.3");
		CHECK(undo.GetCount() == steps + 2);

		// Undo puts the values back; null clears the icon.
		project.Run("edit.undo");
		CHECK(project.Context.GetProject()->GetConfig().Game.Version == "1.2.0");
		CHECK(project.Run("project.setGameInfo", { { "icon", nullptr } })["game"]["icon"].is_null());
	}

	TEST_CASE("project.setGameInfo refuses a sub-asset texture and an icon source it cannot convert")
	{
		SettingsHarness project("ProjectSettingsGameIcon");
		// A model whose texture comes from Textures/Checker.png: the model's texture is a sub-asset, the PNG a texture of its own.
		const std::filesystem::path features = Tests::GetFeatureProjectSourceDirectory() / "Assets";
		const std::string checker = project.Run("asset.import", { { "file", FileSystem::ToUTF8(features / "Textures" / "Checker.png") },
			{ "directory", "Textures" } })["asset"].get<std::string>();
		const std::string model = project.Run("asset.import", { { "file", FileSystem::ToUTF8(features / "Models" / "Platform.gltf") },
			{ "directory", "Models" } })["asset"].get<std::string>();
		const nlohmann::json info = project.Run("asset.info", { { "asset", model }, { "bounds", false } });
		std::string subTexture;
		for (const nlohmann::json& subAsset : info["subAssets"])
		{
			if (subAsset["type"] == "Texture")
				subTexture = subAsset["asset"].get<std::string>();
		}
		REQUIRE_FALSE(subTexture.empty());
		// An HDR image is a texture, but no icon.
		const std::filesystem::path hdr = CreateTemporaryDirectory("ProjectSettingsGameIconHDR") / "Sky.hdr";
		std::string radiance = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
		radiance += std::string("\x80\x80\x80\x81\x80\x80\x80\x81", 8);
		REQUIRE(FileSystem::WriteText(hdr, radiance));
		const std::string sky = project.Run("asset.import", { { "file", FileSystem::ToUTF8(hdr) }, { "directory", "Textures" } })["asset"].get<std::string>();

		const size_t steps = project.Context.GetUndoStack().GetCount();
		const EditorCommandResult inside = project.Execute("project.setGameInfo", { { "icon", subTexture } });
		CHECK_FALSE(inside.Success);
		CHECK(inside.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(inside.Error.find("is a texture inside '") != std::string::npos);
		CHECK(inside.Error.find("Platform.gltf', which has no image file of its own") != std::string::npos);
		const EditorCommandResult notAnImage = project.Execute("project.setGameInfo", { { "icon", sky } });
		CHECK_FALSE(notAnImage.Success);
		CHECK(notAnImage.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(notAnImage.Error.find("'Textures/Sky.hdr' is not a PNG, JPEG, TGA or BMP image") != std::string::npos);
		CHECK(project.Context.GetUndoStack().GetCount() == steps);
		CHECK_FALSE(project.Context.GetProject()->GetConfig().Game.Icon.IsValid());

		// The PNG the model uses is a texture of its own: that one is accepted.
		CHECK(project.Run("project.setGameInfo", { { "icon", checker } })["game"]["icon"] == checker);
	}

	TEST_CASE("A section registered by a game appears in project.settings and is validated by its fields")
	{
		if (!IsSettingsSectionProcess())
			return RunInSettingsSectionProcess();
		SettingsHarness project("ProjectSettingsGameSection");
		const nlohmann::json listed = project.Run("project.settings", { { "section", c_GameTestSection } });
		REQUIRE(listed["sections"].size() == 1);
		const nlohmann::json& section = listed["sections"][0];
		CHECK(section["title"] == "Tuning");
		CHECK(section["category"] == "Game");
		CHECK(section["isDefault"] == true);
		CHECK(section["value"] == nlohmann::json { { "Difficulty", "Normal" }, { "Lives", 3 }, { "Banner", nullptr } });
		CHECK(section["schema"]["properties"]["Difficulty"]["enum"] == nlohmann::json { "Easy", "Normal", "Hard" });

		const EditorCommandResult refused = project.Execute("project.setSettings", { { "section", c_GameTestSection }, { "value", { { "Lives", 12 } } } });
		CHECK(refused.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(refused.Error.find("'Lives'") != std::string::npos);
		CHECK(project.Execute("project.setSettings", { { "section", c_GameTestSection }, { "value", { { "Difficulty", "Brutal" } } } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(project.Execute("project.setSettings", { { "section", c_GameTestSection }, { "value", { { "Banner", "Textures/None.png" } } } }).ErrorKind
			== EditorCommandError::InvalidParameters);

		project.Run("project.setSettings", { { "section", c_GameTestSection }, { "value", { { "Difficulty", "Hard" }, { "Lives", 5 } } } });
		IProjectSettings* provided = EngineServices::Get<IProjectSettings>();
		REQUIRE(provided);
		REQUIRE(provided->FindSection(c_GameTestSection));
		CHECK((*provided->FindSection(c_GameTestSection))["Lives"] == 5);
		REQUIRE(project.WaitForWrites());
		CHECK(project.ReadStoredSettings()[c_GameTestSection]["Value"]["Difficulty"] == "Hard");
	}

	TEST_CASE("Deleting an asset a section names is refused")
	{
		if (!IsSettingsSectionProcess())
			return RunInSettingsSectionProcess();
		SettingsHarness project("ProjectSettingsReferencedAsset");
		const std::string banner = project.ImportTexture("Banner");
		project.Run("project.setSettings", { { "section", c_GameTestSection }, { "value", { { "Banner", banner } } } });

		const EditorCommandResult refused = project.Execute("asset.delete", { { "asset", banner } });
		REQUIRE_FALSE(refused.Success);
		CHECK(refused.Error.find("Tuning") != std::string::npos);
		const nlohmann::json references = project.Run("asset.findReferences", { { "asset", banner } })["referencedBy"];
		REQUIRE(references.size() == 1);
		CHECK(references[0]["settings"] == nlohmann::json { c_GameTestSection });

		project.Run("project.setSettings", { { "section", c_GameTestSection }, { "reset", true } });
		project.Run("asset.delete", { { "asset", banner } });
	}

	TEST_CASE("project.open reports settingsProblems for a section that fails Read")
	{
		if (!IsSettingsSectionProcess())
			return RunInSettingsSectionProcess();
		const std::filesystem::path projectFile = SettingsHarness("ProjectSettingsOpenProblems").ProjectFile;
		{
			std::optional<std::string> text = FileSystem::ReadText(projectFile);
			REQUIRE(text);
			std::optional<nlohmann::json> document = JsonUtils::Parse(*text);
			REQUIRE(document);
			(*document)["Settings"] = { { c_GameTestSection, { { "Version", 1 }, { "Value", { { "Lives", 99 } } } } } };
			REQUIRE(FileSystem::WriteText(projectFile, JsonUtils::Dump(*document, 1, '\t')));
		}
		WriterJobSystem jobs;
		EditorContext context(MakeSpecification(nullptr));
		EditorCommandRegistry commands;
		const nlohmann::json opened = RunCommandToCompletion(context, commands, "project.open", { { "path", FileSystem::ToUTF8(projectFile) } });
		REQUIRE(opened["settingsProblems"].size() == 1);
		CHECK(opened["settingsProblems"][0]["section"] == c_GameTestSection);
		CHECK(opened["settingsProblems"][0]["message"].get<std::string>().find("Lives") != std::string::npos);
		// Its default applies meanwhile; setting it again fixes it.
		CHECK(RunCommandToCompletion(context, commands, "project.settings", { { "section", c_GameTestSection } })["sections"][0]["value"]["Lives"] == 3);
		RunCommandToCompletion(context, commands, "project.setSettings", { { "section", c_GameTestSection }, { "value", { { "Lives", 4 } } } });
		CHECK(RunCommandToCompletion(context, commands, "project.settings", { { "section", c_GameTestSection } })["sections"][0]["problem"].is_null());
	}
}
