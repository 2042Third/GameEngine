#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorNotifications.h"
#include "Editor/HarnessEditor.h"
#include "Editor/PropertyEdit.h"
#include "Editor/SceneRecovery.h"
#include "Perf/SceneGenerators.h"
#include "TestHelpers.h"
#include "UI/ItemProbe.h"
#include "UI/RecoveryDialog.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Platform.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <functional>
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

		EditorCommandResult Execute(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object(), const CommandOrigin& origin = {})
		{
			return Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters, origin));
		}

		// Frames until the entries the scans found satisfy `condition`.
		bool UpdateUntil(const std::function<bool(const std::vector<RecoveryEntry>&)>& condition)
		{
			return Tests::WaitUntil([&]()
			{
				Context.Update(Timestep(1.0f / 60.0f));
				Context.GetRecovery().Wait();
				return !Context.GetRecovery().IsScanning() && condition(Context.GetRecovery().GetEntries());
			}, std::chrono::seconds(10));
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

	TEST_CASE("A journal of an editor that is gone is offered, also when its process id is this process's")
	{
		// Process ids are reused: an editor that crashed may have had the id this process has now, and only the start time
		// tells the two apart.
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryReusedId") / "Game";
		std::filesystem::path file;
		nlohmann::json header;
		std::string body;
		{
			RecoveryHarness harness(project);
			harness.Run("entity.create", { { "name", "Crate" } });
			harness.FlushJournal();
			const std::vector<std::filesystem::path> files = ListRecoveryFiles(harness.GetRecoveryDirectory());
			REQUIRE(files.size() == 1);
			file = files[0];
			const std::optional<std::string> text = FileSystem::ReadText(file);
			REQUIRE(text.has_value());
			header = ReadRecoveryHeader(file);
			body = text->substr(text->find('\n') + 1);
			// Back to the saved scene: this editor leaves nothing behind.
			harness.Run("edit.undo");
			harness.FlushJournal();
			REQUIRE(ListRecoveryFiles(harness.GetRecoveryDirectory()).empty());
		}
		REQUIRE(header["Owner"]["ProcessId"] == Platform::GetProcessID());
		REQUIRE(header["Discarded"] == false);

		// Owned by this process (its id and start time): the journal of the scene this editor edits, not offered.
		REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(header) + "\n" + body));
		{
			RecoveryHarness harness(project, false);
			CHECK(harness.Run("scene.recoveryStatus")["entries"].empty());
		}

		// Another start time: an earlier process with the same id, which is gone.
		header["Owner"]["ProcessStartTime"] = header["Owner"]["ProcessStartTime"].get<uint64_t>() + 1;
		REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(header) + "\n" + body));
		RecoveryHarness harness(project, false);
		const nlohmann::json entries = harness.Run("scene.recoveryStatus")["entries"];
		REQUIRE(entries.size() == 1);
		harness.Run("scene.recover", { { "scene", entries[0]["id"] }, { "action", "apply" } });
		CHECK(harness.Context.GetEditScene()->FindEntityByName("Crate"));
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

			bool Undo(std::string*) override
			{
				Rename("Entity");
				return true;
			}
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

	TEST_CASE("A step that writes a file and edits the scene is journaled by its entity states")
	{
		// A linking prefab.create writes the prefab and links the entities in one step (an EditorActionGroup): the journal keeps
		// the link's entity states, so it applies to the saved scene file, not a snapshot of the scene (which a scene of more
		// than SceneRecovery::c_MaxSnapshotEntities entities would not get).
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryGroup") / "Game";
		nlohmann::json edited;
		{
			RecoveryHarness harness(project);
			const nlohmann::json crate = harness.Run("entity.create", { { "name", "Crate" } })["id"];
			CHECK(harness.Run("prefab.create", { { "entities", nlohmann::json::array({ crate }) }, { "path", "Prefabs/Crate.stprefab" } })["linked"] == true);
			edited = harness.Snapshot();
			harness.Run("project.close", { { "force", true } });
		}

		RecoveryHarness harness(project, false);
		const nlohmann::json entries = harness.Run("scene.recoveryStatus")["entries"];
		REQUIRE(entries.size() == 1);
		CHECK(entries[0]["complete"] == false);
		harness.Run("scene.recover", { { "scene", entries[0]["id"] }, { "action", "apply" } });
		CHECK(harness.Snapshot() == edited);
	}

	TEST_CASE("The frames of a drag merged into one step wait for the next write as that one step")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryDrag") / "Game";
		nlohmann::json edited;
		{
			RecoveryHarness harness(project);
			harness.Run("entity.create", { { "name", "Crate" } });
			harness.FlushJournal();
			REQUIRE(harness.Context.GetRecovery().GetStatus().PendingSteps == 0);

			// An inspector drag: one undo step, merged a frame at a time.
			const Entity crate = harness.Context.GetEditScene()->FindEntityByName("Crate");
			const ComponentInfo& transform = *ComponentRegistry::Find("Transform");
			const PropertyInfo& translation = *transform.FindProperty("Translation");
			const UndoStack& undo = harness.Context.GetUndoStack();
			const size_t steps = undo.GetCount();
			const auto drag = [&](int from, int to)
			{
				for (int frame = from; frame <= to; frame++)
				{
					std::string error;
					REQUIRE_MESSAGE(SetPropertyWithUndo(harness.Context, crate, transform, translation, PropertyValue(glm::vec3(0.0f, 0.0f, static_cast<float>(frame))),
						&error), error);
				}
			};
			drag(1, 30);
			REQUIRE(undo.GetCount() == steps + 1);
			CHECK(harness.Context.GetRecovery().GetStatus().PendingSteps == 1);
			CHECK(harness.Run("scene.recoveryStatus")["journal"]["pendingSteps"] == 1);

			// Written halfway, the drag goes on in the same step: again one step waits for the next write.
			harness.FlushJournal();
			drag(31, 60);
			REQUIRE(undo.GetCount() == steps + 1);
			CHECK(harness.Context.GetRecovery().GetStatus().PendingSteps == 1);
			edited = harness.Snapshot();
			harness.Run("project.close", { { "force", true } });
		}

		// What the journal kept is where the drag ended.
		RecoveryHarness harness(project, false);
		harness.Run("scene.recover", { { "scene", "Scenes/Main.stscene" }, { "action", "apply" } });
		CHECK(harness.Snapshot() == edited);
		const Entity crate = harness.Context.GetEditScene()->FindEntityByName("Crate");
		REQUIRE(crate);
		CHECK(crate.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 0.0f, 60.0f));
	}

	TEST_CASE("Unsaved changes of a scene that moved are recovered from its new place")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryMoved") / "Game";

		// A forced discard after the edited scene moved: offered at once, and it applies.
		nlohmann::json edited;
		{
			RecoveryHarness harness(project);
			harness.Run("entity.create", { { "name", "Before Move" } });
			harness.Run("asset.move", { { "asset", "Scenes/Main.stscene" }, { "path", "Scenes/Renamed.stscene" } });
			harness.Run("entity.create", { { "name", "After Move" } });
			edited = harness.Snapshot();
			harness.Run("scene.new", { { "force", true } });
			const nlohmann::json entries = harness.Run("scene.recoveryStatus")["entries"];
			REQUIRE(entries.size() == 1);
			CHECK(entries[0]["applicable"] == true);
			CHECK(entries[0]["scenePath"] == "Scenes/Renamed.stscene");
			harness.Run("scene.recover", { { "scene", "Scenes/Renamed.stscene" }, { "action", "apply" } });
			CHECK(harness.Snapshot() == edited);
			harness.Run("scene.save");
		}

		// The journal of an edited scene follows it when it moves.
		std::filesystem::path journal;
		nlohmann::json header;
		std::string body;
		{
			RecoveryHarness harness(project, false);
			harness.Run("scene.open", { { "scene", "Scenes/Renamed.stscene" } });
			harness.Run("entity.create", { { "name", "Crashed" } });
			harness.FlushJournal();
			const std::vector<std::filesystem::path> files = ListRecoveryFiles(harness.GetRecoveryDirectory());
			REQUIRE(files.size() == 1);
			journal = files[0];
			CHECK(ReadRecoveryHeader(journal)["ScenePath"] == "Scenes/Renamed.stscene");
			harness.Run("asset.move", { { "asset", "Scenes/Renamed.stscene" }, { "path", "Levels/Final.stscene" } });
			harness.Context.Update(Timestep(1.0f / 60.0f));
			harness.FlushJournal();
			header = ReadRecoveryHeader(journal);
			CHECK(header["ScenePath"] == "Levels/Final.stscene");
			const std::optional<std::string> text = FileSystem::ReadText(journal);
			REQUIRE(text.has_value());
			body = text->substr(text->find('\n') + 1);
			// Back to the saved scene: this editor leaves nothing behind. The move is a step of the history too (asset.move): it is
			// undone first, and the scene moves again with a step of its own, which leaves it saved.
			harness.Run("edit.undo"); // The move
			harness.Run("edit.undo"); // Crashed
			harness.FlushJournal();
			REQUIRE(ListRecoveryFiles(harness.GetRecoveryDirectory()).empty());
			harness.Run("asset.move", { { "asset", "Scenes/Renamed.stscene" }, { "path", "Levels/Final.stscene" } });
			harness.Context.Update(Timestep(1.0f / 60.0f));
			harness.FlushJournal();
			REQUIRE(ListRecoveryFiles(harness.GetRecoveryDirectory()).empty());
		}

		// What an editor left behind (as a crash would: its process is gone) still naming the scene's place before a move, as
		// one written just before the move would: the next editor offers it at the scene's new place.
		header["ScenePath"] = "Scenes/Renamed.stscene";
		header["Owner"]["ProcessStartTime"] = header["Owner"]["ProcessStartTime"].get<uint64_t>() + 1;
		REQUIRE(FileSystem::WriteText(journal, JsonUtils::Dump(header) + "\n" + body));
		RecoveryHarness harness(project, false);
		const nlohmann::json entries = harness.Run("scene.recoveryStatus")["entries"];
		REQUIRE(entries.size() == 1);
		CHECK(entries[0]["applicable"] == true);
		CHECK(entries[0]["scenePath"] == "Levels/Final.stscene");

		// A move while the entry is on offer: it is read again, at the scene's new place.
		harness.Run("asset.move", { { "asset", "Levels/Final.stscene" }, { "path", "Levels/Last.stscene" } });
		CHECK(harness.UpdateUntil([](const std::vector<RecoveryEntry>& offered)
		{
			return offered.size() == 1 && offered[0].Applicable && offered[0].ScenePath == "Levels/Last.stscene";
		}));
		harness.Run("scene.recover", { { "scene", entries[0]["id"] }, { "action", "apply" } });
		CHECK(harness.Context.GetEditScene()->FindEntityByName("Crashed"));
		CHECK(harness.Context.GetEditScene()->FindEntityByName("After Move"));
		harness.Run("edit.undo");
		CHECK_FALSE(harness.Context.GetEditScene()->FindEntityByName("Crashed"));
	}

	TEST_CASE("A scene that moves before its version is noted keeps its changes recoverable")
	{
		// The journal notes the version of the scene file on an I/O job. A scene moved before that job runs (it waits behind a
		// discard's write, or a script moves the scene right after opening it) is no longer where the job looks: the move's
		// own operation notes it at the new place.
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RecoveryMovedEarly");
		const std::filesystem::path assets = directory / "Assets";
		Scene saved("Level");
		saved.CreateEntity("Saved");
		const std::filesystem::path movedFile = assets / "Levels" / "Level.stscene";
		REQUIRE(FileSystem::WriteText(movedFile, JsonUtils::Dump(SceneSerializer::Serialize(saved), 1, '\t') + "\n"));

		SceneRecovery recovery;
		recovery.SetProject(directory / "Recovery", assets);
		Scene scene("Level");
		// Where the scene was when it opened: gone by the time its version is noted.
		recovery.BeginScene(scene, UUID(), assets / "Scenes" / "Level.stscene", "Scenes/Level.stscene");
		recovery.OnSceneMoved(movedFile, "Levels/Level.stscene");

		UndoStack undo;
		undo.SetObserver(&recovery);
		SceneEditTransaction transaction(scene, "Create 'Edit'", {});
		transaction.TrackCreated(scene.CreateEntity("Edit").GetUUID());
		REQUIRE(transaction.Commit(undo));
		REQUIRE(recovery.Flush(scene, true));
		recovery.Wait();
		const std::vector<std::filesystem::path> files = ListRecoveryFiles(directory / "Recovery");
		REQUIRE(files.size() == 1);
		const nlohmann::json header = ReadRecoveryHeader(files[0]);
		CHECK(header["ScenePath"] == "Levels/Level.stscene");
		CHECK(header["BaseHash"].is_string());
		undo.SetObserver(nullptr);
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

	TEST_CASE("Recovery data of a scene file hashed in several pieces still applies to it")
	{
		// The journal records the hash of the saved document; the scan hashes the file a piece at a time. Both must agree for
		// files larger than a piece.
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryLargeScene") / "Game";
		{
			RecoveryHarness harness(project);
			Tests::Perf::CreateFlatRoots(*harness.Context.GetEditScene(), 20'000);
			harness.Run("scene.save");
			REQUIRE(FileSystem::GetFileSize(project / "Assets" / "Scenes" / "Main.stscene").value_or(0) > 2 * 1024 * 1024);
			harness.Run("entity.create", { { "name", "Recovered" } });
			harness.Run("project.close", { { "force", true } });
		}
		RecoveryHarness harness(project, false);
		const nlohmann::json entries = harness.Run("scene.recoveryStatus")["entries"];
		REQUIRE(entries.size() == 1);
		INFO(entries.dump());
		CHECK(entries[0]["applicable"] == true);
		harness.Run("scene.recover", { { "scene", entries[0]["id"] }, { "action", "apply" } });
		CHECK(harness.Context.GetEditScene()->FindEntityByName("Recovered"));
	}

	TEST_CASE("Recover asks about the edited scene's own unsaved changes over the dialog, which stays open")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryOfferAsks") / "Game";
		{
			RecoveryHarness harness(project);
			harness.Run("entity.create", { { "name", "Recovered" } });
			harness.Run("project.close", { { "force", true } });
		}

		EditorOptions options;
		options.ProjectPath = project;
		Tests::HarnessEditor editor({}, options);
		REQUIRE(editor.FramesUntil([&]() { return editor.Layer->GetRecoveryDialog().IsOpen(); }));
		editor.Frames(3);
		// An agent changed the edited scene meanwhile.
		editor.Run("entity.create", { { "name", "Current" } });
		REQUIRE(editor.Context().IsSceneModified());

		REQUIRE(editor.Click("Recovery.Recover.0"));
		editor.Frames(2);
		REQUIRE(UI::ItemProbe::Find("UnsavedChanges.DontSave").has_value());
		// Over the recovery dialog, not in its place: it stays open, so no toast says the changes are still on offer.
		CHECK(editor.Layer->GetRecoveryDialog().IsOpen());
		const std::deque<EditorNotification>& notifications = editor.Context().GetNotifications().GetActive();
		CHECK(std::none_of(notifications.begin(), notifications.end(), [](const EditorNotification& notification) { return notification.ShowRecovery; }));
		CHECK(editor.Context().GetEditScene()->FindEntityByName("Current"));

		REQUIRE(editor.Click("UnsavedChanges.DontSave"));
		REQUIRE(editor.FramesUntil([&]() { return editor.Context().GetEditScene()->FindEntityByName("Recovered").IsValid(); }));
		CHECK_FALSE(editor.Context().GetEditScene()->FindEntityByName("Current"));
		CHECK(editor.Context().IsSceneModified());
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

	TEST_CASE("Discarded changes are offered at once, and the person hears who discarded them")
	{
		RecoveryHarness harness(Tests::CreateTemporaryDirectory("RecoveryDiscardNotice") / "Game");
		const EditorNotifications& notifications = harness.Context.GetNotifications();
		auto find = [&notifications](std::string_view title) -> const EditorNotification*
		{
			const auto it = std::find_if(notifications.GetActive().begin(), notifications.GetActive().end(),
				[title](const EditorNotification& notification) { return notification.Title == title; });
			return it != notifications.GetActive().end() ? &*it : nullptr;
		};

		// An agent discards the person's changes: they are offered without a project reopening or a status request, and the
		// person is told who did it, with Recover and the command in the Activity panel.
		harness.Run("entity.create", { { "name", "Crate" } });
		const EditorCommandResult forced = harness.Execute("scene.new", { { "name", "Other" }, { "force", true } },
			CommandOrigin { CommandSource::Automation, "Claude Code" });
		REQUIRE(forced.Success);
		CHECK(forced.Value["discardedChanges"] == true);
		CHECK(harness.UpdateUntil([](const std::vector<RecoveryEntry>& entries)
		{
			return entries.size() == 1 && entries[0].Discarded && entries[0].Applicable && entries[0].SceneName == "Main";
		}));
		const EditorNotification* byAgent = find("Claude Code discarded unsaved changes");
		REQUIRE(byAgent != nullptr);
		CHECK(byAgent->Level == NotificationLevel::Warning);
		CHECK(byAgent->ShowRecovery);
		CHECK(byAgent->JournalEntry == forced.JournalId);
		CHECK(byAgent->Message.find("scene.new replaced the scene 'Main'") != std::string::npos);

		// The person's own Don't Save (a command of the interface) is news too, quieter: Recover is the undo of the answer.
		harness.Context.GetNotifications().Clear();
		harness.Run("entity.create", { { "name", "Sketch" } });
		REQUIRE(harness.Execute("scene.open", { { "scene", "Scenes/Main.stscene" }, { "force", true } }, CommandOrigin { CommandSource::UI }).Success);
		const EditorNotification* byPerson = find("Unsaved changes discarded");
		REQUIRE(byPerson != nullptr);
		CHECK(byPerson->Level == NotificationLevel::Info);
		CHECK(byPerson->ShowRecovery);
		CHECK(byPerson->Message == "The changes to 'Other' are kept as recovery data.");
		CHECK(harness.UpdateUntil([](const std::vector<RecoveryEntry>& entries) { return entries.size() == 2; }));

		// Replacing a scene without changes discards nothing and says nothing.
		harness.Context.GetNotifications().Clear();
		REQUIRE(harness.Execute("scene.new", { { "name", "Empty" } }, CommandOrigin { CommandSource::Automation, "Claude Code" }).Success);
		CHECK(notifications.IsEmpty());

		// A command script that closes the project: the changes stay with the project, which offers them when it opens again.
		harness.Run("entity.create", { { "name", "Late" } });
		REQUIRE(harness.Execute("project.close", { { "force", true } }, CommandOrigin { CommandSource::CommandScript }).Success);
		const EditorNotification* byScript = find("A command script discarded unsaved changes");
		REQUIRE(byScript != nullptr);
		CHECK_FALSE(byScript->ShowRecovery);
		CHECK(byScript->Message.find("of the project 'Game'") != std::string::npos);

		// Without a project nothing can be kept, and an agent that discards says so.
		harness.Context.GetNotifications().Clear();
		harness.Run("entity.create", { { "name", "Nowhere" } });
		REQUIRE(harness.Execute("scene.new", { { "force", true } }, CommandOrigin { CommandSource::Automation, "Claude Code" }).Success);
		const EditorNotification* lost = find("Claude Code discarded unsaved changes");
		REQUIRE(lost != nullptr);
		CHECK(lost->Message.find("are gone: no project is open") != std::string::npos);
		CHECK_FALSE(lost->ShowRecovery);
	}

	TEST_CASE("Changes discarded on purpose are offered up to a limit, what a crash left always")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryDiscardLimit") / "Game";
		RecoveryHarness harness(project);

		// What a crashed editor left: a journal whose owner process is gone.
		harness.Run("entity.create", { { "name", "Crashed" } });
		harness.FlushJournal();
		std::vector<std::filesystem::path> files = ListRecoveryFiles(harness.GetRecoveryDirectory());
		REQUIRE(files.size() == 1);
		const std::string text = *FileSystem::ReadText(files[0]);
		nlohmann::json header = ReadRecoveryHeader(files[0]);
		header["Owner"]["ProcessStartTime"] = header["Owner"]["ProcessStartTime"].get<uint64_t>() + 1;
		header["Time"] = header["Time"].get<int64_t>() - 60'000; // The oldest of all
		const std::string crashedId = "Crashed-0000000000000001";
		REQUIRE(FileSystem::WriteText(harness.GetRecoveryDirectory() / (crashedId + SceneRecovery::c_FileExtension),
			JsonUtils::Dump(header) + "\n" + text.substr(text.find('\n') + 1)));
		harness.Run("edit.undo");
		harness.FlushJournal();

		// An agent that discards scene after scene: each discard is kept, the newest are offered.
		const size_t discards = SceneRecovery::c_MaxOfferedDiscards + 3;
		std::string newest;
		for (size_t index = 0; index < discards; index++)
		{
			harness.Run("entity.create", { { "name", fmt::format("Attempt {}", index) } });
			const std::string file = harness.Context.GetRecovery().GetStatus().File;
			newest = file.substr(0, file.size() - std::string_view(SceneRecovery::c_FileExtension).size());
			REQUIRE(harness.Execute("scene.open", { { "scene", "Scenes/Main.stscene" }, { "force", true } }).Success);
			harness.Context.GetRecovery().Wait();
			// Apart in time, so that "newest" means one file.
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
		}

		const nlohmann::json entries = harness.Run("scene.recoveryStatus")["entries"];
		size_t offeredDiscards = 0;
		bool crashOffered = false;
		bool newestOffered = false;
		for (const nlohmann::json& entry : entries)
		{
			offeredDiscards += entry["discarded"].get<bool>() ? 1 : 0;
			crashOffered |= entry["id"] == crashedId;
			newestOffered |= entry["id"] == newest;
		}
		CHECK(offeredDiscards == SceneRecovery::c_MaxOfferedDiscards);
		CHECK(crashOffered);
		CHECK(newestOffered);
		CHECK(entries.size() == SceneRecovery::c_MaxOfferedDiscards + 1);
		// The others are kept where discarded entries go.
		CHECK(ListRecoveryFiles(harness.GetRecoveryDirectory() / SceneRecovery::c_DiscardedDirectoryName).size() == discards - SceneRecovery::c_MaxOfferedDiscards);
	}

	TEST_CASE("A toast's Recover brings back what was just discarded, and the dialog fits the window with many offers")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryToast") / "Game";
		{
			RecoveryHarness harness(project);
		}
		EditorOptions options;
		options.ProjectPath = project;
		// A laptop's window at 150%: eight offers at their full height would not fit.
		Tests::ImGuiHarness::Specification specification;
		specification.DisplaySize = ImVec2(1600.0f, 900.0f);
		specification.ContentScale = 1.5f;
		Tests::HarnessEditor editor(specification, options);
		editor.Frames(3);
		REQUIRE_FALSE(editor.Layer->GetRecoveryDialog().IsOpen());

		// The person discards changes by mistake: the toast's Recover opens the dialog with them while the scan that finds them
		// may still run, and recovering brings them back.
		editor.Run("entity.create", { { "name", "Precious" } });
		editor.Context().GetNotifications().Clear();
		REQUIRE(editor.Layer->GetCommands().Execute(editor.Context(), "scene.new", { { "force", true } }, CommandOrigin { CommandSource::UI }).Success);
		// A toast sizes itself over its first frames.
		editor.Frames(2);
		REQUIRE(editor.Click("Toast.0.Recover"));
		REQUIRE(editor.FramesUntil([&]() { return UI::ItemProbe::Find("Recovery.Recover.0").has_value(); }));
		CHECK(editor.Layer->GetRecoveryDialog().IsOpen());
		// A dialog that sizes itself to its contents settles over its first frames.
		editor.Frames(3);
		REQUIRE(editor.Click("Recovery.Recover.0"));
		// The new scene has no changes of its own: nothing to ask.
		REQUIRE(editor.FramesUntil([&]() { return editor.Context().GetEditScene()->FindEntityByName("Precious").IsValid(); }));
		editor.Frames(2);
		CHECK_FALSE(editor.Layer->GetRecoveryDialog().IsOpen());

		// Asked for right after a discard, the dialog opens once the scan that finds it has finished, with the entry, instead of
		// closing as if nothing were left. (scene.open answers frames after the discard, since the scene loads in the
		// background: the scene opens here at once, as tools open it, so that the dialog is asked for right after.)
		const AssetHandle main = editor.Context().GetAssetManager()->FindAssetByPath("Scenes/Main.stscene");
		REQUIRE(main.IsValid());
		auto openMain = [&]()
		{
			std::string error;
			REQUIRE_MESSAGE(editor.Context().OpenScene(main, &error), error);
		};
		editor.Run("entity.create", { { "name", "Again" } });
		openMain();
		editor.Layer->ShowRecoveryDialog();
		editor.Frames(1);
		REQUIRE(editor.Context().GetRecovery().IsScanning());
		CHECK_FALSE(editor.Layer->GetRecoveryDialog().IsOpen());
		CHECK(editor.FramesUntil([&]() { return UI::ItemProbe::Find("Recovery.Recover.0").has_value(); }));
		CHECK(editor.Layer->GetRecoveryDialog().IsOpen());

		// Many offers scroll in a list: the dialog's buttons stay inside the window, and Discard All empties it.
		for (size_t index = 0; index + 1 < SceneRecovery::c_MaxOfferedDiscards; index++)
		{
			editor.Run("entity.create", { { "name", fmt::format("Attempt {}", index) } });
			openMain();
			editor.Context().GetRecovery().Wait();
		}
		REQUIRE(editor.FramesUntil([&]()
		{
			return !editor.Context().GetRecovery().IsScanning() && editor.Context().GetRecovery().GetEntries().size() == SceneRecovery::c_MaxOfferedDiscards;
		}));
		// The dialog stayed open while the offers came: placed for one entry, it grew, and stays inside the window.
		editor.Frames(3);
		REQUIRE(editor.Layer->GetRecoveryDialog().IsOpen());
		const ImGuiWindow* dialog = ImGui::FindWindowByName(RecoveryDialog::c_Popup);
		REQUIRE(dialog != nullptr);
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		CHECK(dialog->Pos.y >= viewport->Pos.y);
		CHECK(dialog->Pos.y + dialog->Size.y <= viewport->Pos.y + viewport->Size.y);
		const std::optional<UI::ItemProbe::Item> later = UI::ItemProbe::Find("Recovery.Later");
		REQUIRE(later.has_value());
		CHECK(later->Max.y <= dialog->Pos.y + dialog->Size.y);
		REQUIRE(editor.Click("Recovery.DiscardAll"));
		REQUIRE(editor.FramesUntil([&]() { return !editor.Layer->GetRecoveryDialog().IsOpen(); }));
		editor.Context().GetRecovery().Wait();
		CHECK(ListRecoveryFiles(project / ".strata" / SceneRecovery::c_DirectoryName).size() <= 1); // The edited scene's own journal, if any
		CHECK(editor.Context().GetRecovery().GetEntries().empty());
	}

	TEST_CASE("Changes discarded as the project opens are offered by their toast, not by the dialog that opens with the project")
	{
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("RecoveryEarlyDiscard") / "Game";
		{
			RecoveryHarness harness(project);
		}
		EditorOptions options;
		options.ProjectPath = project;
		Tests::HarnessEditor editor({}, options);
		// Before the editor's first frame, while the scan of the project's recovery files runs: a command script edits and
		// replaces the scene, as a script that starts with scene.new {force} does.
		editor.Run("entity.create", { { "name", "Early" } });
		REQUIRE(editor.Layer->GetCommands().Execute(editor.Context(), "scene.new", { { "force", true } }, CommandOrigin { CommandSource::CommandScript }).Success);
		REQUIRE(editor.FramesUntil([&]()
		{
			return !editor.Context().GetRecovery().IsScanning() && editor.Context().GetRecovery().GetEntries().size() == 1;
		}));
		editor.Frames(3);
		// The dialog's offer as the project opens is what a crash or an earlier session left; this discard has its toast.
		CHECK_FALSE(editor.Layer->GetRecoveryDialog().IsOpen());
		CHECK(UI::ItemProbe::Find("Toast.0.Recover").has_value());
		CHECK(RecoveryDialog::HasOffer(editor.Context()));
		CHECK_FALSE(RecoveryDialog::HasOffer(editor.Context(), true));

		// Closed with the project and opened again, the discard is what an earlier session left: offered as the project opens.
		editor.Run("project.close", { { "force", true } });
		editor.RunToCompletion("project.open", { { "path", FileSystem::ToUTF8(project) } });
		REQUIRE(editor.FramesUntil([&]() { return editor.Layer->GetRecoveryDialog().IsOpen(); }));
	}
}
