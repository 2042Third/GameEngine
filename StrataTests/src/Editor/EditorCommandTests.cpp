#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Runtime/GameRuntime.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/SceneSerializer.h>

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

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		std::string Error(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
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
		harness.Run("entity.rename", { { "entity", child }, { "name", "Bulb" } });
		CHECK(harness.Run("entity.find", { { "name", "Bulb" } })["entities"] == nlohmann::json::array({ child }));
		CHECK(harness.Run("entity.find", { { "component", "PointLight" } })["entities"].size() == 1);
		const nlohmann::json edited = harness.SceneSnapshot();

		harness.Run("entity.delete", { { "entities", { parent } } });
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
		CHECK_FALSE(harness.Error("material.create", { { "path", "Materials/Bad.stmat" }, { "properties", { { "Shininess", 1 } } } }).empty());
		harness.Run("material.set", { { "material", "Materials/Red.stmat" }, { "properties", { { "Metallic", 1.0 } } } });
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
		const std::string scene = harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } })["scene"].get<std::string>();
		CHECK(harness.Run("scene.info")["modified"] == false);
		CHECK(harness.Run("scene.info")["name"] == "Main"); // Named after its file
		const nlohmann::json saved = harness.SceneSnapshot();
		harness.Run("entity.delete", { { "entities", { cube } } });
		CHECK(harness.Run("scene.info")["modified"] == true);
		harness.Run("scene.open", { { "scene", "Scenes/Main.stscene" } });
		CHECK(harness.SceneSnapshot() == saved);
		CHECK(harness.Run("scene.info")["scene"] == scene);
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
		const nlohmann::json withRuntime = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(directory / "WithRuntime") }, { "runtime", FileSystem::ToUTF8(directory / "FakeRuntime.bin") } });
		CHECK(FileSystem::ReadText(FileSystem::FromUTF8(withRuntime["executable"].get<std::string>())) == "runtime");
		CHECK(FileSystem::FromUTF8(withRuntime["executable"].get<std::string>()).stem() == "My Game_");
		CHECK(FileSystem::ReadText(directory / "WithRuntime" / "ThirdPartyNotices.md") == "notices");

		const nlohmann::json exported = harness.Run("project.export", { { "directory", FileSystem::ToUTF8(output) }, { "includeRuntime", false }, { "width", 640 } });
		CHECK(exported["executable"].is_null());
		const std::filesystem::path manifest = FileSystem::FromUTF8(exported["manifest"].get<std::string>());
		CHECK(manifest.filename() == "My Game_.stgame");
		CHECK(FileSystem::IsRegularFile(FileSystem::FromUTF8(exported["assetPack"].get<std::string>())));

		// The runtime replaces the editor's asset manager while it lives.
		std::string error;
		Scope<GameRuntime> runtime = GameRuntime::Create(manifest, &error);
		REQUIRE_MESSAGE(runtime, error);
		CHECK(runtime->GetManifest().Name == "My Game!");
		CHECK(runtime->GetManifest().WindowWidth == 640);
		CHECK(UUIDToJson(runtime->GetSceneHandle()) == scene);
		REQUIRE(runtime->GetScene()->IsRunning());
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
}
