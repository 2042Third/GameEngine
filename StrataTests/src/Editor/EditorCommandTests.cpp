#include <doctest/doctest.h>

#include "Animation/SkinnedGltf.h"

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/UndoStack.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetDependencies.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Version.h>
#include <Strata/Physics/PhysicsSystem.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Renderer/RendererComponents.h>
#include <Strata/Runtime/GameRuntime.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/SceneSerializer.h>

#include <cstdint>
#include <utility>

using namespace Strata;

namespace
{

	// Runs commands against a context and fails the test with the command's error when one is expected to work.
	struct CommandHarness
	{
		EditorContext Context;
		EditorCommandRegistry Commands;

		explicit CommandHarness(bool watchFiles = false)
			: Context(EditorContextSpecification { watchFiles })
		{
		}

		// Runs a command to its end, as the editor's command runner does (scene.open, for one, answers frames later).
		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		std::string Error(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
			CHECK_FALSE(result.Success);
			return result.Error;
		}

		nlohmann::json SceneSnapshot() const
		{
			return SceneSerializer::Serialize(*Context.GetEditScene())["Scene"]["Entities"];
		}
	};

}

TEST_SUITE("Editor.Commands")
{
	TEST_CASE("asset.info gives the bounds of meshes, models (at their import scale, standing) and prefabs")
	{
		CommandHarness harness;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorAssetBounds") / "Game";
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		const nlohmann::json cube = harness.Run("asset.info", { { "asset", UUIDToJson(BuiltinAssets::CubeMesh) } });
		CHECK(cube["bounds"]["min"] == nlohmann::json { -0.5, -0.5, -0.5 });
		CHECK(cube["bounds"]["size"] == nlohmann::json { 1.0, 1.0, 1.0 });
		CHECK_FALSE(cube.contains("boundsPending"));
		CHECK_FALSE(harness.Run("asset.info", { { "asset", UUIDToJson(BuiltinAssets::CubeMesh) }, { "bounds", false } }).contains("bounds"));

		// The Z-up arm at Scale 1 and 0.01: standing along +Y in its rest pose, two units and two centimeters tall.
		REQUIRE(FileSystem::CreateDirectories(directory / "Assets" / "Models"));
		REQUIRE(FileSystem::WriteBytes(directory / "Assets" / "Models" / "Arm.gltf", Tests::CreateSkinnedArmGltf(Tests::ZUpArmOptions())));
		REQUIRE(FileSystem::WriteBytes(directory / "Assets" / "Models" / "SmallArm.gltf", Tests::CreateSkinnedArmGltf(Tests::ZUpArmOptions())));
		harness.Context.GetAssetManager()->Scan();
		harness.Run("asset.setImportSettings", { { "asset", "Models/SmallArm.gltf" }, { "settings", { { "Scale", 0.01 } } } });
		const nlohmann::json arm = harness.Run("asset.info", { { "asset", "Models/Arm.gltf" } });
		const nlohmann::json small = harness.Run("asset.info", { { "asset", "Models/SmallArm.gltf" } });
		REQUIRE(arm["bounds"].is_object());
		REQUIRE(small["bounds"].is_object());
		for (int axis = 0; axis < 3; axis++)
		{
			CAPTURE(axis);
			CHECK(small["bounds"]["min"][axis].get<double>() == doctest::Approx(arm["bounds"]["min"][axis].get<double>() * 0.01));
			CHECK(small["bounds"]["max"][axis].get<double>() == doctest::Approx(arm["bounds"]["max"][axis].get<double>() * 0.01));
		}
		CHECK(arm["bounds"]["min"][1].get<double>() == doctest::Approx(0.0).epsilon(1e-4));
		CHECK(arm["bounds"]["max"][1].get<double>() == doctest::Approx(2.0).epsilon(1e-4));
		CHECK(arm["bounds"]["size"][2].get<double>() == doctest::Approx(0.2).epsilon(1e-4));
		// Answered from the cache the second time.
		const uint64_t measured = harness.Context.GetAssetBounds().GetMeasureCount();
		CHECK(harness.Run("asset.info", { { "asset", "Models/Arm.gltf" } })["bounds"] == arm["bounds"]);
		CHECK(harness.Context.GetAssetBounds().GetMeasureCount() == measured);

		// A prefab of a cube, around its root's origin wherever the root stood.
		const std::string crate = harness.Run("entity.create", { { "name", "Crate" }, { "components", {
			{ "Transform", { { "Translation", { 5, 0, 0 } }, { "Scale", { 2, 1, 1 } } } },
			{ "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } } })["id"].get<std::string>();
		harness.Run("prefab.create", { { "entities", { crate } }, { "path", "Prefabs/Crate.stprefab" } });
		const nlohmann::json prefab = harness.Run("asset.info", { { "asset", "Prefabs/Crate.stprefab" } });
		CHECK(prefab["bounds"]["min"] == nlohmann::json { -1.0, -0.5, -0.5 });
		CHECK(prefab["bounds"]["max"] == nlohmann::json { 1.0, 0.5, 0.5 });
	}

	TEST_CASE("Materials warn about textures imported for another use than their slot reads, with the command that fixes it")
	{
		CommandHarness harness;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorTextureUsage") / "Game";
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		// A Poly Haven name: the importer of the base took "_1k" for the end of the name and imported it as Color.
		std::vector<uint8_t> pixels;
		for (int texel = 0; texel < 16; texel++)
			pixels.insert(pixels.end(), { 128, 128, 255, 255 });
		REQUIRE(FileSystem::CreateDirectories(directory / "Assets" / "Textures"));
		REQUIRE(FileSystem::WriteBytes(directory / "Assets" / "Textures" / "aerial_rocks_02_nor_gl_1k.png", Tests::EncodePNG(4, 4, pixels)));
		harness.Context.GetAssetManager()->Scan();
		const std::string texture = UUIDToJson(harness.Context.GetAssetManager()->FindAssetByPath("Textures/aerial_rocks_02_nor_gl_1k.png")).get<std::string>();
		harness.Run("asset.setImportSettings", { { "asset", texture }, { "settings", { { "Usage", "Color" } } } });

		harness.Run("material.create", { { "path", "Materials/Rocks.stmat" } });
		const nlohmann::json set = harness.Run("material.set", { { "material", "Materials/Rocks.stmat" }, { "properties", { { "NormalMap", texture } } } });
		REQUIRE(set["warnings"].size() == 1);
		const nlohmann::json& warning = set["warnings"][0];
		CHECK(warning["property"] == "NormalMap");
		CHECK(warning["texture"] == texture);
		CHECK(warning["usage"] == "Color");
		CHECK(warning["expected"] == "NormalMap");
		CHECK(warning["message"] == "aerial_rocks_02_nor_gl_1k is imported as Color: its normals will be wrong");
		CHECK(warning["fix"]["command"] == "asset.setImportSettings");
		CHECK(harness.Run("material.get", { { "material", "Materials/Rocks.stmat" } })["warnings"] == set["warnings"]);

		// The fix imports the texture as a normal map; the warning is gone.
		harness.Run(warning["fix"]["command"].get<std::string>(), warning["fix"]["parameters"]);
		CHECK(harness.Context.GetAssetManager()->GetImportSettings(*UUIDFromJson(nlohmann::json(texture)))["Usage"] == "NormalMap");
		CHECK(harness.Run("material.get", { { "material", "Materials/Rocks.stmat" } })["warnings"].empty());

		// A normal map as base color warns too; material.create reports it.
		const nlohmann::json created = harness.Run("material.create", { { "path", "Materials/Odd.stmat" }, { "properties", { { "BaseColorMap", texture } } } });
		REQUIRE(created["warnings"].size() == 1);
		CHECK(created["warnings"][0]["expected"] == "Color");
	}

	TEST_CASE("Saving a scene records what it references as its file shows")
	{
		CommandHarness harness;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorSceneReferences");
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "References" } });
		harness.Run("material.create", { { "path", "Materials/Red.stmat" } });
		harness.Run("material.create", { { "path", "Materials/Blue.stmat" } });
		harness.Run("entity.create", { { "name", "Red" }, { "components", { { "MeshRenderer", { { "Mesh", "0000000000000001" }, { "Material", "Materials/Red.stmat" } } } } } });
		const AssetHandle scene = *UUIDFromJson(harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } })["scene"]);
		EditorAssetManager& assets = *harness.Context.GetAssetManager();
		auto fileReferences = [&]()
		{
			const std::optional<std::vector<uint8_t>> file = FileSystem::ReadBytes(assets.GetAbsolutePath(scene));
			REQUIRE(file);
			std::vector<AssetHandle> references;
			REQUIRE(AssetDependencies::Scan(AssetType::Scene, *file, references));
			return references;
		};
		CHECK(assets.GetDependencies(scene) == fileReferences());
		CHECK(assets.GetDependencies(scene).size() == 2); // The built-in cube and the red material

		harness.Run("entity.create", { { "name", "Blue" }, { "components", { { "MeshRenderer", { { "Mesh", "0000000000000002" }, { "Material", "Materials/Blue.stmat" } } } } } });
		harness.Run("scene.save");
		CHECK(assets.GetDependencies(scene) == fileReferences());
		CHECK(assets.GetDependencies(scene).size() == 4);
	}

	TEST_CASE("Every command documents itself with an object schema")
	{
		CommandHarness harness;
		const nlohmann::json listing = harness.Run("editor.commands");
		REQUIRE(listing["commands"].size() >= 40);
		for (const nlohmann::json& command : listing["commands"])
		{
			CAPTURE(command["name"].get<std::string>());
			CHECK_FALSE(command["description"].get<std::string>().empty());
			CHECK(command["parameters"]["type"] == "object");
			const std::string name = command["name"].get<std::string>();
			CHECK(name.find('.') != std::string::npos);
		}

		CHECK(harness.Error("does.not.exist").find("Unknown command") != std::string::npos);
		CHECK_FALSE(harness.Commands.Execute(harness.Context, "scene.info", nlohmann::json::array()).Success);
	}

	TEST_CASE("editor.status gives a compact overview, and the sections other editor parts provide when asked")
	{
		CommandHarness harness;
		nlohmann::json status = harness.Run("editor.status");
		CHECK_FALSE(status["engineVersion"].get<std::string>().empty());
		// The compact answer has these keys and no others ("assets" is the asset manager's compact form; the provider of
		// "editor" is the EditorLayer's, which this harness has none of).
		std::vector<std::string> keys;
		for (const auto& [key, value] : status.items())
			keys.push_back(key);
		CHECK(keys == std::vector<std::string> { "assets", "engineVersion", "errors", "play", "project", "saving", "scene", "selection", "undo" });
		CHECK(status["project"] == nlohmann::json { { "open", false }, { "name", nullptr }, { "path", nullptr } });
		CHECK(status["scene"]["modified"] == false);
		CHECK(status["scene"]["scene"].is_null());
		CHECK(status["scene"]["entityCount"] == 0);
		CHECK(status["play"] == nlohmann::json { { "state", "Edit" }, { "paused", false }, { "pendingSteps", 0 }, { "running", nullptr } });
		CHECK(status["selection"] == nlohmann::json { { "count", 0 }, { "primary", nullptr } });
		CHECK(status["undo"] == nlohmann::json { { "undo", "" }, { "redo", "" } });
		CHECK(status["errors"]["scriptBuild"] == nlohmann::json { { "failed", false }, { "errors", 0 } });
		CHECK(status["errors"]["scriptFault"].is_null());
		CHECK(status["errors"]["importFailures"] == 0);
		std::vector<std::string> assetKeys;
		for (const auto& [key, value] : status["assets"].items())
			assetKeys.push_back(key);
		CHECK(assetKeys == std::vector<std::string> { "failed", "importing", "loading", "ready", "scanning" });
		CHECK(status["saving"] == nlohmann::json::array());

		// Sections add what the compact answer leaves out.
		status = harness.Run("editor.status", { { "sections", { "engine", "undo", "memory" } } });
		// The commit the build was configured from: a short hash in a git checkout.
		const std::string commit = status["engineCommit"].get<std::string>();
		CHECK(commit == c_EngineCommit);
		CHECK((commit == "unknown" || (commit.size() == 12 && commit.find_first_not_of("0123456789abcdef") == std::string::npos)));
		CHECK_FALSE(status["platform"].get<std::string>().empty());
		CHECK(status["undo"]["position"] == 0);
		CHECK(status["undo"]["count"] == 0);
		CHECK(status.contains("memory"));
		CHECK_FALSE(status.contains("sceneLoad"));
		CHECK_FALSE(status.contains("game"));
		CHECK(harness.Error("editor.status", { { "sections", { "nothing" } } }).find("unknown section 'nothing'") != std::string::npos);
		CHECK(harness.Error("editor.status", { { "sections", "engine" } }).find("array of strings") != std::string::npos);

		const std::string entity = harness.Run("entity.create", { { "name", "Player" } })["id"].get<std::string>();
		harness.Run("selection.set", { { "entities", { entity } } });
		harness.Context.SetStatusProvider("automation", []() { return nlohmann::json { { "port", 1234 } }; });
		harness.Context.SetStatusProvider("brief", []() { return nlohmann::json { { "everything", 1 } }; },
			[]() { return nlohmann::json { { "some", 1 } }; });
		// A provider cannot replace a built-in section.
		harness.Context.SetStatusProvider("scene", []() { return nlohmann::json("replaced"); });
		harness.Context.SetStatusProvider("memory", []() { return nlohmann::json("replaced"); });
		status = harness.Run("editor.status");
		CHECK(status["scene"]["modified"] == true);
		CHECK(status["scene"]["entityCount"] == 1);
		CHECK(status["selection"]["count"] == 1);
		CHECK(status["selection"]["primary"] == entity);
		CHECK(status["undo"]["undo"] == "Create 'Player'");
		// Providers without a compact form are reported only when asked for; the others compactly.
		CHECK_FALSE(status.contains("automation"));
		CHECK_FALSE(status.contains("memory"));
		CHECK(status["brief"] == nlohmann::json { { "some", 1 } });
		status = harness.Run("editor.status", { { "sections", { "automation", "brief", "memory" } } });
		CHECK(status["automation"]["port"] == 1234);
		CHECK(status["brief"] == nlohmann::json { { "everything", 1 } });
		CHECK(status["memory"] != "replaced");
		CHECK(status["undo"].contains("count") == false);

		// All of it: every section (the built-in ones, then every provider's full form).
		status = harness.Run("editor.status", { { "sections", { "all" } } });
		keys.clear();
		for (const auto& [key, value] : status.items())
			keys.push_back(key);
		CHECK(keys == std::vector<std::string> { "assets", "automation", "brief", "engineCommit", "engineVersion", "errors", "game", "memory",
			"platform", "play", "project", "recovery", "saving", "scene", "sceneLoad", "selection", "undo" });
		CHECK(status["undo"]["position"] == 1);
		CHECK(status["undo"]["count"] == 1);
		CHECK(status["selection"]["inspectedAsset"].is_null());
		CHECK(status["assets"].contains("pools"));
		CHECK(status["assets"]["scanning"].is_null());

		harness.Run("play.start");
		CHECK(harness.Run("editor.status")["play"]["state"] == "Play");
		harness.Run("play.stop");

		harness.Context.SetStatusProvider("automation", nullptr);
		harness.Context.SetStatusProvider("brief", nullptr);
		harness.Context.SetStatusProvider("scene", nullptr);
		harness.Context.SetStatusProvider("memory", nullptr);
		CHECK_FALSE(harness.Run("editor.status", { { "sections", { "all" } } }).contains("automation"));
		CHECK(harness.Error("editor.status", { { "sections", { "automation" } } }).find("unknown section") != std::string::npos);

		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorStatusProject");
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "StatusGame" }, { "force", true } });
		const nlohmann::json project = harness.Run("editor.status", { { "sections", { "project" } } })["project"];
		CHECK(project == harness.Run("project.info"));
		CHECK(project["name"] == "StatusGame");
		const nlohmann::json compact = harness.Run("editor.status")["project"];
		CHECK(compact["open"] == true);
		CHECK(compact["name"] == "StatusGame");
		CHECK(compact["path"] == project["path"]);
		CHECK(FileSystem::FromUTF8(compact["path"].get<std::string>()) == harness.Context.GetProject()->GetProjectFile());
	}

	TEST_CASE("The compact editor.status of the feature project fits in 1.5 KiB")
	{
		CommandHarness harness;
		const std::filesystem::path projectFile = Tests::CopyFeatureProject(Tests::CreateTemporaryDirectory("CompactStatus") / "Project");
		Tests::RunCommandToCompletion(harness.Context, harness.Commands, "project.open", { { "path", FileSystem::ToUTF8(projectFile) } });
		Tests::RunCommandToCompletion(harness.Context, harness.Commands, "scene.open", { { "scene", "Scenes/Feature.stscene" } });
		const nlohmann::json entities = harness.Run("scene.hierarchy")["entities"];
		REQUIRE_FALSE(entities.empty());
		harness.Run("selection.set", { { "entities", { entities[0]["id"] } } });
		harness.Run("entity.rename", { { "entity", entities[0]["id"] }, { "name", "A name long enough to be noticed in the status" } });
		const nlohmann::json status = harness.Run("editor.status");
		CHECK(status["project"]["open"] == true);
		CHECK(status["scene"]["entityCount"].get<size_t>() > 10);
		CHECK(status["selection"]["count"] == 1);
		const size_t bytes = status.dump().size();
		CHECK_MESSAGE(bytes <= 1536, "The compact editor.status takes ", bytes, " bytes: ", status.dump());
		CHECK(harness.Run("editor.status", { { "sections", { "all" } } }).dump().size() > bytes);
	}

	TEST_CASE("editor.status counts the errors logged since a client last read the log")
	{
		CommandHarness harness;
		harness.Run("log.read", { { "after", Log::GetBuffer().GetLatestSequence() } });
		CHECK(harness.Run("editor.status")["errors"]["unread"] == 0);
		ST_CORE_ERROR("status-unread-test one");
		ST_CORE_WARN("status-unread-test warning");
		ST_CORE_CRITICAL("status-unread-test two");
		CHECK(harness.Run("editor.status")["errors"]["unread"] == 2);
		// Reading only the critical messages leaves the error unread; reading from Error on marks both read.
		const nlohmann::json critical = harness.Run("log.read", { { "after", 0 }, { "minLevel", "Critical" }, { "maxCount", 10000 } });
		CHECK(harness.Run("editor.status")["errors"]["unread"] == 2);
		uint64_t after = 0;
		nlohmann::json page;
		do
		{
			page = harness.Run("log.read", { { "after", after }, { "minLevel", "Error" } });
			after = page["latest"].get<uint64_t>();
		} while (page["more"] == true);
		CHECK(harness.Run("editor.status")["errors"]["unread"] == 0);
		CHECK(critical["messages"].back()["message"] == "status-unread-test two");
	}

	TEST_CASE("editor.batch runs its steps with their bindings as one undo step")
	{
		CommandHarness harness;
		const nlohmann::json before = harness.SceneSnapshot();
		const size_t position = harness.Context.GetUndoStack().GetPosition();
		const nlohmann::json steps = nlohmann::json::array({
			{ { "command", "entity.create" }, { "parameters", { { "name", "Root" } } }, { "bind", { { "root", "/id" } } } },
			{ { "command", "entity.create" }, { "parameters", { { "name", "Child" }, { "parent", { { "$ref", "root" } } } } }, { "bind", { { "child", "/id" } } } },
			{ { "command", "entity.create" }, { "parameters", { { "name", "Grandchild" }, { "parent", { { "$ref", "child" } } } } } },
			{ { "command", "entity.get" }, { "parameters", { { "entity", { { "$ref", "child" } } } } }, { "expect", { { "/name", { { "equals", "Child" } } } } } } });
		const nlohmann::json result = harness.Run("editor.batch", { { "steps", steps } });
		REQUIRE(result["steps"].size() == 4);
		CHECK(result["steps"][0]["command"] == "entity.create");
		const std::string root = result["bindings"]["root"].get<std::string>();
		const std::string child = result["bindings"]["child"].get<std::string>();
		CHECK(result["steps"][1]["result"]["id"] == child);
		CHECK(harness.Run("entity.get", { { "entity", child } })["parent"] == root);
		const std::string grandchild = result["steps"][2]["result"]["id"].get<std::string>();
		CHECK(harness.Run("entity.get", { { "entity", grandchild } })["parent"] == child);

		// One undo step for the three, which undo and redo together.
		const UndoStack& undo = harness.Context.GetUndoStack();
		CHECK(undo.GetPosition() == position + 1);
		CHECK(undo.GetUndoName() == "Batch of 4 commands");
		harness.Run("edit.undo");
		CHECK(harness.SceneSnapshot() == before);
		harness.Run("edit.redo");
		CHECK(harness.Run("entity.get", { { "entity", grandchild } })["parent"] == child);
	}

	TEST_CASE("editor.batch undoes the steps before a failing one when atomic, and refuses what takes frames before anything runs")
	{
		CommandHarness harness;
		const nlohmann::json before = harness.SceneSnapshot();
		const size_t position = harness.Context.GetUndoStack().GetPosition();
		const nlohmann::json failing = nlohmann::json::array({
			{ { "command", "entity.create" }, { "parameters", { { "name", "First" } } } },
			{ { "command", "entity.create" }, { "parameters", { { "name", "Second" } } } },
			{ { "command", "entity.rename" }, { "parameters", { { "entity", "00000000000BAD00" }, { "name", "Nobody" } } } },
			{ { "command", "entity.create" }, { "parameters", { { "name", "Never" } } } } });
		EditorCommandResult result = harness.Commands.Execute(harness.Context, "editor.batch", { { "steps", failing } });
		CHECK_FALSE(result.Success);
		CHECK(result.Error.starts_with("Step 2 (entity.rename) failed: "));
		CHECK(harness.SceneSnapshot() == before);
		CHECK(harness.Context.GetUndoStack().GetPosition() == position);
		CHECK_FALSE(harness.Context.GetEditScene()->FindEntityByName("Never"));

		// Not atomic: what succeeded stays, as one step, and the failure is listed with its error.
		const nlohmann::json kept = harness.Run("editor.batch", { { "steps", failing }, { "atomic", false } });
		REQUIRE(kept["steps"].size() == 4);
		CHECK(kept["steps"][2].contains("error"));
		CHECK(kept["steps"][3].contains("result"));
		CHECK(harness.Context.GetEditScene()->FindEntityByName("Never"));
		CHECK(harness.Context.GetUndoStack().GetPosition() == position + 1);

		// Commands that take frames, unknown commands, a batch in a batch and references nothing binds are refused first.
		const size_t entities = harness.Context.GetEditScene()->GetEntityCount();
		for (const nlohmann::json& refused : {
			nlohmann::json::array({ { { "command", "entity.create" } }, { { "command", "viewport.capture" } } }),
			nlohmann::json::array({ { { "command", "entity.create" } }, { { "command", "play.step" } } }),
			// Saves write in the background and the commands that replace the scene or quit wait for them.
			nlohmann::json::array({ { { "command", "entity.create" } }, { { "command", "scene.save" } } }),
			nlohmann::json::array({ { { "command", "entity.create" } }, { { "command", "scene.saveAs" }, { "parameters", { { "path", "Batch.stscene" } } } } }),
			nlohmann::json::array({ { { "command", "entity.create" } }, { { "command", "scene.new" }, { "parameters", { { "force", true } } } } }),
			nlohmann::json::array({ { { "command", "entity.create" } }, { { "command", "editor.quit" }, { "parameters", { { "force", true } } } } }),
			nlohmann::json::array({ { { "command", "entity.create" } }, { { "command", "no.such" } } }),
			nlohmann::json::array({ { { "command", "entity.create" } }, { { "command", "editor.batch" }, { "parameters", { { "steps", nlohmann::json::array() } } } } }),
			nlohmann::json::array({ { { "command", "entity.create" }, { "parameters", { { "parent", { { "$ref", "missing" } } } } } } }) })
		{
			CAPTURE(refused.dump());
			result = harness.Commands.Execute(harness.Context, "editor.batch", { { "steps", refused } });
			CHECK_FALSE(result.Success);
			CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
			CHECK(harness.Context.GetEditScene()->GetEntityCount() == entities);
		}
		CHECK(harness.Commands.Execute(harness.Context, "editor.batch", { { "steps", nlohmann::json::array() } }).ErrorKind == EditorCommandError::InvalidParameters);
		nlohmann::json tooMany = nlohmann::json::array();
		for (int index = 0; index < 1001; index++)
			tooMany.push_back({ { "command", "scene.info" } });
		CHECK(harness.Commands.Execute(harness.Context, "editor.batch", { { "steps", tooMany } }).ErrorKind == EditorCommandError::InvalidParameters);
	}

	TEST_CASE("prefab.instantiate {instances} makes a thousand instances in one undo step")
	{
		CommandHarness harness;
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorInstantiateMany") / "Game";
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		const std::string crate = harness.Run("entity.create", { { "name", "Crate" }, { "components", {
			{ "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) } } } } } })["id"].get<std::string>();
		harness.Run("entity.create", { { "name", "Lid" }, { "parent", crate } });
		const nlohmann::json prefab = harness.Run("prefab.create", { { "entities", { crate } }, { "path", "Prefabs/Crate.stprefab" }, { "link", false } })["asset"];
		const std::string holder = harness.Run("entity.create", { { "name", "Holder" } })["id"].get<std::string>();
		const size_t entitiesBefore = harness.Context.GetEditScene()->GetEntityCount();
		const size_t position = harness.Context.GetUndoStack().GetPosition();
		const nlohmann::json before = harness.SceneSnapshot();

		// 998 placed by their translation, one under the holder with nothing given, one with a light on its root.
		nlohmann::json instances = nlohmann::json::array();
		for (int index = 0; index < 998; index++)
			instances.push_back({ { "components", { { "Transform", { { "Translation", { index, 0, 0 } } } } } } });
		instances.push_back({ { "parent", holder } });
		instances.push_back({ { "components", { { "PointLight", { { "Intensity", 3.0 } } } } } });
		const nlohmann::json result = harness.Run("prefab.instantiate", { { "prefab", prefab }, { "instances", instances } });
		REQUIRE(result["instances"].size() == 1000);
		CHECK(harness.Context.GetEditScene()->GetEntityCount() == entitiesBefore + 2000);
		CHECK(harness.Context.GetUndoStack().GetPosition() == position + 1);

		Scene& scene = *harness.Context.GetEditScene();
		const Entity placed = scene.GetEntityByUUID(*UUIDFromJson(result["instances"][997]["id"]));
		REQUIRE(placed);
		CHECK(placed.GetComponent<TransformComponent>().Translation == glm::vec3(997.0f, 0.0f, 0.0f));
		CHECK(placed.GetChildren().size() == 1);
		CHECK(harness.Run("prefab.status", { { "entity", result["instances"][997]["id"] } })["modified"] == false);
		const Entity held = scene.GetEntityByUUID(*UUIDFromJson(result["instances"][998]["id"]));
		REQUIRE(held);
		CHECK(held.GetParent().GetUUID().ToString() == holder);
		const Entity lit = scene.GetEntityByUUID(*UUIDFromJson(result["instances"][999]["id"]));
		REQUIRE(lit.HasComponent<PointLightComponent>());
		CHECK(lit.GetComponent<PointLightComponent>().Intensity == 3.0f);
		// Siblings get numbered names, as one at a time would.
		CHECK(scene.GetEntityByUUID(*UUIDFromJson(result["instances"][1]["id"])).GetName() != placed.GetName());

		harness.Run("edit.undo");
		CHECK(harness.SceneSnapshot() == before);

		// Checked before anything is made: a bad instance, or instances with parent and components.
		const size_t count = scene.GetEntityCount();
		CHECK(harness.Commands.Execute(harness.Context, "prefab.instantiate", { { "prefab", prefab }, { "instances", { { { "components", { { "Transform",
			{ { "Translation", "far" } } } } } } } } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Commands.Execute(harness.Context, "prefab.instantiate", { { "prefab", prefab }, { "instances", { { { "colour", 1 } } } } })
			.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Commands.Execute(harness.Context, "prefab.instantiate", { { "prefab", prefab }, { "parent", holder },
			{ "instances", { nlohmann::json::object() } } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(scene.GetEntityCount() == count);
	}

	TEST_CASE("project.create refuses an empty directory as invalid parameters")
	{
		EditorContext context { EditorContextSpecification { false } };
		EditorCommandRegistry commands;
		for (const char* directory : { "", "  " })
		{
			CAPTURE(directory);
			const EditorCommandResult result = commands.Execute(context, "project.create", { { "directory", directory }, { "name", "Game" } });
			CHECK_FALSE(result.Success);
			CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
			CHECK(result.Error.find("directory") != std::string::npos);
			CHECK_FALSE(context.HasProject());
		}
	}

	TEST_CASE("editor.quit keeps unsaved changes unless forced")
	{
		CommandHarness unchanged;
		const nlohmann::json quit = unchanged.Run("editor.quit");
		CHECK(quit["quitting"] == true);
		CHECK(quit["discardedChanges"] == false);
		CHECK(unchanged.Context.IsQuitRequested());

		CommandHarness modified;
		modified.Run("entity.create", { { "name", "Unsaved" } });
		const EditorCommandResult refused = modified.Commands.Execute(modified.Context, "editor.quit");
		CHECK_FALSE(refused.Success);
		CHECK(refused.ErrorKind == EditorCommandError::Failed);
		CHECK(refused.Error.find("unsaved changes") != std::string::npos);
		CHECK(refused.Error.find("force") != std::string::npos);
		CHECK_FALSE(modified.Context.IsQuitRequested());
		CHECK(modified.Commands.Execute(modified.Context, "editor.quit", { { "force", "yes" } }).ErrorKind == EditorCommandError::InvalidParameters);

		const nlohmann::json forced = modified.Run("editor.quit", { { "force", true } });
		CHECK(forced["discardedChanges"] == true);
		CHECK(modified.Context.IsQuitRequested());
	}

	TEST_CASE("Entities are created, edited and deleted with undo")
	{
		CommandHarness harness;
		const nlohmann::json empty = harness.SceneSnapshot();

		const std::string parent = harness.Run("entity.create", { { "name", "Parent" } })["id"].get<std::string>();
		const std::string child = harness.Run("entity.create", {
			{ "name", "Lamp" },
			{ "parent", parent },
			{ "components", { { "Transform", { { "Translation", { 1, 2, 3 } } } }, { "PointLight", { { "Intensity", 5.0 } } } } } })["id"].get<std::string>();

		nlohmann::json lamp = harness.Run("entity.get", { { "entity", child } });
		CHECK(lamp["name"] == "Lamp");
		CHECK(lamp["parent"] == parent);
		CHECK(lamp["components"]["PointLight"]["Intensity"] == 5.0);
		CHECK(lamp["components"]["Transform"]["Translation"] == nlohmann::json({ 1.0, 2.0, 3.0 }));

		harness.Run("component.set", { { "entity", child }, { "component", "PointLight" }, { "values", { { "Range", 4.5 } } } });
		CHECK(harness.Run("entity.rename", { { "entity", child }, { "name", "Bulb" } }) == nlohmann::json { { "entity", child }, { "name", "Bulb" } });
		CHECK(harness.Run("entity.find", { { "name", "Bulb" } })["entities"] == nlohmann::json::array({ child }));
		CHECK(harness.Run("entity.find", { { "component", "PointLight" } })["entities"].size() == 1);
		const nlohmann::json edited = harness.SceneSnapshot();

		// The IDs given (each once; the child goes with its parent) and how many went.
		CHECK(harness.Run("entity.delete", { { "entities", { parent, child, parent } } }) == nlohmann::json { { "deleted", { parent, child } }, { "count", 2 } });
		CHECK(harness.Run("scene.info")["entityCount"] == 0);
		CHECK(harness.Context.IsSceneModified());

		// Undo everything step by step, then redo it all.
		harness.Run("edit.undo");
		CHECK(harness.SceneSnapshot() == edited);
		for (int step = 0; step < 4; step++)
			harness.Run("edit.undo");
		CHECK(harness.SceneSnapshot() == empty);
		CHECK(harness.Error("edit.undo") == "Nothing to undo");
		for (int step = 0; step < 4; step++)
			harness.Run("edit.redo");
		CHECK(harness.SceneSnapshot() == edited);
	}

	TEST_CASE("Deleting many entities of a long sibling list is one undo step that restores every position")
	{
		CommandHarness harness;
		Scene& scene = *harness.Context.GetEditScene();
		std::vector<UUID> roots;
		for (int index = 0; index < 2000; index++)
		{
			Entity root = scene.CreateEntity("Root" + std::to_string(index));
			root.AddComponent<TagComponent>(index % 5 == 0 ? "Doomed" : "Kept");
			roots.push_back(root.GetUUID());
		}
		scene.CreateChildEntity(scene.GetEntityByUUID(roots[5]), "Child");
		const nlohmann::json original = harness.SceneSnapshot();
		CHECK(scene.FindEntitiesByTag("Doomed").size() == 400);

		nlohmann::json doomed = nlohmann::json::array();
		for (size_t index = 0; index < roots.size(); index += 5)
			doomed.push_back(UUIDToJson(roots[index]));
		harness.Context.SetSelection({ roots[0], roots[1], roots[5] });
		const size_t history = harness.Context.GetUndoStack().GetHistory().size();
		harness.Run("entity.delete", { { "entities", doomed } });
		CHECK(harness.Context.GetUndoStack().GetHistory().size() == history + 1);
		CHECK(scene.GetEntityCount() == 1600);
		CHECK(harness.Context.GetSelection() == std::vector<UUID> { roots[1] });
		CHECK_FALSE(harness.Context.IsSelected(roots[0]));
		CHECK(harness.Context.IsSelected(roots[1]));
		CHECK(scene.FindEntitiesByTag("Doomed").empty());
		CHECK_FALSE(scene.FindEntityByName("Child").IsValid());
		std::string error;
		CHECK_MESSAGE(scene.ValidateHierarchy(&error), error);

		harness.Run("edit.undo");
		CHECK(harness.SceneSnapshot() == original);
		CHECK(scene.GetRootEntities().size() == 2000);
		CHECK(scene.FindEntitiesByTag("Doomed").size() == 400);
		REQUIRE(scene.GetEntityByUUID(roots[5]));
		CHECK(scene.FindEntityByName("Child").GetParent() == scene.GetEntityByUUID(roots[5]));
		CHECK_MESSAGE(scene.ValidateHierarchy(&error), error);
		scene.UpdateWorldTransforms();
		CHECK_MESSAGE(scene.ValidateWorldTransforms(&error), error);

		harness.Run("edit.redo");
		CHECK(scene.GetEntityCount() == 1600);
		harness.Run("edit.undo");
		CHECK(harness.SceneSnapshot() == original);
	}

	TEST_CASE("Name and tag lookups follow renames, tag edits and their undo")
	{
		CommandHarness harness;
		Scene& scene = *harness.Context.GetEditScene();
		const std::string id = harness.Run("entity.create", { { "name", "Before" }, { "components", { { "Tag", { { "Tag", "Red" } } } } } })["id"].get<std::string>();
		const Entity entity = scene.FindEntityByName("Before");
		REQUIRE(entity);
		CHECK(scene.FindEntitiesByTag("Red") == std::vector<Entity> { entity });

		harness.Run("entity.rename", { { "entity", id }, { "name", "After" } });
		harness.Run("component.set", { { "entity", id }, { "component", "Tag" }, { "values", { { "Tag", "Blue" } } } });
		CHECK(scene.FindEntityByName("After") == entity);
		CHECK_FALSE(scene.FindEntityByName("Before").IsValid());
		CHECK(scene.FindEntitiesByTag("Red").empty());
		CHECK(scene.FindEntitiesByTag("Blue") == std::vector<Entity> { entity });

		harness.Run("edit.undo");
		harness.Run("edit.undo");
		CHECK(scene.FindEntityByName("Before") == entity);
		CHECK_FALSE(scene.FindEntityByName("After").IsValid());
		CHECK(scene.FindEntitiesByTag("Red") == std::vector<Entity> { entity });
		CHECK(scene.FindEntitiesByTag("Blue").empty());

		harness.Run("component.remove", { { "entity", id }, { "component", "Tag" } });
		CHECK(scene.FindEntitiesByTag("Red").empty());
		harness.Run("edit.undo");
		CHECK(scene.FindEntitiesByTag("Red") == std::vector<Entity> { entity });
		std::string error;
		CHECK_MESSAGE(scene.ValidateHierarchy(&error), error);
	}

	TEST_CASE("The selection keeps its order and answers membership")
	{
		CommandHarness harness;
		Scene& scene = *harness.Context.GetEditScene();
		const UUID a = scene.CreateEntity("A").GetUUID();
		const UUID b = scene.CreateEntity("B").GetUUID();
		const UUID c = scene.CreateEntity("C").GetUUID();

		harness.Context.SetSelection({ c, a, c, UUID(0x1234), b });
		CHECK(harness.Context.GetSelection() == std::vector<UUID> { c, a, b });
		harness.Context.Select(a, true);
		CHECK(harness.Context.GetSelection() == std::vector<UUID> { c, b, a });
		CHECK(harness.Context.GetPrimarySelection().GetUUID() == a);
		harness.Context.Deselect(c);
		CHECK_FALSE(harness.Context.IsSelected(c));
		CHECK(harness.Context.IsSelected(b));
		harness.Context.Select(c);
		CHECK(harness.Context.GetSelection() == std::vector<UUID> { c });
		CHECK_FALSE(harness.Context.IsSelected(a));
		harness.Context.ClearSelection();
		CHECK(harness.Context.GetSelection().empty());
		CHECK_FALSE(harness.Context.IsSelected(c));
	}

	TEST_CASE("The inspector shows an asset until an entity is selected")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorInspection");
		CommandHarness harness;
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Game" } });
		const std::string material = harness.Run("material.create", { { "path", "Materials/Red.stmat" } })["asset"].get<std::string>();
		const std::string entity = harness.Run("entity.create", { { "name", "Cube" } })["id"].get<std::string>();
		EditorContext& context = harness.Context;
		CHECK(context.GetInspectionTarget() == InspectionTarget::Entities);
		CHECK(harness.Run("selection.get")["inspectedAsset"].is_null());

		harness.Run("selection.set", { { "entities", { entity } } });
		uint64_t version = context.GetInspectionVersion();
		harness.Run("selection.set", { { "asset", "Materials/Red.stmat" } });
		CHECK(context.GetInspectionTarget() == InspectionTarget::Asset);
		CHECK(UUIDToJson(context.GetInspectedAsset()) == material);
		CHECK(context.GetInspectionVersion() != version);
		// The entities stay selected (their outline and gizmo stay) while the asset is inspected.
		nlohmann::json selection = harness.Run("selection.get");
		CHECK(selection["entities"] == nlohmann::json::array({ entity }));
		CHECK(selection["inspectedAsset"] == material);
		CHECK(harness.Run("editor.status", { { "sections", { "selection" } } })["selection"]["inspectedAsset"] == material);

		// Selecting an entity inspects the entities again; clearing the selection does not.
		version = context.GetInspectionVersion();
		context.ClearSelection();
		CHECK(context.GetInspectionTarget() == InspectionTarget::Asset);
		context.Select(*UUIDFromJson(nlohmann::json(entity)));
		CHECK(context.GetInspectionTarget() == InspectionTarget::Entities);
		CHECK(context.GetInspectionVersion() != version);
		CHECK(context.SetInspectedAsset(*UUIDFromJson(nlohmann::json(material))));
		harness.Run("selection.set", { { "entities", nlohmann::json::array() } });
		CHECK(context.GetInspectionTarget() == InspectionTarget::Asset);
		harness.Run("selection.set", { { "asset", nullptr } });
		CHECK(context.GetInspectionTarget() == InspectionTarget::Entities);

		// Unknown assets are refused; a deleted asset is no longer inspected; neither is anything after the project closed.
		CHECK_FALSE(context.SetInspectedAsset(UUID(0x1234)));
		CHECK(harness.Commands.Execute(context, "selection.set", { { "asset", "Materials/Missing.stmat" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Commands.Execute(context, "selection.set", nlohmann::json::object()).ErrorKind == EditorCommandError::InvalidParameters);
		harness.Run("selection.set", { { "asset", material } });
		harness.Run("asset.delete", { { "asset", material } });
		context.Update(Timestep(1.0f / 60.0f));
		CHECK(context.GetInspectionTarget() == InspectionTarget::Entities);
		CHECK(context.SetInspectedAsset(BuiltinAssets::DefaultMaterial));
		context.CloseProject();
		CHECK(context.GetInspectionTarget() == InspectionTarget::Entities);
	}

	TEST_CASE("Invalid edits fail without changing the scene or the history")
	{
		CommandHarness harness;
		const std::string entity = harness.Run("entity.create", { { "components", { { "PointLight", nlohmann::json::object() } } } })["id"].get<std::string>();
		const nlohmann::json before = harness.SceneSnapshot();
		const size_t history = harness.Context.GetUndoStack().GetHistory().size();

		CHECK(harness.Error("entity.create", { { "components", { { "NoSuchComponent", nlohmann::json::object() } } } }).find("Unknown component") != std::string::npos);
		CHECK_FALSE(harness.Error("component.set", { { "entity", entity }, { "component", "PointLight" }, { "values", { { "Intensity", 2.0 }, { "Color", "red" } } } }).empty());
		CHECK_FALSE(harness.Error("component.set", { { "entity", entity }, { "component", "PointLight" }, { "values", { { "NoSuchProperty", 1 } } } }).empty());
		CHECK(harness.Error("component.set", { { "entity", entity }, { "component", "SpotLight" }, { "values", { { "Range", 1 } } } }).find("component.add") != std::string::npos);
		CHECK_FALSE(harness.Error("entity.get", { { "entity", "123" } }).empty());
		CHECK_FALSE(harness.Error("entity.get", { { "entity", 42 } }).empty());
		CHECK_FALSE(harness.Error("entity.setParent", { { "entity", entity }, { "parent", entity } }).empty());
		CHECK_FALSE(harness.Error("entity.delete", { { "entities", nlohmann::json::array() } }).empty());
		CHECK_FALSE(harness.Error("component.remove", { { "entity", entity }, { "component", "Transform" } }).empty());

		CHECK(harness.SceneSnapshot() == before);
		CHECK(harness.Context.GetUndoStack().GetHistory().size() == history);
	}

	TEST_CASE("Creating entities in a full scene fails without changing the scene or the history")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorCommandFullScene");
		CommandHarness harness;
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Game" } });
		const std::string cube = harness.Run("entity.create", { { "name", "Cube" } })["id"].get<std::string>();
		harness.Run("prefab.create", { { "entities", { cube } }, { "path", "Prefabs/Cube.stprefab" } });

		// The registry is filled with plain EnTT entities: they count against its limit like scene entities, and are much
		// quicker to make.
		entt::registry& registry = harness.Context.GetEditScene()->GetRegistry();
		std::vector<entt::entity> filler(Scene::c_MaxEntities - registry.storage<entt::entity>().free_list());
		registry.create(filler.begin(), filler.end());
		const nlohmann::json before = harness.SceneSnapshot();
		const size_t history = harness.Context.GetUndoStack().GetHistory().size();

		CHECK(harness.Error("entity.create", { { "name", "OneTooMany" } }) == "The scene is full: it holds at most 1048575 entities");
		CHECK(harness.Error("entity.create", { { "name", "ChildTooMany" }, { "parent", cube } }) == "The scene is full: it holds at most 1048575 entities");
		const std::string instantiateError = harness.Error("prefab.instantiate", { { "prefab", "Prefabs/Cube.stprefab" } });
		CHECK(instantiateError.find("Instantiating 'Prefabs/Cube.stprefab' failed") != std::string::npos);
		CHECK(instantiateError.find("at most 1048575 entities") != std::string::npos);
		CHECK(harness.SceneSnapshot() == before);
		CHECK(harness.Context.GetUndoStack().GetHistory().size() == history);

		// Room again once an entity is gone.
		registry.destroy(filler.back());
		harness.Run("entity.create", { { "name", "Fits" }, { "parent", cube } });
		CHECK(harness.Run("scene.info")["entityCount"] == 2);
		CHECK(harness.Context.GetUndoStack().GetHistory().size() == history + 1);
	}

	TEST_CASE("Hierarchy, duplication, components and selection")
	{
		CommandHarness harness;
		const std::string a = harness.Run("entity.create", { { "name", "A" } })["id"].get<std::string>();
		const std::string b = harness.Run("entity.create", { { "name", "B" } })["id"].get<std::string>();
		const std::string c = harness.Run("entity.create", { { "name", "C" }, { "parent", a } })["id"].get<std::string>();

		harness.Run("entity.setParent", { { "entity", b }, { "parent", a }, { "index", 0 } });
		nlohmann::json hierarchy = harness.Run("scene.hierarchy")["entities"];
		REQUIRE(hierarchy.size() == 3);
		CHECK(hierarchy[0]["id"] == a);
		CHECK(hierarchy[1]["id"] == b);
		CHECK(hierarchy[1]["depth"] == 1);
		CHECK(hierarchy[2]["id"] == c);

		const std::string copy = harness.Run("entity.duplicate", { { "entity", a } })["id"].get<std::string>();
		CHECK(harness.Run("entity.get", { { "entity", copy } })["children"].size() == 2);
		CHECK(harness.Run("scene.info")["entityCount"] == 6);

		harness.Run("component.add", { { "entity", c }, { "component", "SpotLight" }, { "values", { { "OuterConeAngle", 40.0 } } } });
		CHECK(harness.Run("component.get", { { "entity", c }, { "component", "SpotLight" } })["values"]["OuterConeAngle"] == 40.0);
		// Properties another one turns off are listed with why; none: no inactive key.
		CHECK_FALSE(harness.Run("component.get", { { "entity", c }, { "component", "SpotLight" } }).contains("inactive"));
		harness.Run("component.add", { { "entity", c }, { "component", "PostProcess" }, { "values", { { "Bloom", false } } } });
		const nlohmann::json post = harness.Run("component.get", { { "entity", c }, { "component", "PostProcess" } });
		CHECK(post["inactive"] == nlohmann::json { { "BloomIntensity", "Bloom is off" }, { "BloomThreshold", "Bloom is off" } });
		CHECK(harness.Run("entity.get", { { "entity", c } })["inactive"] == nlohmann::json { { "PostProcess", post["inactive"] } });
		harness.Run("component.remove", { { "entity", c }, { "component", "PostProcess" } });
		CHECK_FALSE(harness.Run("entity.get", { { "entity", c } }).contains("inactive"));
		harness.Run("component.remove", { { "entity", c }, { "component", "SpotLight" } });
		CHECK_FALSE(harness.Error("component.get", { { "entity", c }, { "component", "SpotLight" } }).empty());

		harness.Run("entity.setActive", { { "entity", a }, { "active", false } });
		CHECK(harness.Run("entity.get", { { "entity", a } })["active"] == false);

		harness.Run("selection.set", { { "entities", { a, c } } });
		CHECK(harness.Run("selection.get")["entities"] == nlohmann::json::array({ a, c }));
		CHECK(UUIDToJson(harness.Context.GetPrimarySelection().GetUUID()) == c);
		harness.Run("entity.delete", { { "entities", { c } } });
		CHECK(harness.Run("selection.get")["entities"] == nlohmann::json::array({ a }));

		const nlohmann::json components = harness.Run("component.list")["components"];
		CHECK(std::any_of(components.begin(), components.end(), [](const nlohmann::json& component) { return component["name"] == "MeshRenderer"; }));
		CHECK(std::none_of(components.begin(), components.end(), [](const nlohmann::json& component) { return component["name"] == "Relationship"; }));
	}

	TEST_CASE("Play mode runs a copy and discards its changes")
	{
		CommandHarness harness;
		const std::string entity = harness.Run("entity.create", { { "name", "Player" } })["id"].get<std::string>();
		const nlohmann::json edited = harness.SceneSnapshot();
		const size_t history = harness.Context.GetUndoStack().GetHistory().size();

		CHECK(harness.Run("play.start")["state"] == "Play");
		CHECK_FALSE(harness.Error("play.start").empty());
		CHECK(harness.Context.GetActiveScene() != harness.Context.GetEditScene());
		harness.Run("entity.rename", { { "entity", entity }, { "name", "Changed" } });
		harness.Run("entity.create", { { "name", "Spawned" } });
		CHECK(harness.Context.GetUndoStack().GetHistory().size() == history); // Not recorded while playing
		CHECK_FALSE(harness.Error("edit.undo").empty());

		harness.Run("play.pause", { { "paused", true } });
		harness.Run("play.step", { { "frames", 2 } });
		CHECK(harness.Run("play.state")["paused"] == true);
		harness.Context.Update(Timestep(1.0f / 60.0f));

		CHECK(harness.Run("play.stop")["state"] == "Edit");
		CHECK(harness.SceneSnapshot() == edited);
		CHECK(harness.Run("play.simulate")["state"] == "Simulate");
		harness.Run("play.stop");
		CHECK_FALSE(harness.Error("play.pause", { { "paused", true } }).empty());
	}

	TEST_CASE("Projects, materials, prefabs and scenes round trip through assets")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorCommandProject");
		CommandHarness harness;
		CHECK(harness.Run("project.info")["open"] == false);
		CHECK_FALSE(harness.Error("material.create", { { "path", "Red.stmat" } }).empty());

		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Game" } });
		CHECK(harness.Run("project.info")["name"] == "Game");

		const std::string material = harness.Run("material.create", { { "path", "Materials/Red.stmat" }, { "properties", { { "BaseColor", { 1, 0, 0, 1 } }, { "Roughness", 0.3 } } } })["asset"].get<std::string>();
		// An unknown property names the ones that exist, so a client can correct itself.
		const std::string unknown = harness.Error("material.create", { { "path", "Materials/Bad.stmat" }, { "properties", { { "Shininess", 1 } } } });
		CHECK(unknown.find("'Shininess'") != std::string::npos);
		CHECK(unknown.find("BaseColor") != std::string::npos);
		CHECK(unknown.find("AlphaMode") != std::string::npos);
		harness.Run("material.set", { { "material", "Materials/Red.stmat" }, { "properties", { { "Metallic", 1.0 } } } });

		// material.get: the values and what every property accepts.
		const nlohmann::json described = harness.Run("material.get", { { "material", "Materials/Red.stmat" } });
		CHECK(described["asset"] == material);
		CHECK(described["values"]["BaseColor"] == nlohmann::json { 1.0, 0.0, 0.0, 1.0 });
		CHECK(described["values"]["Metallic"] == 1.0);
		bool describesAlphaMode = false;
		for (const nlohmann::json& property : described["properties"])
		{
			if (property["Name"] == "AlphaMode")
				describesAlphaMode = property["Type"] == "Enum" && property["Options"] == nlohmann::json { "Opaque", "Mask", "Blend" };
		}
		CHECK(describesAlphaMode);
		CHECK(described["properties"].size() == described["values"].size());
		CHECK(harness.Run("material.get", { { "material", "Builtin/DefaultMaterial" } })["values"]["BaseColor"].is_array());
		CHECK_FALSE(harness.Error("material.get", { { "material", "Builtin/Cube" } }).empty());
		const nlohmann::json materialInfo = harness.Run("asset.info", { { "asset", material } });
		CHECK(materialInfo["type"] == "Material");
		CHECK(materialInfo["path"] == "Materials/Red.stmat");

		// Asset properties accept paths as well as handles.
		const std::string byPath = harness.Run("entity.create", { { "components", { { "MeshRenderer", { { "Material", "Materials/Red.stmat" } } } } } })["id"].get<std::string>();
		CHECK(harness.Run("component.get", { { "entity", byPath }, { "component", "MeshRenderer" } })["values"]["Material"] == material);
		CHECK(harness.Error("entity.create", { { "components", { { "MeshRenderer", { { "Material", "Materials/Missing.stmat" } } } } } }).find("no asset") != std::string::npos);
		harness.Run("entity.delete", { { "entities", { byPath } } });

		// A cube using the material, saved as a prefab and instantiated twice.
		const std::string cube = harness.Run("entity.create", {
			{ "name", "Cube" },
			{ "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) }, { "Material", material } } } } } })["id"].get<std::string>();
		const std::string prefab = harness.Run("prefab.create", { { "entities", { cube } }, { "path", "Prefabs/Cube.stprefab" } })["asset"].get<std::string>();
		const nlohmann::json instance = harness.Run("prefab.instantiate", {
			{ "prefab", "Prefabs/Cube.stprefab" },
			{ "components", { { "Transform", { { "Translation", { 3, 0, 0 } } } } } } })["entities"];
		REQUIRE(instance.size() == 1);
		const nlohmann::json instanceEntity = harness.Run("entity.get", { { "entity", instance[0] } });
		CHECK(instanceEntity["components"]["MeshRenderer"]["Material"] == material);
		CHECK(instanceEntity["components"]["Transform"]["Translation"][0] == 3.0);
		harness.Run("edit.undo");
		CHECK(harness.Run("scene.info")["entityCount"] == 1);
		harness.Run("edit.redo");

		// Save, change, reopen: the saved state comes back.
		CHECK_FALSE(harness.Error("scene.save").empty()); // Never saved: needs a path
		CHECK_FALSE(harness.Context.GetEditScene()->GetSourceAsset().IsValid());
		const std::string scene = harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } })["scene"].get<std::string>();
		CHECK(harness.Run("scene.info")["modified"] == false);
		CHECK(harness.Run("scene.info")["name"] == "Main"); // Named after its file
		CHECK(UUIDToJson(harness.Context.GetEditScene()->GetSourceAsset()) == scene); // From now on it is that asset's scene
		const nlohmann::json saved = harness.SceneSnapshot();
		harness.Run("entity.delete", { { "entities", { cube } } });
		CHECK(harness.Run("scene.info")["modified"] == true);
		CHECK(harness.Run("scene.open", { { "scene", "Scenes/Main.stscene" }, { "force", true } })["discardedChanges"] == true);
		CHECK(harness.SceneSnapshot() == saved);
		CHECK(harness.Run("scene.info")["scene"] == scene);
		CHECK(UUIDToJson(harness.Context.GetEditScene()->GetSourceAsset()) == scene);
		CHECK_FALSE(harness.Context.GetUndoStack().CanUndo());

		harness.Run("project.setStartScene", { { "scene", scene } });
		const nlohmann::json assets = harness.Run("asset.list", { { "includeBuiltin", false } })["assets"];
		CHECK(assets.size() == 3);
		CHECK_FALSE(harness.Error("asset.delete", { { "asset", scene } }).empty()); // The open scene

		// Reopening the project continues with its start scene.
		CommandHarness reopened;
		reopened.Run("project.open", { { "path", FileSystem::ToUTF8(directory / "Game") } });
		CHECK(reopened.Run("scene.info")["scene"] == scene);
		CHECK(reopened.SceneSnapshot() == saved);
		CHECK(prefab.size() == 16);
	}

	TEST_CASE("Editors without a project have the built-in assets")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorBuiltinAssets");
		{
			CommandHarness harness;
			REQUIRE(AssetManager::HasActive());
			CHECK(harness.Context.GetAssetManager() == nullptr); // No project assets
			CHECK(AssetManager::GetAsset<Mesh>(BuiltinAssets::CubeMesh) != nullptr);
			CHECK(AssetManager::GetAsset<Material>(BuiltinAssets::DefaultMaterial) != nullptr);
			const std::string cube = harness.Run("entity.create", { { "components", { { "MeshRenderer", { { "Mesh", "Builtin/Cube" } } } } } })["id"].get<std::string>();
			CHECK(harness.Run("component.get", { { "entity", cube }, { "component", "MeshRenderer" } })["values"]["Mesh"] == UUIDToJson(BuiltinAssets::CubeMesh));

			// A project's asset manager replaces it while the project is open.
			harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Game" }, { "force", true } });
			CHECK(AssetManager::GetActive().get() == harness.Context.GetAssetManager());
			harness.Context.CloseProject();
			REQUIRE(AssetManager::HasActive());
			CHECK(AssetManager::GetAsset<Mesh>(BuiltinAssets::CubeMesh) != nullptr);
		}
		CHECK_FALSE(AssetManager::HasActive());
	}

	TEST_CASE("Exported games run in the game runtime")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorExport");
		const std::filesystem::path output = directory / "Build";
		CommandHarness harness;
		CHECK_FALSE(harness.Error("project.export", { { "directory", FileSystem::ToUTF8(output) } }).empty()); // No project

		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "My Game!" } });
		CHECK(harness.Error("project.export", { { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false } }).find("start scene") != std::string::npos);
		const std::string material = harness.Run("material.create", { { "path", "Red.stmat" }, { "properties", { { "BaseColor", { 1, 0, 0, 1 } } } } })["asset"].get<std::string>();
		harness.Run("entity.create", { { "name", "Player" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) }, { "Material", material } } } } } });

		const std::string scene = harness.Run("scene.saveAs", { { "path", "Main.stscene" } })["scene"].get<std::string>();
		harness.Run("entity.create", { { "name", "Unsaved" } });
		CHECK(harness.Error("project.export", { { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false } }).find("unsaved") != std::string::npos);
		harness.Run("edit.undo");
		CHECK(harness.Error("project.export", { { "directory", FileSystem::ToUTF8(directory / "Game" / "Build") }, { "includeRuntime", false } }).find("outside") != std::string::npos);
		CHECK(harness.Error("project.export", { { "directory", "relative/path" }, { "includeRuntime", false } }).find("absolute") != std::string::npos);
		CHECK(harness.Error("project.export", { { "directory", FileSystem::ToUTF8(output) }, { "runtime", FileSystem::ToUTF8(directory / "Missing.exe") } }).find("runtime") != std::string::npos);

		// Any file can stand in for the runtime executable: it is copied and named after the game, with the notices.
		REQUIRE(FileSystem::WriteText(directory / "FakeRuntime.bin", "runtime"));
		CHECK(harness.Error("project.export", { { "directory", FileSystem::ToUTF8(directory / "WithRuntime") }, { "runtime", FileSystem::ToUTF8(directory / "FakeRuntime.bin") } })
			.find("ThirdPartyNotices") != std::string::npos);
		REQUIRE(FileSystem::WriteText(directory / "ThirdPartyNotices.md", "notices"));
		const nlohmann::json withRuntime = Tests::RunCommandToCompletion(harness.Context, harness.Commands, "project.export", { { "directory", FileSystem::ToUTF8(directory / "WithRuntime") }, { "runtime", FileSystem::ToUTF8(directory / "FakeRuntime.bin") } });
		CHECK(FileSystem::ReadText(FileSystem::FromUTF8(withRuntime["executable"].get<std::string>())) == "runtime");
		CHECK(FileSystem::FromUTF8(withRuntime["executable"].get<std::string>()).stem() == "My Game_");
		CHECK(FileSystem::ReadText(directory / "WithRuntime" / "ThirdPartyNotices.md") == "notices");

		const nlohmann::json exported = Tests::RunCommandToCompletion(harness.Context, harness.Commands, "project.export", { { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false }, { "width", 640 } });
		CHECK(exported["executable"].is_null());
		const std::filesystem::path manifest = FileSystem::FromUTF8(exported["manifest"].get<std::string>());
		CHECK(manifest.filename() == "My Game_.stgame");
		CHECK(FileSystem::IsRegularFile(FileSystem::FromUTF8(exported["assetPack"].get<std::string>())));

		// The runtime replaces the editor's asset manager while it lives.
		std::string error;
		Scope<GameRuntime> runtime = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(runtime, error);
		// The start scene loads in the background; nothing plays until it is in.
		CHECK_FALSE(runtime->HasStarted());
		CHECK(runtime->IsLoadingScene());
		REQUIRE(runtime->WaitForStart());
		CHECK_FALSE(runtime->IsLoadingScene());
		CHECK(runtime->GetManifest().Name == "My Game!");
		CHECK(runtime->GetManifest().WindowWidth == 640);
		CHECK(UUIDToJson(runtime->GetSceneHandle()) == scene);
		CHECK(UUIDToJson(runtime->GetScene()->GetSourceAsset()) == scene);
		REQUIRE(runtime->GetScene()->IsRunning());
		CHECK_FALSE(runtime->GetScene()->IsTimeDilated()); // Tools and tests step deterministically by default
		for (int frame = 0; frame < 5; frame++)
			runtime->Update(Timestep(1.0f / 60.0f));
		Entity player = runtime->GetScene()->FindEntityByName("Player");
		REQUIRE(player);
		CHECK(UUIDToJson(player.GetComponent<MeshRendererComponent>().Material) == material);
		CHECK(AssetManager::LoadAssetSync<Material>(player.GetComponent<MeshRendererComponent>().Material) != nullptr);
		CHECK_FALSE(runtime->LoadScene(UUID(0x1234), &error));
		CHECK(runtime->GetScene()->IsRunning()); // The current scene keeps running
		runtime.reset();
		CHECK_FALSE(AssetManager::HasActive());

		// Games on the wall clock play their scenes with time dilation.
		GameRuntimeOptions wallClock;
		wallClock.TimeDilation = true;
		runtime = GameRuntime::Create(manifest, &error, wallClock);
		REQUIRE_MESSAGE(runtime, error);
		REQUIRE(runtime->WaitForStart());
		CHECK(runtime->GetScene()->IsTimeDilated());
		runtime.reset();

		// A broken pack fails cleanly.
		REQUIRE(FileSystem::WriteText(FileSystem::FromUTF8(exported["assetPack"].get<std::string>()), "garbage"));
		CHECK_FALSE(GameRuntime::Create(manifest, &error));
	}

	TEST_CASE("Scene settings changes are undoable")
	{
		CommandHarness harness;
		harness.Run("scene.setSettings", { { "gravity", { 0, -1.62, 0 } }, { "fixedTimestep", 0.02 } });
		CHECK(harness.Run("scene.info")["settings"]["gravity"][1].get<double>() == doctest::Approx(-1.62));
		CHECK_FALSE(harness.Error("scene.setSettings", { { "fixedTimestep", 5.0 } }).empty());
		CHECK_FALSE(harness.Error("scene.setSettings", { { "gravity", { 0, "down", 0 } } }).empty());
		harness.Run("edit.undo");
		CHECK(harness.Run("scene.info")["settings"]["gravity"][1].get<double>() == doctest::Approx(-9.81));
	}

	TEST_CASE("Play mode runs on the wall clock when the editor asks for time dilation")
	{
		for (const bool timeDilation : { false, true })
		{
			EditorContextSpecification specification;
			specification.WatchAssetFiles = false;
			specification.TimeDilation = timeDilation;
			EditorContext context(specification);
			std::string error;
			REQUIRE_MESSAGE(context.Play(&error), error);
			CHECK(context.GetActiveScene()->IsTimeDilated() == timeDilation);
			context.Stop();
			CHECK_FALSE(context.GetEditScene()->IsRunning());
		}
	}

	TEST_CASE("Physics capacities are validated and changed in one undo step")
	{
		CommandHarness harness;
		const nlohmann::json automatic = { { "maxBodies", 0 }, { "maxBodyPairs", 0 }, { "maxContactConstraints", 0 } };
		CHECK(harness.Run("scene.info")["settings"]["physicsCapacity"] == automatic);

		// Invalid values change nothing: they are not counts in range, or not capacities at all.
		const nlohmann::json invalid[] = {
			{ { "maxBodies", -1 } },
			{ { "maxBodies", uint64_t(PhysicsCapacitySettings::c_MaxBodiesLimit) + 1 } },
			{ { "maxBodyPairs", 2.5 } },
			{ { "maxContactConstraints", "lots" } },
			{ { "maxBodies", 64 }, { "gravityScale", 2 } },
			nlohmann::json::array({ 64 }) };
		for (const nlohmann::json& capacity : invalid)
		{
			INFO(capacity.dump());
			CHECK(harness.Commands.Execute(harness.Context, "scene.setSettings", { { "physicsCapacity", capacity }, { "fixedTimestep", 0.02 } }).ErrorKind == EditorCommandError::InvalidParameters);
		}
		CHECK(harness.Run("scene.info")["settings"]["physicsCapacity"] == automatic);
		CHECK(harness.Run("scene.info")["settings"]["fixedTimestep"].get<double>() == doctest::Approx(1.0 / 60.0));

		// Only the given capacities change, in one step with the other settings.
		const size_t steps = harness.Context.GetUndoStack().GetPosition();
		const nlohmann::json changed = harness.Run("scene.setSettings", { { "physicsCapacity", { { "maxBodies", 200000 }, { "maxContactConstraints", 50000 } } },
			{ "fixedTimestep", 0.02 } })["settings"];
		CHECK(changed["physicsCapacity"] == nlohmann::json { { "maxBodies", 200000 }, { "maxBodyPairs", 0 }, { "maxContactConstraints", 50000 } });
		CHECK(harness.Context.GetEditScene()->GetSettings().PhysicsCapacity == PhysicsCapacitySettings { 200000, 0, 50000 });
		CHECK(harness.Context.GetUndoStack().GetPosition() == steps + 1);

		harness.Run("edit.undo");
		CHECK(harness.Run("scene.info")["settings"]["physicsCapacity"] == automatic);
		CHECK(harness.Run("scene.info")["settings"]["fixedTimestep"].get<double>() == doctest::Approx(1.0 / 60.0));
		harness.Run("edit.redo");
		CHECK(harness.Context.GetEditScene()->GetSettings().PhysicsCapacity == PhysicsCapacitySettings { 200000, 0, 50000 });

		// The limits are accepted; one more is not.
		const PhysicsCapacitySettings limits { PhysicsCapacitySettings::c_MaxBodiesLimit, PhysicsCapacitySettings::c_MaxBodyPairsLimit,
			PhysicsCapacitySettings::c_MaxContactConstraintsLimit };
		const std::pair<const char*, uint64_t> beyondLimits[] = { { "maxBodyPairs", uint64_t(limits.MaxBodyPairs) + 1 },
			{ "maxContactConstraints", uint64_t(limits.MaxContactConstraints) + 1 } };
		for (const auto& [name, value] : beyondLimits)
		{
			INFO(name);
			CHECK(harness.Commands.Execute(harness.Context, "scene.setSettings", { { "physicsCapacity", { { name, value } } } }).ErrorKind
				== EditorCommandError::InvalidParameters);
		}
		harness.Run("scene.setSettings", { { "physicsCapacity", { { "maxBodies", limits.MaxBodies }, { "maxBodyPairs", limits.MaxBodyPairs },
			{ "maxContactConstraints", limits.MaxContactConstraints } } } });
		CHECK(harness.Context.GetEditScene()->GetSettings().PhysicsCapacity == limits);
		harness.Run("edit.undo");

		// While playing, a change reaches the running copy only, which play.stop discards, and capacities do not reach the
		// running world: the result warns about both.
		harness.Run("entity.create", { { "name", "Crate" }, { "components", { { "RigidBody", nlohmann::json::object() }, { "BoxCollider", nlohmann::json::object() } } } });
		harness.Run("play.start");
		const PhysicsSystem* physics = harness.Context.GetActiveScene()->GetSystem<PhysicsSystem>();
		REQUIRE(physics != nullptr);
		CHECK(physics->GetStats().MaxBodies == 200000);
		const size_t playSteps = harness.Context.GetUndoStack().GetPosition();
		const nlohmann::json playing = harness.Run("scene.setSettings", { { "physicsCapacity", { { "maxBodies", 1000 } } } });
		REQUIRE(playing.contains("warning"));
		CHECK(playing["warning"].get<std::string>().find("discarded by play.stop") != std::string::npos);
		CHECK(playing["warning"].get<std::string>().find("physics capacities apply to worlds created later") != std::string::npos);
		CHECK(harness.Context.GetActiveScene()->GetSettings().PhysicsCapacity.MaxBodies == 1000);
		CHECK(physics->GetStats().MaxBodies == 200000);
		const nlohmann::json gravityOnly = harness.Run("scene.setSettings", { { "gravity", { 0, -5, 0 } } });
		REQUIRE(gravityOnly.contains("warning"));
		CHECK(gravityOnly["warning"].get<std::string>().find("capacities") == std::string::npos);
		CHECK(harness.Context.GetUndoStack().GetPosition() == playSteps);
		harness.Run("play.stop");
		CHECK(harness.Context.GetEditScene()->GetSettings().PhysicsCapacity == PhysicsCapacitySettings { 200000, 0, 50000 });
		CHECK_FALSE(harness.Run("scene.setSettings", { { "physicsCapacity", { { "maxBodies", 300000 } } } }).contains("warning"));
	}

	TEST_CASE("Components this build does not register survive play, duplication, prefabs, undo and saving")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("UnknownComponentsProject") / "Game";
		{
			CommandHarness creator;
			creator.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
		}

		// A scene saved by a build with a vehicle module, which this one lacks.
		const nlohmann::json vehicle = { { "Wheels", 4 }, { "Engine", { { "Power", 310.5 }, { "Kind", "V8" } } }, { "Gears", { 1, 2, 3 } }, { "Owner", nullptr } };
		const std::string truck = "00000000000000AB";
		const nlohmann::json document = {
			{ "Strata", { { "Format", "Scene" }, { "Version", 1 } } },
			{ "Scene", { { "Name", "Garage" }, { "Entities", {
				{ { "ID", truck }, { "Components", { { "Name", { { "Name", "Truck" } } }, { "Vehicle", vehicle } } } }
			} } } }
		};
		REQUIRE(FileSystem::CreateDirectories(directory / "Assets" / "Scenes"));
		REQUIRE(FileSystem::WriteText(directory / "Assets" / "Scenes" / "Garage.stscene", JsonUtils::Dump(document, 1, '\t') + "\n"));

		// The vehicle component of the entity with `id` in a scene document's entity list (null when there is none).
		auto vehicleOf = [](const nlohmann::json& entities, const std::string& id) -> nlohmann::json
		{
			for (const nlohmann::json& entity : entities)
			{
				if (entity["ID"] == id)
					return entity["Components"].contains("Vehicle") ? entity["Components"]["Vehicle"] : nlohmann::json();
			}
			return nlohmann::json();
		};

		CommandHarness harness;
		harness.Run("project.open", { { "path", FileSystem::ToUTF8(directory) } });
		const uint64_t beforeOpen = Log::GetBuffer().GetLatestSequence();
		harness.Run("scene.open", { { "scene", "Scenes/Garage.stscene" } });
		size_t warnings = 0;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(beforeOpen))
			warnings += entry.Level == LogLevel::Warn && entry.Message.find("Unknown component 'Vehicle'") != std::string::npos ? 1 : 0;
		CHECK(warnings == 1);
		const nlohmann::json edited = harness.SceneSnapshot();
		CHECK(vehicleOf(edited, truck) == vehicle);

		// Play runs a copy that has it; stopping returns to the edited scene, which still has it.
		harness.Run("play.start");
		const Ref<Scene> running = harness.Context.GetActiveScene();
		REQUIRE(running != harness.Context.GetEditScene());
		CHECK(vehicleOf(SceneSerializer::Serialize(*running)["Scene"]["Entities"], truck) == vehicle);
		harness.Run("play.stop");
		CHECK(harness.SceneSnapshot() == edited);

		const std::string copy = harness.Run("entity.duplicate", { { "entity", truck } })["id"].get<std::string>();
		CHECK(vehicleOf(harness.SceneSnapshot(), copy) == vehicle);

		harness.Run("prefab.create", { { "entities", { truck } }, { "path", "Prefabs/Truck.stprefab" } });
		const nlohmann::json instance = harness.Run("prefab.instantiate", { { "prefab", "Prefabs/Truck.stprefab" } })["entities"];
		REQUIRE(instance.size() == 1);
		CHECK(vehicleOf(harness.SceneSnapshot(), instance[0].get<std::string>()) == vehicle);

		const nlohmann::json beforeDelete = harness.SceneSnapshot();
		harness.Run("entity.delete", { { "entities", { truck } } });
		CHECK(vehicleOf(harness.SceneSnapshot(), truck).is_null());
		harness.Run("edit.undo");
		CHECK(harness.SceneSnapshot() == beforeDelete);
		CHECK(vehicleOf(harness.SceneSnapshot(), truck) == vehicle);

		harness.Run("scene.saveAs", { { "path", "Scenes/Saved.stscene" } });
		const std::optional<std::string> text = FileSystem::ReadText(directory / "Assets" / "Scenes" / "Saved.stscene");
		REQUIRE(text);
		const std::optional<nlohmann::json> saved = JsonUtils::Parse(*text);
		REQUIRE(saved);
		CHECK(vehicleOf((*saved)["Scene"]["Entities"], truck) == vehicle);
		CHECK(vehicleOf((*saved)["Scene"]["Entities"], copy) == vehicle);
	}
}
