#include <doctest/doctest.h>

#include "Asset/AssetTestUtils.h"
#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ProjectMigration.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetRegistration.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Project/Project.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/ProjectCollisionLayers.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneSerializer.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr AssetHandle c_MainScene = UUID(0x5CE7E00000000001ull);

	EditorContextSpecification MakeSpecification()
	{
		EditorContextSpecification specification;
		specification.WatchAssetFiles = false;
		return specification;
	}

	// A scene document as 0.5.0 wrote it, storing `layers` in its settings (null: no key), with one entity. With
	// settingsFirst the settings come before the entities, as the streaming writer puts them; otherwise after them, as
	// older documents (tab-indented, keys sorted) have them.
	std::string MakeSceneText(const std::string& name, const nlohmann::json& layers, bool settingsFirst)
	{
		Scene scene(name);
		scene.CreateEntity("Thing");
		const nlohmann::json document = SceneSerializer::Serialize(scene);
		nlohmann::json settings = document["Scene"]["Settings"];
		if (!layers.is_null())
			settings["PhysicsLayers"] = layers;
		const std::string header = "\"Strata\":" + JsonUtils::Dump(document["Strata"]);
		const std::string nameField = "\"Name\":" + JsonUtils::Dump(name);
		const std::string settingsField = "\"Settings\":" + JsonUtils::Dump(settings);
		const std::string entities = "\"Entities\":" + JsonUtils::Dump(document["Scene"]["Entities"]);
		if (settingsFirst)
			return "{" + header + ",\"Scene\":{" + nameField + "," + settingsField + "," + entities + "}}";
		return "{\"Scene\":{" + entities + "," + nameField + "," + settingsField + "}," + header + "}";
	}

	nlohmann::json StoreLayers(const PhysicsLayerSettings& layers)
	{
		return layers.ToJson();
	}

	// A project as 0.5.0 left it: a project file of `version` (2: 0.5.0's) without settings, and scenes whose documents store
	// collision layers; the first scene is the start scene.
	struct OldProject
	{
		std::filesystem::path Directory;
		std::filesystem::path ProjectFile;
		std::vector<std::string> ScenePaths;

		explicit OldProject(const std::string& name, uint32_t version = 2)
			: Directory(CreateTemporaryDirectory(name) / "Old")
		{
			ProjectFile = Directory / "Old.stproj";
			const nlohmann::json project = {
				{ "Strata", { { "Format", "Project" }, { "Version", version } } },
				{ "Project", { { "Name", "Old" }, { "AssetDirectory", "Assets" }, { "StartScene", UUIDToJson(c_MainScene) },
					{ "Scripts", { { "SourceDirectory", "Scripts" }, { "ModuleName", "OldScripts" } } } } } };
			REQUIRE(FileSystem::CreateDirectories(Directory / "Assets" / "Scenes"));
			REQUIRE(FileSystem::WriteText(ProjectFile, JsonUtils::Dump(project, 1, '\t')));
		}

		// Writes a scene document (null layers: no key) with its .meta; `handle` null makes one.
		void AddScene(const std::string& path, const nlohmann::json& layers, bool settingsFirst = true, AssetHandle handle = UUID::Null())
		{
			const std::filesystem::path file = Directory / "Assets" / FileSystem::FromUTF8(path);
			REQUIRE(FileSystem::CreateDirectories(file.parent_path()));
			REQUIRE(FileSystem::WriteText(file, MakeSceneText(FileSystem::ToUTF8(file.stem()), layers, settingsFirst)));
			std::filesystem::path meta = file;
			meta += ".meta";
			REQUIRE(WriteAssetMeta(meta, handle.IsValid() ? handle : UUID(), AssetType::Scene, nlohmann::json::object()));
			ScenePaths.push_back(path);
		}

		std::string ReadScene(const std::string& path) const
		{
			const std::optional<std::string> text = FileSystem::ReadText(Directory / "Assets" / FileSystem::FromUTF8(path));
			REQUIRE(text.has_value());
			return *text;
		}

		nlohmann::json ReadProjectFile() const
		{
			const std::optional<std::string> text = FileSystem::ReadText(ProjectFile);
			REQUIRE(text.has_value());
			std::optional<nlohmann::json> json = JsonUtils::Parse(*text);
			REQUIRE(json.has_value());
			return *json;
		}
	};

	struct MigrationHarness
	{
		EditorContext Context { MakeSpecification() };
		EditorCommandRegistry Commands;

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return RunCommandToCompletion(Context, Commands, name, parameters);
		}

		nlohmann::json Open(const std::filesystem::path& project)
		{
			const nlohmann::json result = Run("project.open", { { "path", FileSystem::ToUTF8(project) }, { "force", true } });
			// The project file is written in the background: project.save waits for it.
			Run("project.save");
			return result;
		}

		PhysicsLayerSettings ProjectLayers()
		{
			const nlohmann::json* value = Context.GetProject()->GetSettings().FindSection(ProjectCollisionLayers::c_SectionId);
			return value ? PhysicsLayerSettings::FromJson(*value) : PhysicsLayerSettings();
		}
	};

	// The warnings logged since `sequence` that mention `text`.
	size_t CountWarnings(uint64_t sequence, std::string_view text)
	{
		size_t count = 0;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(sequence))
		{
			if (entry.Level >= LogLevel::Warn && entry.Message.find(text) != std::string::npos)
				count++;
		}
		return count;
	}

	PhysicsLayerSettings MakeBallAndGhost()
	{
		PhysicsLayerSettings layers;
		layers.Names[1] = "Ball";
		layers.Names[2] = "Ghost";
		layers.SetCollide(1, 2, false);
		return layers;
	}

	const nlohmann::json c_DefaultTable = { { "Names", nlohmann::json::object() }, { "NoCollisions", nlohmann::json::array() } };

}

