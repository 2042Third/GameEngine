#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/HarnessEditor.h"
#include "Editor/SceneRecovery.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Autosave and crash recovery: the journal of the edited scene's unsaved changes (SceneRecovery) and scene.recover.

using namespace Strata;

namespace
{

	EditorContextSpecification MakeSpecification()
	{
		EditorContextSpecification specification;
		specification.WatchAssetFiles = false;
		specification.HotReloadScripts = false;
		return specification;
	}

	// An editor on a basic3d project, whose saved start scene is Scenes/Main.stscene.
	struct RecoveryHarness
	{
		std::filesystem::path Project;
		EditorContext Context { MakeSpecification() };
		EditorCommandRegistry Commands;

		explicit RecoveryHarness(const std::filesystem::path& project, bool create = true)
			: Project(project)
		{
			if (create)
				Run("project.create", { { "directory", FileSystem::ToUTF8(Project) }, { "name", "Game" }, { "template", "basic3d" } });
			else
				Run("project.open", { { "path", FileSystem::ToUTF8(Project) } });
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Tests::RunCommandToCompletion(Context, Commands, name, parameters);
		}

		EditorCommandResult Execute(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
		}

		// Writes the journal now and waits for the disk.
		void FlushJournal()
		{
			Context.GetRecovery().Flush(*Context.GetEditScene(), Context.IsSceneModified());
			Context.GetRecovery().Wait();
		}

		std::filesystem::path GetRecoveryDirectory() const { return Project / ".strata" / SceneRecovery::c_DirectoryName; }

		// The scene as a document without its name (a recovered scene that was never saved gets its file's name).
		nlohmann::json Snapshot() const
		{
			nlohmann::json document = SceneSerializer::Serialize(*Context.GetEditScene());
			return document["Scene"]["Entities"];
		}

		nlohmann::json Settings() const
		{
			return SceneRecovery::SerializeSettings(Context.GetEditScene()->GetSettings());
		}
	};

	std::vector<std::filesystem::path> ListRecoveryFiles(const std::filesystem::path& directory)
	{
		std::vector<std::filesystem::path> files;
		std::error_code error;
		for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
		{
			if (it->is_regular_file(error) && FileSystem::ToUTF8(it->path().extension()) == SceneRecovery::c_FileExtension)
				files.push_back(it->path());
		}
		return files;
	}

	nlohmann::json ReadRecoveryHeader(const std::filesystem::path& file)
	{
		const std::optional<std::string> text = FileSystem::ReadText(file);
		REQUIRE(text.has_value());
		const std::optional<nlohmann::json> header = JsonUtils::Parse(std::string_view(*text).substr(0, text->find('\n')));
		REQUIRE(header.has_value());
		return *header;
	}

	// Edits of every kind a journal must replay: creating, renaming, component values, reparenting, reordering, deleting
	// and the scene's settings.
	void MakeEdits(RecoveryHarness& harness)
	{
		const std::string crate = harness.Run("entity.create", { { "name", "Crate" }, { "components", { { "Transform", { { "Translation", { 1, 2, 3 } } } } } } })["id"].get<std::string>();
		const std::string lamp = harness.Run("entity.create", { { "name", "Lamp" }, { "parent", crate }, { "components", { { "PointLight", { { "Intensity", 4 } } } } } })["id"].get<std::string>();
		const std::string first = harness.Run("entity.create", { { "name", "First" } })["id"].get<std::string>();
		harness.Run("entity.setParent", { { "entity", first }, { "index", 0 } }); // Moves the others along
		harness.Run("component.set", { { "entity", lamp }, { "component", "PointLight" }, { "values", { { "Range", 7 } } } });
		harness.Run("entity.rename", { { "entity", crate }, { "name", "Big Crate" } });
		const std::string sun = harness.Context.GetEditScene()->FindEntityByName("Sun").GetUUID().ToString();
		harness.Run("entity.delete", { { "entities", { sun } } });
		harness.Run("scene.setSettings", { { "gravity", { 0, -3, 0 } } });
		harness.Run("component.set", { { "entity", crate }, { "component", "Transform" }, { "values", { { "Translation", { 4, 5, 6 } } } } });
	}

}

