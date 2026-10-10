#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Version.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Material.h>
#include <Strata/Renderer/Mesh.h>
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

	TEST_CASE("editor.status gives an overview with the sections other editor parts provide")
	{
		CommandHarness harness;
		nlohmann::json status = harness.Run("editor.status");
		CHECK_FALSE(status["engineVersion"].get<std::string>().empty());
		// The commit the build was configured from: a short hash in a git checkout.
		const std::string commit = status["engineCommit"].get<std::string>();
		CHECK(commit == c_EngineCommit);
		CHECK((commit == "unknown" || (commit.size() == 12 && commit.find_first_not_of("0123456789abcdef") == std::string::npos)));
		CHECK_FALSE(status["platform"].get<std::string>().empty());
		CHECK(status["project"]["open"] == false);
		CHECK(status["scene"]["modified"] == false);
		CHECK(status["scene"]["scene"].is_null());
		CHECK(status["scene"]["entityCount"] == 0);
		CHECK(status["play"]["state"] == "Edit");
		CHECK(status["selection"]["count"] == 0);
		CHECK(status["selection"]["primary"].is_null());
		CHECK(status["undo"]["position"] == 0);

		const std::string entity = harness.Run("entity.create", { { "name", "Player" } })["id"].get<std::string>();
		harness.Run("selection.set", { { "entities", { entity } } });
		harness.Context.SetStatusProvider("automation", []() { return nlohmann::json { { "port", 1234 } }; });
		// A provider cannot replace a built-in section.
		harness.Context.SetStatusProvider("scene", []() { return nlohmann::json("replaced"); });
		status = harness.Run("editor.status");
		CHECK(status["scene"]["modified"] == true);
		CHECK(status["scene"]["entityCount"] == 1);
		CHECK(status["selection"]["count"] == 1);
		CHECK(status["selection"]["primary"] == entity);
		CHECK(status["undo"]["position"] == 1);
		CHECK(status["undo"]["count"] == 1);
		CHECK(status["undo"]["undo"] == "Create Entity");
		CHECK(status["automation"]["port"] == 1234);

		harness.Run("play.start");
		CHECK(harness.Run("editor.status")["play"]["state"] == "Play");
		harness.Run("play.stop");

		harness.Context.SetStatusProvider("automation", nullptr);
		harness.Context.SetStatusProvider("scene", nullptr);
		CHECK_FALSE(harness.Run("editor.status").contains("automation"));

		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorStatusProject");
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "StatusGame" } });
		const nlohmann::json project = harness.Run("editor.status")["project"];
		CHECK(project == harness.Run("project.info"));
		CHECK(project["name"] == "StatusGame");
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
			harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Game" } });
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