TEST_SUITE("Editor.ProjectMigration")
{
	TEST_CASE("A 0.5.0 project's scene layers become the project's")
	{
		OldProject old("MigrateLayers");
		old.AddScene("Scenes/Main.stscene", StoreLayers(MakeBallAndGhost()), true, c_MainScene);
		old.AddScene("Scenes/Menu.stscene", c_DefaultTable, false);
		old.AddScene("Scenes/Older.stscene", nullptr, false);
		const std::string mainText = old.ReadScene("Scenes/Main.stscene");

		MigrationHarness harness;
		const uint64_t logged = Log::GetBuffer().GetLatestSequence();
		const nlohmann::json result = harness.Open(old.ProjectFile);
		CHECK(result["migrated"]["collisionLayers"]["scenes"] == nlohmann::json::array({ "Scenes/Main.stscene" }));
		CHECK(result["migrated"]["collisionLayers"]["conflicts"].empty());
		CHECK(result["migrated"]["collisionLayers"]["unreadable"].empty());
		CHECK(harness.ProjectLayers() == MakeBallAndGhost());
		// The start scene opened with the project's table, which its own equals: no warning.
		CHECK(result["startSceneOpened"] == true);
		CHECK(harness.Context.GetActiveScene()->GetPhysicsLayers() == MakeBallAndGhost());
		CHECK_FALSE(harness.Context.GetActiveScene()->DoStoredLayersDiffer());
		CHECK(CountWarnings(logged, "") == 0);

		// The file is version 3 with the section; the scene files are untouched.
		const nlohmann::json file = old.ReadProjectFile();
		CHECK(file["Strata"]["Version"] == Project::c_FormatVersion);
		REQUIRE(file.contains("Settings"));
		CHECK(PhysicsLayerSettings::FromJson(file["Settings"][ProjectCollisionLayers::c_SectionId]["Value"]) == MakeBallAndGhost());
		CHECK(old.ReadScene("Scenes/Main.stscene") == mainText);
		// No undo step: the project had no table to go back to.
		CHECK(harness.Context.GetUndoStack().GetPosition() == 0);
	}

	TEST_CASE("Different names for one index are reported; the start scene's wins")
	{
		OldProject old("MigrateNameConflicts");
		PhysicsLayerSettings other;
		other.Names[1] = "Sphere"; // Layer 1 is "Ball" in the start scene
		other.Names[2] = "Ghost";
		other.Names[3] = "Ball";   // A name the start scene gives layer 1
		// The start scene comes first although "Scenes/A..." sorts before it.
		old.AddScene("Scenes/Zed.stscene", StoreLayers(MakeBallAndGhost()), true, c_MainScene);
		old.AddScene("Scenes/A.stscene", StoreLayers(other), false);

		MigrationHarness harness;
		const uint64_t logged = Log::GetBuffer().GetLatestSequence();
		const nlohmann::json result = harness.Open(old.ProjectFile);
		const PhysicsLayerSettings layers = harness.ProjectLayers();
		CHECK(layers.Names[1] == "Ball");
		CHECK(layers.Names[2] == "Ghost");
		CHECK(layers.Names[3].empty());
		CHECK(result["migrated"]["collisionLayers"]["scenes"] == nlohmann::json::array({ "Scenes/Zed.stscene", "Scenes/A.stscene" }));
		const nlohmann::json& conflicts = result["migrated"]["collisionLayers"]["conflicts"];
		REQUIRE(conflicts.size() == 2);
		CHECK(conflicts[0].get<std::string>().find("Sphere") != std::string::npos);
		CHECK(conflicts[1].get<std::string>().find("layer 3 'Ball'") != std::string::npos);
		// One warning lists them.
		CHECK(CountWarnings(logged, "Sphere") == 1);

		// Without a start scene among them, the first by path names layers.
		const ProjectMigration::LayerMerge merge = ProjectMigration::MergeSceneLayers(
			{ { "Scenes/A.stscene", UUID(), other }, { "Scenes/Zed.stscene", UUID(), MakeBallAndGhost() } }, UUID::Null());
		REQUIRE(merge.Layers.has_value());
		CHECK(merge.Layers->Names[1] == "Sphere");
		CHECK(merge.Layers->Names[3] == "Ball");
	}

	TEST_CASE("A pair any scene disabled is disabled in the project")
	{
		OldProject old("MigrateMatrix");
		PhysicsLayerSettings other;
		other.SetCollide(3, 3, false);
		old.AddScene("Scenes/Main.stscene", StoreLayers(MakeBallAndGhost()), true, c_MainScene);
		old.AddScene("Scenes/Other.stscene", StoreLayers(other), true);

		MigrationHarness harness;
		const nlohmann::json result = harness.Open(old.ProjectFile);
		const PhysicsLayerSettings layers = harness.ProjectLayers();
		CHECK_FALSE(layers.DoCollide(1, 2));
		CHECK_FALSE(layers.DoCollide(3, 3));
		CHECK(layers.DoCollide(1, 3));
		CHECK(layers.DoCollide(0, 0));
		// Matrices that differ are a conflict.
		const nlohmann::json& conflicts = result["migrated"]["collisionLayers"]["conflicts"];
		REQUIRE(conflicts.size() == 1);
		CHECK(conflicts[0].get<std::string>().find("Scenes/Other.stscene") != std::string::npos);
	}

	TEST_CASE("A 0.5.0 project whose scenes store no layers or the default table gets no section and is written as version 3")
	{
		OldProject old("MigrateNothing");
		old.AddScene("Scenes/Main.stscene", c_DefaultTable, true, c_MainScene);
		old.AddScene("Scenes/Older.stscene", nullptr, false);

		MigrationHarness harness;
		const uint64_t logged = Log::GetBuffer().GetLatestSequence();
		const nlohmann::json result = harness.Open(old.ProjectFile);
		CHECK(result["migrated"]["collisionLayers"]["scenes"].empty());
		CHECK_FALSE(harness.Context.GetProject()->GetSettings().IsStored(ProjectCollisionLayers::c_SectionId));
		const nlohmann::json file = old.ReadProjectFile();
		CHECK(file["Strata"]["Version"] == Project::c_FormatVersion);
		CHECK_FALSE(file.contains("Settings"));
		CHECK(CountWarnings(logged, "") == 0);
	}

	TEST_CASE("A version-3 project without the section is not migrated")
	{
		OldProject old("NoMigrationOfVersion3", 3);
		old.AddScene("Scenes/Main.stscene", StoreLayers(MakeBallAndGhost()), true, c_MainScene);

		MigrationHarness harness;
		const uint64_t logged = Log::GetBuffer().GetLatestSequence();
		const nlohmann::json result = harness.Open(old.ProjectFile);
		CHECK(result["migrated"].is_null());
		CHECK_FALSE(harness.Context.GetProject()->GetSettings().IsStored(ProjectCollisionLayers::c_SectionId));
		// The project's default table applies; the scene's own is reported once.
		const Scene& scene = *harness.Context.GetActiveScene();
		CHECK(scene.GetPhysicsLayers() == PhysicsLayerSettings());
		CHECK(scene.DoStoredLayersDiffer());
		CHECK(CountWarnings(logged, "stores collision layers of its own") == 1);
		harness.Run("scene.open", { { "scene", "Scenes/Main.stscene" }, { "force", true } });
		CHECK(CountWarnings(logged, "stores collision layers of its own") == 1);
	}

	TEST_CASE("A migrated or new project is not scanned again")
	{
		OldProject old("MigrateOnce");
		old.AddScene("Scenes/Main.stscene", StoreLayers(MakeBallAndGhost()), true, c_MainScene);
		MigrationHarness harness;
		CHECK_FALSE(harness.Open(old.ProjectFile)["migrated"].is_null());
		harness.Run("project.close", { { "force", true } });
		const nlohmann::json again = harness.Open(old.ProjectFile);
		CHECK(again["migrated"].is_null());
		CHECK(harness.ProjectLayers() == MakeBallAndGhost());

		const std::filesystem::path directory = CreateTemporaryDirectory("NewProjectNotScanned") / "Game";
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" }, { "template", "basic3d" }, { "force", true } });
		CHECK(harness.Context.GetProject()->GetFileVersion() == Project::c_FormatVersion);
		CHECK_FALSE(harness.Context.IsMigratingProject());
		CHECK(harness.Context.GetProjectMigrationResult().is_null());
	}

	TEST_CASE("Resetting the layers survives a reopen")
	{
		OldProject old("ResetSurvives");
		old.AddScene("Scenes/Main.stscene", StoreLayers(MakeBallAndGhost()), true, c_MainScene);
		MigrationHarness harness;
		harness.Open(old.ProjectFile);
		REQUIRE(harness.ProjectLayers() == MakeBallAndGhost());
		harness.Run("project.setSettings", { { "section", ProjectCollisionLayers::c_SectionId }, { "reset", true } });
		CHECK(harness.Context.GetActiveScene()->GetPhysicsLayers() == PhysicsLayerSettings());
		harness.Run("project.save");
		harness.Run("project.close", { { "force", true } });

		const nlohmann::json reopened = harness.Open(old.ProjectFile);
		CHECK(reopened["migrated"].is_null());
		CHECK_FALSE(harness.Context.GetProject()->GetSettings().IsStored(ProjectCollisionLayers::c_SectionId));
		CHECK(harness.Context.GetActiveScene()->GetPhysicsLayers() == PhysicsLayerSettings());
	}

	TEST_CASE("The upgrade reads scenes on a job before the start scene opens")
	{
		OldProject old("MigrationOnAJob");
		old.AddScene("Scenes/Main.stscene", StoreLayers(MakeBallAndGhost()), true, c_MainScene);
		ScopedJobSystem jobs(2, 1);
		MigrationHarness harness;
		std::string error;
		REQUIRE_MESSAGE(harness.Context.BeginOpenProject(old.ProjectFile, &error), error);
		// The project is open, its scenes are being read, and its start scene waits for them.
		CHECK(harness.Context.IsMigratingProject());
		CHECK(harness.Context.IsSceneLoading());
		CHECK(harness.Context.GetSceneLoadStatus().State == SceneLoadState::LoadingScene);
		CHECK_FALSE(harness.Context.GetProject()->GetSettings().IsStored(ProjectCollisionLayers::c_SectionId));
		CHECK(harness.Context.GetSceneHandle() != c_MainScene);

		REQUIRE(WaitUntil([&]()
		{
			harness.Context.Update(Timestep(1.0f / 60.0f));
			return !harness.Context.IsMigratingProject() && !harness.Context.IsSceneLoading();
		}, std::chrono::seconds(30)));
		CHECK(harness.ProjectLayers() == MakeBallAndGhost());
		CHECK(harness.Context.GetSceneHandle() == c_MainScene);
		CHECK(harness.Context.GetActiveScene()->GetPhysicsLayers() == MakeBallAndGhost());
		harness.Run("project.save");
	}

	TEST_CASE("Edits wait for the upgrade also in a project without a start scene")
	{
		OldProject old("MigrationEditsWait");
		// Without a start scene nothing else holds edits back while the scenes are read.
		nlohmann::json file = old.ReadProjectFile();
		file["Project"].erase("StartScene");
		REQUIRE(FileSystem::WriteText(old.ProjectFile, JsonUtils::Dump(file, 1, '\t')));
		old.AddScene("Scenes/Main.stscene", StoreLayers(MakeBallAndGhost()), true);

		MigrationHarness harness;
		std::string error;
		REQUIRE_MESSAGE(harness.Context.BeginOpenProject(old.ProjectFile, &error), error);
		REQUIRE(harness.Context.IsMigratingProject());
		CHECK(harness.Context.IsWaitingForAssets());
		// The UI's direct commands are refused, saying why.
		const std::optional<std::string> waiting = harness.Commands.DescribeAssetScanWait(harness.Context, "project.setCollisionLayers");
		REQUIRE(waiting.has_value());
		CHECK(waiting->find("0.5.0") != std::string::npos);

		// An edit of the layers made meanwhile runs once the scenes' layers are the project's, and builds on them: neither
		// replaces the other, and its undo goes back to the migrated table.
		EditorCommandResult named = harness.Commands.Execute(harness.Context, "project.setCollisionLayers", { { "layerNames", { { "3", "Player" } } } });
		CHECK(named.IsPending());
		named = CompleteCommand(harness.Context, std::move(named));
		REQUIRE_MESSAGE(named.Success, named.Error);
		CHECK_FALSE(harness.Context.IsMigratingProject());
		PhysicsLayerSettings expected = MakeBallAndGhost();
		expected.Names[3] = "Player";
		CHECK(harness.ProjectLayers() == expected);
		CHECK(harness.Context.GetActiveScene()->GetPhysicsLayers() == expected);
		harness.Run("edit.undo");
		CHECK(harness.ProjectLayers() == MakeBallAndGhost());
		harness.Run("project.save");
	}

	TEST_CASE("Copies of PhysicsPlayground and OpenWorld migrate to their committed sections")
	{
		struct Expected
		{
			const char* Sample;
			std::vector<std::string> Scenes;
			PhysicsLayerSettings Layers;
		};
		PhysicsLayerSettings terrain;
		terrain.Names[1] = "Terrain";
		std::vector<Expected> samples;
		samples.push_back({ "PhysicsPlayground", { "Scenes/Main.stscene" }, MakeBallAndGhost() });
		samples.push_back({ "OpenWorld", { "Worlds/Island.stscene" }, terrain });
		samples.push_back({ "ThirdPerson", {}, PhysicsLayerSettings() });
		samples.push_back({ "Tetris", {}, PhysicsLayerSettings() });
		for (const Expected& expected : samples)
		{
			CAPTURE(expected.Sample);
			std::string error;
			const std::filesystem::path copy = CopySampleProject(expected.Sample, &error);
			REQUIRE_MESSAGE(!copy.empty(), error);
			const std::filesystem::path projectFile = Project::FindProjectFile(copy, &error);
			REQUIRE_MESSAGE(!projectFile.empty(), error);
			// The section the sample was migrated to when it was opened in place (none: the default table).
			std::optional<nlohmann::json> file = JsonUtils::Parse(FileSystem::ReadText(projectFile).value_or(""));
			REQUIRE(file.has_value());
			CHECK((*file)["Strata"]["Version"] == Project::c_FormatVersion);
			const nlohmann::json settings = file->value("Settings", nlohmann::json::object());
			const nlohmann::json* committed = JsonUtils::Find(settings, ProjectCollisionLayers::c_SectionId);
			const nlohmann::json* committedValue = committed ? JsonUtils::Find(*committed, "Value") : nullptr;
			const PhysicsLayerSettings committedLayers = committedValue ? PhysicsLayerSettings::FromJson(*committedValue) : PhysicsLayerSettings();
			CHECK(committedLayers == expected.Layers);
			// As 0.5.0 left it: version 2, without settings.
			(*file)["Strata"]["Version"] = 2;
			file->erase("Settings");
			REQUIRE(FileSystem::WriteText(projectFile, JsonUtils::Dump(*file, 1, '\t')));

			MigrationHarness harness;
			const uint64_t logged = Log::GetBuffer().GetLatestSequence();
			const nlohmann::json result = harness.Open(projectFile);
			nlohmann::json scenes = nlohmann::json::array();
			for (const std::string& scene : expected.Scenes)
				scenes.push_back(scene);
			CHECK(result["migrated"]["collisionLayers"]["scenes"] == scenes);
			CHECK(result["migrated"]["collisionLayers"]["conflicts"].empty());
			CHECK(harness.ProjectLayers() == committedLayers);
			CHECK(CountWarnings(logged, "collision layers") == 0);
		}
	}

	TEST_CASE("A copy of OpenWorld loads Loading.stscene without a layer warning")
	{
		std::string error;
		const std::filesystem::path copy = CopySampleProject("OpenWorld", &error);
		REQUIRE_MESSAGE(!copy.empty(), error);
		MigrationHarness harness;
		// The start scene (the island, whose layers the project's are now) opens with the project, then Loading.stscene.
		const uint64_t logged = Log::GetBuffer().GetLatestSequence();
		harness.Open(Project::FindProjectFile(copy));
		REQUIRE(harness.ProjectLayers().Names[1] == "Terrain");
		harness.Run("scene.open", { { "scene", "Scenes/Loading.stscene" }, { "force", true } });
		// Loading.stscene stores the default table, which counts as storing nothing.
		CHECK_FALSE(harness.Context.GetActiveScene()->GetStoredPhysicsLayers().has_value());
		CHECK_FALSE(harness.Context.GetActiveScene()->DoStoredLayersDiffer());
		CHECK(harness.Context.GetActiveScene()->GetPhysicsLayers().Names[1] == "Terrain");
		CHECK(CountWarnings(logged, "collision layers") == 0);
	}
}
