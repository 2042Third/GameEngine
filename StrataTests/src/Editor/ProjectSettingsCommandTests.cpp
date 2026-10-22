#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/SettingsFileWriter.h"
#include "Editor/UndoStack.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JobSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Project/Project.h>

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

	struct SettingsHarness
	{
		WriterJobSystem Jobs; // Before the context, which it outlives
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		std::filesystem::path ProjectFile;
		AssetHandle Main = UUID::Null();
		AssetHandle Second = UUID::Null();

		explicit SettingsHarness(const std::string& name)
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
	};

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

		const nlohmann::json closed = project.Run("project.close", { { "force", true } });
		CHECK(closed["closed"] == true);
		CHECK(closed["discardedSettings"] == true);
		CHECK(closed["discardedChanges"] == false);
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
}