TEST_SUITE("Editor.Recovery")
{
	TEST_CASE("The journal of a saved scene holds its unsaved changes until they are saved or undone")
	{
		RecoveryHarness harness(Tests::CreateTemporaryDirectory("RecoveryJournal") / "Game");
		CHECK(ListRecoveryFiles(harness.GetRecoveryDirectory()).empty());

		harness.Run("entity.create", { { "name", "Crate" } });
		// Not at once: a few seconds after the edits stop (or with Flush).
		harness.Context.Update(Timestep(1.0f / 60.0f));
		harness.Context.GetRecovery().Wait();
		CHECK(ListRecoveryFiles(harness.GetRecoveryDirectory()).empty());
		CHECK(harness.Context.GetRecovery().GetStatus().Pending);

		harness.FlushJournal();
		const std::vector<std::filesystem::path> files = ListRecoveryFiles(harness.GetRecoveryDirectory());
		REQUIRE(files.size() == 1);
		const nlohmann::json header = ReadRecoveryHeader(files[0]);
		CHECK(header["SceneName"] == "Main");
		CHECK(header["ScenePath"] == "Scenes/Main.stscene");
		CHECK(header["Discarded"] == false);
		CHECK(header["BaseHash"].is_string());
		CHECK(harness.Run("editor.status")["recovery"]["journal"]["writes"] == 1);

		// Undoing back to the saved scene leaves nothing to recover.
		harness.Run("edit.undo");
		harness.FlushJournal();
		CHECK(ListRecoveryFiles(harness.GetRecoveryDirectory()).empty());

		// Saving removes it too.
		harness.Run("edit.redo");
		harness.FlushJournal();
		CHECK(ListRecoveryFiles(harness.GetRecoveryDirectory()).size() == 1);
		harness.Run("scene.save");
		harness.Context.GetRecovery().Wait();
		CHECK(ListRecoveryFiles(harness.GetRecoveryDirectory()).empty());
	}

	TEST_CASE("The journal is written a few seconds after edits stop")
	{
		RecoveryHarness harness(Tests::CreateTemporaryDirectory("RecoveryTiming") / "Game");
		harness.Run("entity.create", { { "name", "Crate" } });
		const auto start = std::chrono::steady_clock::now();
		CHECK(Tests::WaitUntil([&]()
		{
			harness.Context.Update(Timestep(1.0f / 60.0f));
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			return !ListRecoveryFiles(harness.GetRecoveryDirectory()).empty();
		}, std::chrono::seconds(10)));
		CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= SceneRecovery::c_QuietSeconds);
	}

	TEST_CASE("Discarded changes are offered and recovered as one undo step back to the saved scene")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryApply") / "Game";
		nlohmann::json saved;
		nlohmann::json edited;
		nlohmann::json editedSettings;
		{
			RecoveryHarness harness(project);
			saved = harness.Snapshot();
			MakeEdits(harness);
			edited = harness.Snapshot();
			editedSettings = harness.Settings();
			// A forced close writes the changes as recovery data first.
			CHECK(harness.Run("project.close", { { "force", true } })["discardedChanges"] == true);
		}

		RecoveryHarness harness(project, false);
		const nlohmann::json status = harness.Run("scene.recoveryStatus");
		REQUIRE(status["entries"].size() == 1);
		const nlohmann::json entry = status["entries"][0];
		CHECK(entry["sceneName"] == "Main");
		CHECK(entry["discarded"] == true);
		CHECK(entry["applicable"] == true);
		CHECK(harness.Run("editor.status")["recovery"]["applicable"] == 1);

		CHECK(harness.Snapshot() == saved);
		const nlohmann::json recovered = harness.Run("scene.recover", { { "scene", "Scenes/Main.stscene" }, { "action", "apply" } });
		CHECK(recovered["recovered"] == entry["id"]);
		CHECK(recovered["discardedChanges"] == false);
		CHECK(harness.Snapshot() == edited);
		CHECK(harness.Settings() == editedSettings);
		CHECK(harness.Context.IsSceneModified());
		CHECK(harness.Context.GetUndoStack().GetHistory() == std::vector<std::string> { "Recover Unsaved Changes" });

		// The edited scene's journal holds the changes now; the file they came from is gone.
		harness.Context.GetRecovery().Wait();
		const std::vector<std::filesystem::path> files = ListRecoveryFiles(harness.GetRecoveryDirectory());
		REQUIRE(files.size() == 1);
		CHECK(FileSystem::ToUTF8(files[0].stem()) != entry["id"].get<std::string>());
		CHECK(harness.Run("scene.recoveryStatus")["entries"].empty()); // This editor's own journal is not offered

		harness.Run("edit.undo");
		CHECK(harness.Snapshot() == saved);
		CHECK_FALSE(harness.Context.IsSceneModified());
		harness.Run("edit.redo");
		CHECK(harness.Snapshot() == edited);
	}

	TEST_CASE("A scene that was never saved is recovered from a snapshot into a new scene")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryUntitled") / "Game";
		nlohmann::json edited;
		{
			RecoveryHarness harness(project);
			harness.Run("scene.new", { { "name", "Sketch" }, { "template", "basic3d" } });
			harness.Run("entity.create", { { "name", "Idea" } });
			edited = harness.Snapshot();
			harness.Run("scene.open", { { "scene", "Scenes/Main.stscene" }, { "force", true } });
			harness.Context.GetRecovery().Wait();
		}

		RecoveryHarness harness(project, false);
		const nlohmann::json entries = harness.Run("scene.recoveryStatus")["entries"];
		REQUIRE(entries.size() == 1);
		CHECK(entries[0]["scene"].is_null());
		CHECK(entries[0]["complete"] == true);
		CHECK(entries[0]["sceneName"] == "Sketch");
		harness.Run("scene.recover", { { "scene", entries[0]["id"] }, { "action", "apply" } });
		CHECK(harness.Context.GetEditScene()->GetName() == "Sketch");
		CHECK_FALSE(harness.Context.GetSceneHandle().IsValid());
		CHECK(harness.Snapshot() == edited);
		harness.Run("edit.undo");
		CHECK(harness.Context.GetEditScene()->GetEntityCount() == 0);
	}

	TEST_CASE("Recovery data of a scene that changed since is not applied, and can be discarded")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryOutdated") / "Game";
		{
			RecoveryHarness harness(project);
			harness.Run("entity.create", { { "name", "Lost" } });
			harness.Run("project.close", { { "force", true } });
		}

		RecoveryHarness harness(project, false);
		// Another editor saved the scene meanwhile.
		harness.Run("entity.create", { { "name", "Other" } });
		harness.Run("scene.save");
		const nlohmann::json entries = harness.Run("scene.recoveryStatus")["entries"];
		REQUIRE(entries.size() == 1);
		CHECK(entries[0]["applicable"] == false);
		CHECK(entries[0]["problem"].get<std::string>().find("changed") != std::string::npos);

		const EditorCommandResult refused = harness.Execute("scene.recover", { { "scene", entries[0]["id"] }, { "action", "apply" } });
		CHECK_FALSE(refused.Success);
		CHECK(refused.ErrorKind == EditorCommandError::Failed);
		CHECK_FALSE(harness.Context.GetEditScene()->FindEntityByName("Lost"));

		const EditorCommandResult unknown = harness.Execute("scene.recover", { { "scene", "NoSuchRecovery" }, { "action", "apply" } });
		CHECK(unknown.ErrorKind == EditorCommandError::InvalidParameters);

		CHECK(harness.Run("scene.recover", { { "scene", entries[0]["id"] }, { "action", "discard" } })["discarded"] == entries[0]["id"]);
		harness.Context.GetRecovery().Wait();
		CHECK(harness.Run("scene.recoveryStatus")["entries"].empty());
		CHECK(ListRecoveryFiles(harness.GetRecoveryDirectory() / SceneRecovery::c_DiscardedDirectoryName).size() == 1);
	}

	TEST_CASE("Applying recovery data guards the edited scene's own unsaved changes")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryGuard") / "Game";
		{
			RecoveryHarness harness(project);
			harness.Run("entity.create", { { "name", "Recovered" } });
			harness.Run("project.close", { { "force", true } });
		}
		RecoveryHarness harness(project, false);
		harness.Run("entity.create", { { "name", "Current" } });
		const EditorCommandResult refused = harness.Execute("scene.recover", { { "scene", "Scenes/Main.stscene" }, { "action", "apply" } });
		CHECK_FALSE(refused.Success);
		CHECK(refused.Error.find("force: true") != std::string::npos);
		CHECK(harness.Context.GetEditScene()->FindEntityByName("Current"));

		const nlohmann::json applied = harness.Run("scene.recover", { { "scene", "Scenes/Main.stscene" }, { "action", "apply" }, { "force", true } });
		CHECK(applied["discardedChanges"] == true);
		CHECK(harness.Context.GetEditScene()->FindEntityByName("Recovered"));
		CHECK_FALSE(harness.Context.GetEditScene()->FindEntityByName("Current"));
		// What the force discarded is recovery data in turn.
		harness.Context.GetRecovery().Wait();
		CHECK(harness.Run("scene.recoveryStatus")["entries"].size() == 1);
	}

	TEST_CASE("A scene that was never saved and starts too large for a snapshot is not journaled, and the status says why")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecoveryLarge");
		SceneRecovery recovery;
		recovery.SetProject(directory / "Recovery", directory / "Assets");
		Scene scene("Huge");
		for (size_t index = 0; index <= SceneRecovery::c_MaxSnapshotEntities; index++)
			scene.CreateEntity("Bulk");
		recovery.BeginScene(scene, UUID::Null(), {}, {});
		CHECK(recovery.GetStatus().Skipped.find("never saved") != std::string::npos);

		UndoStack undo;
		undo.SetObserver(&recovery);
		SceneEditTransaction transaction(scene, "Create Entity", {});
		transaction.TrackCreated(scene.CreateEntity("Edit").GetUUID());
		REQUIRE(transaction.Commit(undo));
		CHECK_FALSE(recovery.Flush(scene, true));
		recovery.Wait();
		CHECK(ListRecoveryFiles(directory / "Recovery").empty());
		undo.SetObserver(nullptr);
	}

	TEST_CASE("A step only a snapshot captures turns the journal into a snapshot of the scene")
	{
		// An action that changes the scene without describing its effect as entity states.
		class RenameEverything final : public EditorAction
		{
		public:
			explicit RenameEverything(Scene& scene)
				: m_Scene(scene)
			{
			}

			const std::string& GetName() const override
			{
				static const std::string s_Name = "Rename Everything";
				return s_Name;
			}

			bool Execute(std::string*) override
			{
				Rename("Renamed");
				return true;
			}

			void Undo() override { Rename("Entity"); }
		private:
			void Rename(const std::string& name)
			{
				for (Entity entity : m_Scene.GetEntitiesInHierarchyOrder())
				{
					entity.GetComponent<NameComponent>().Name = name;
					entity.MarkModified<NameComponent>();
				}
			}
		private:
			Scene& m_Scene;
		};

		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryOther") / "Game";
		nlohmann::json edited;
		{
			RecoveryHarness harness(project);
			harness.Run("entity.create", { { "name", "Before" } });
			REQUIRE(harness.Context.GetUndoStack().Execute(CreateScope<RenameEverything>(*harness.Context.GetEditScene())));
			harness.Run("entity.create", { { "name", "After" } });
			edited = harness.Snapshot();
			harness.Run("project.close", { { "force", true } });
		}

		RecoveryHarness harness(project, false);
		const nlohmann::json entries = harness.Run("scene.recoveryStatus")["entries"];
		REQUIRE(entries.size() == 1);
		CHECK(entries[0]["complete"] == true);
		harness.Run("scene.recover", { { "scene", entries[0]["id"] }, { "action", "apply" } });
		CHECK(harness.Snapshot() == edited);
	}

	TEST_CASE("The editor offers recovery data when the project opens, and Recover applies it")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryOffer") / "Game";
		{
			RecoveryHarness harness(project);
			harness.Run("entity.create", { { "name", "Recovered" } });
			harness.Run("project.close", { { "force", true } });
		}

		EditorOptions options;
		options.ProjectPath = project;
		Tests::HarnessEditor editor({}, options);
		REQUIRE(editor.FramesUntil([&]() { return editor.Layer->GetRecoveryDialog().IsOpen(); }));
		// A dialog that sizes itself to its contents settles over its first frames.
		editor.Frames(3);
		const std::optional<UI::ItemProbe::Item> recover = UI::ItemProbe::Find("Recovery.Recover.0");
		REQUIRE(recover.has_value());
		const std::vector<RecoveryEntry>& entries = editor.Context().GetRecovery().GetEntries();
		REQUIRE(entries.size() == 1);
		INFO(SceneRecovery::DescribeEntry(entries[0]).dump());
		REQUIRE(recover->Enabled);
		CHECK_FALSE(editor.Context().GetEditScene()->FindEntityByName("Recovered"));

		REQUIRE(editor.Click("Recovery.Recover.0"));
		REQUIRE(editor.FramesUntil([&]() { return editor.Context().GetEditScene()->FindEntityByName("Recovered").IsValid(); }));
		CHECK(editor.Context().IsSceneModified());
		editor.Frames(2);
		CHECK_FALSE(editor.Layer->GetRecoveryDialog().IsOpen());

		// Once recovered, nothing is offered: the File menu item is off.
		REQUIRE(editor.Click("Menu.File"));
		editor.Frames(1);
		const std::optional<UI::ItemProbe::Item> item = UI::ItemProbe::Find("Menu.File.Recover");
		REQUIRE(item.has_value());
		CHECK_FALSE(item->Enabled);
	}

	TEST_CASE("Damaged recovery files are reported, not applied")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryDamaged") / "Game";
		{
			RecoveryHarness harness(project);
			harness.Run("entity.create", { { "name", "Crate" } });
			harness.Run("project.close", { { "force", true } });
		}
		const std::vector<std::filesystem::path> files = ListRecoveryFiles(project / ".strata" / SceneRecovery::c_DirectoryName);
		REQUIRE(files.size() == 1);
		const std::string text = *FileSystem::ReadText(files[0]);
		// The body cut short: the header still reads, the steps do not.
		REQUIRE(FileSystem::WriteText(files[0], text.substr(0, text.find('\n') + 20)));
		REQUIRE(FileSystem::WriteText(files[0].parent_path() / ("Garbage" + std::string(SceneRecovery::c_FileExtension)), "not a recovery file"));

		RecoveryHarness harness(project, false);
		const nlohmann::json entries = harness.Run("scene.recoveryStatus")["entries"];
		REQUIRE(entries.size() == 1); // The garbage file is no recovery data
		const EditorCommandResult result = harness.Execute("scene.recover", { { "scene", entries[0]["id"] }, { "action", "apply" } });
		CHECK_FALSE(result.Success);
		CHECK(result.Error.find("damaged") != std::string::npos);
		CHECK_FALSE(harness.Context.IsSceneModified());
	}
}
