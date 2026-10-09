#include <doctest/doctest.h>

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/PropertyEdit.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/SceneSerializer.h>

using namespace Strata;

namespace
{

	struct Harness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;

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

		std::string Create(const std::string& name, const nlohmann::json& extra = nlohmann::json::object())
		{
			nlohmann::json parameters = extra;
			parameters["name"] = name;
			return Run("entity.create", parameters)["id"].get<std::string>();
		}

		nlohmann::json Snapshot() const
		{
			return SceneSerializer::Serialize(*Context.GetEditScene())["Scene"]["Entities"];
		}
	};

	nlohmann::json ExampleValue(const nlohmann::json& schema)
	{
		const nlohmann::json type = schema.value("type", nlohmann::json("string"));
		const std::string kind = type.is_array() ? type[0].get<std::string>() : type.get<std::string>();
		if (kind == "boolean")
			return true;
		if (kind == "integer")
			return schema.value("minimum", 1);
		if (kind == "number")
			return 0.5;
		if (kind == "array")
			return nlohmann::json::array();
		if (kind == "object")
			return nlohmann::json::object();
		return "x";
	}

}

TEST_SUITE("Editor.Validation")
{
	TEST_CASE("Every command rejects unknown and missing parameters before doing anything")
	{
		Harness harness;
		harness.Create("Keep");
		const nlohmann::json before = harness.Snapshot();
		for (const EditorCommand* command : harness.Commands.GetAll())
		{
			CAPTURE(command->Name);
			// A misspelled parameter: always rejected, whatever else is given.
			const std::string unknown = harness.Error(command->Name, { { "definitelyNotAParameter", 1 } });
			CHECK(unknown.find("Unknown parameter 'definitelyNotAParameter'") != std::string::npos);

			// Each required parameter, left out while the others are present.
			const nlohmann::json& schema = command->Parameters;
			if (!schema.contains("required"))
				continue;
			for (const nlohmann::json& missing : schema["required"])
			{
				nlohmann::json parameters = nlohmann::json::object();
				for (const nlohmann::json& required : schema["required"])
				{
					if (required != missing)
						parameters[required.get<std::string>()] = ExampleValue(schema["properties"][required.get<std::string>()]);
				}
				CHECK(harness.Error(command->Name, parameters).find("Missing parameter '" + missing.get<std::string>() + "'") != std::string::npos);
			}
		}
		CHECK(harness.Snapshot() == before);
	}

	TEST_CASE("Component values are validated like in the inspector")
	{
		Harness harness;
		const std::string entity = harness.Create("Box");
		const nlohmann::json before = harness.Snapshot();

		// Built-in assets resolve without a project, by handle or "Builtin/<Name>".
		harness.Run("component.add", { { "entity", entity }, { "component", "MeshRenderer" }, { "values", { { "Mesh", "Builtin/Cube" } } } });
		CHECK(harness.Run("component.get", { { "entity", entity }, { "component", "MeshRenderer" } })["values"]["Mesh"] == UUIDToJson(BuiltinAssets::CubeMesh));
		// Wrong asset type, unknown handle, unknown property: rejected.
		CHECK(harness.Error("component.set", { { "entity", entity }, { "component", "MeshRenderer" }, { "values", { { "Mesh", UUIDToJson(BuiltinAssets::DefaultMaterial) } } } })
			.find("not a Mesh") != std::string::npos);
		CHECK(harness.Error("component.set", { { "entity", entity }, { "component", "MeshRenderer" }, { "values", { { "Material", "00000000DEADBEEF" } } } })
			.find("no asset") != std::string::npos);
		CHECK(harness.Error("component.set", { { "entity", entity }, { "component", "MeshRenderer" }, { "values", { { "Meshh", "Builtin/Cube" } } } })
			.find("unknown property 'Meshh'") != std::string::npos);
		// Hidden and internal components are not editable.
		CHECK_FALSE(harness.Error("component.add", { { "entity", entity }, { "component", "PrefabInstance" } }).empty());
		CHECK_FALSE(harness.Error("component.add", { { "entity", entity }, { "component", "Relationship" } }).empty());
		// Null clears a reference.
		harness.Run("component.set", { { "entity", entity }, { "component", "MeshRenderer" }, { "values", { { "Mesh", nullptr } } } });
		CHECK(harness.Run("component.get", { { "entity", entity }, { "component", "MeshRenderer" } })["values"]["Mesh"] == UUIDToJson(UUID::Null()));

		// Property names are matched case-insensitively and stored canonically.
		harness.Run("component.set", { { "entity", entity }, { "component", "Transform" }, { "values", { { "translation", { 1, 2, 3 } } } } });
		CHECK(harness.Run("component.get", { { "entity", entity }, { "component", "Transform" } })["values"]["Translation"][2] == 3.0);
		harness.Run("edit.undo");
		harness.Run("edit.undo");
		harness.Run("edit.undo");
		CHECK(harness.Snapshot() == before);
	}

	TEST_CASE("Changes during play warn that they are discarded")
	{
		Harness harness;
		const std::string entity = harness.Create("Player");
		harness.Run("play.start");
		const nlohmann::json created = harness.Run("entity.create", { { "name", "Temporary" } });
		CHECK(created["warning"].get<std::string>().find("discarded by play.stop") != std::string::npos);
		const nlohmann::json renamed = harness.Run("entity.rename", { { "entity", entity }, { "name", "Changed" } });
		CHECK(renamed.contains("warning"));
		harness.Run("play.stop");
		CHECK_FALSE(harness.Run("entity.rename", { { "entity", entity }, { "name", "Edited" } }).contains("warning"));
		CHECK_FALSE(harness.Error("play.step", { { "frames", 0 } }).empty());
		CHECK_FALSE(harness.Error("play.step").empty()); // Not paused
	}

	TEST_CASE("Deleting a parent prunes selected descendants and scene settings no-ops record nothing")
	{
		Harness harness;
		const std::string parent = harness.Create("Parent");
		const std::string child = harness.Create("Child", { { "parent", parent } });
		harness.Run("selection.set", { { "entities", { parent, child } } });
		CHECK_FALSE(harness.Error("selection.set", { { "entities", { "00000000000000AB" } } }).empty());
		CHECK(harness.Run("selection.get")["entities"].size() == 2); // Unchanged by the failure
		harness.Run("entity.delete", { { "entities", { parent } } });
		CHECK(harness.Run("selection.get")["entities"].empty());

		const size_t history = harness.Run("edit.history")["history"].size();
		harness.Run("scene.setSettings", { { "gravity", { 0, -9.81, 0 } } });
		CHECK(harness.Run("edit.history")["history"].size() == history);
	}

	TEST_CASE("The log is paged oldest first without gaps")
	{
		Harness harness;
		const uint64_t start = Log::GetBuffer().GetLatestSequence();
		for (int message = 0; message < 7; message++)
			ST_WARN("Paging test message {}", message);

		std::vector<std::string> seen;
		uint64_t after = start;
		bool more = true;
		while (more)
		{
			const nlohmann::json page = harness.Run("log.read", { { "after", after }, { "maxCount", 3 }, { "minLevel", "Warn" } });
			for (const nlohmann::json& message : page["messages"])
				seen.push_back(message["message"].get<std::string>());
			after = page["latest"].get<uint64_t>();
			more = page["more"].get<bool>();
		}
		std::vector<std::string> expected;
		for (int message = 0; message < 7; message++)
			expected.push_back(fmt::format("Paging test message {}", message));
		CHECK(seen == expected);
		CHECK_FALSE(harness.Error("log.read", { { "minLevel", "Loud" } }).empty());
	}

	TEST_CASE("Undo restores positions after duplicating, reparenting and failed instantiation")
	{
		Harness harness;
		const std::string a = harness.Create("A");
		const std::string b = harness.Create("B", { { "components", { { "Transform", { { "Translation", { 5, 0, 0 } } } } } } });
		const std::string c = harness.Create("C");
		const nlohmann::json original = harness.Snapshot();

		const std::string copy = harness.Run("entity.duplicate", { { "entity", a } })["id"].get<std::string>();
		const nlohmann::json duplicated = harness.Snapshot();
		harness.Run("edit.undo");
		CHECK(harness.Snapshot() == original);
		harness.Run("edit.redo");
		CHECK(harness.Snapshot() == duplicated); // The copy is back right after A

		// Moving B under A at index 0 keeps its world position; undo restores parent, order and transform.
		const std::string child = harness.Create("Child", { { "parent", a } });
		harness.Run("component.set", { { "entity", a }, { "component", "Transform" }, { "values", { { "Translation", { 1, 0, 0 } } } } });
		const nlohmann::json beforeMove = harness.Snapshot();
		harness.Run("entity.setParent", { { "entity", b }, { "parent", a }, { "index", 0 } });
		nlohmann::json moved = harness.Run("entity.get", { { "entity", a } });
		CHECK(moved["children"][0] == b);
		CHECK(harness.Run("component.get", { { "entity", b }, { "component", "Transform" } })["values"]["Translation"][0].get<double>() == doctest::Approx(4.0));
		harness.Run("edit.undo");
		CHECK(harness.Snapshot() == beforeMove);

		// A failed instantiation leaves nothing behind.
		CHECK_FALSE(harness.Error("prefab.instantiate", { { "prefab", "Builtin/Cube" } }).empty()); // Not a prefab
		harness.Run("scene.new");
		CHECK_FALSE(harness.Run("edit.history")["history"].size() > 0);
		CHECK(harness.Run("scene.info")["entityCount"] == 0);
		CHECK(copy != child);
		CHECK_FALSE(c.empty());
	}

	TEST_CASE("Property edits merge while dragging and stop merging at save points and play")
	{
		Harness harness;
		const std::string id = harness.Create("Lamp", { { "components", { { "PointLight", nlohmann::json::object() } } } });
		Entity entity = harness.Context.GetEditScene()->GetEntityByUUID(*UUIDFromJson(id));
		const ComponentInfo& light = *ComponentRegistry::Find("PointLight");
		const PropertyInfo& intensity = *light.FindProperty("Intensity");
		UndoStack& undo = harness.Context.GetUndoStack();
		const size_t base = undo.GetHistory().size();
		const float initial = entity.GetComponent<PointLightComponent>().Intensity;

		for (float value = 1.0f; value <= 4.0f; value += 1.0f)
			REQUIRE(SetPropertyWithUndo(harness.Context, entity, light, intensity, value));
		CHECK(undo.GetHistory().size() == base + 1); // One drag, one step
		undo.BreakMerge();
		REQUIRE(SetPropertyWithUndo(harness.Context, entity, light, intensity, 10.0f));
		CHECK(undo.GetHistory().size() == base + 2);
		harness.Context.GetUndoStack().MarkSaved();
		REQUIRE(SetPropertyWithUndo(harness.Context, entity, light, intensity, 11.0f));
		CHECK(undo.GetHistory().size() == base + 3); // Not merged across the save point

		std::string error;
		CHECK_FALSE(SetPropertyWithUndo(harness.Context, entity, light, intensity, std::string("bright"), &error));
		CHECK_FALSE(error.empty());
		CHECK(entity.GetComponent<PointLightComponent>().Intensity == 11.0f);

		harness.Context.Undo();
		CHECK(entity.GetComponent<PointLightComponent>().Intensity == 10.0f);
		harness.Context.Undo();
		CHECK(entity.GetComponent<PointLightComponent>().Intensity == 4.0f);
		harness.Context.Undo();
		CHECK(entity.GetComponent<PointLightComponent>().Intensity == initial);

		REQUIRE(harness.Context.Play());
		Entity running = harness.Context.GetActiveScene()->GetEntityByUUID(*UUIDFromJson(id));
		const size_t history = undo.GetHistory().size();
		REQUIRE(SetPropertyWithUndo(harness.Context, running, light, intensity, 99.0f));
		CHECK(undo.GetHistory().size() == history); // Not recorded while playing
		harness.Context.Stop();
	}

	TEST_CASE("Large hierarchies delete and restore exactly")
	{
		Harness harness;
		Scene& scene = *harness.Context.GetEditScene();
		Entity parent = scene.CreateEntity("Parent");
		for (int index = 0; index < 20000; index++)
			scene.CreateChildEntity(parent, fmt::format("Child {}", index));
		scene.CreateEntity("After");
		const nlohmann::json original = harness.Snapshot();

		harness.Run("entity.delete", { { "entities", { UUIDToJson(parent.GetUUID()) } } });
		CHECK(scene.GetEntityCount() == 1);
		harness.Run("edit.undo");
		CHECK(harness.Snapshot() == original);
		harness.Run("edit.redo");
		CHECK(scene.GetEntityCount() == 1);
		harness.Run("edit.undo");
		CHECK(harness.Snapshot() == original);
	}

	TEST_CASE("Asset commands import, configure, move and delete project files")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorAssetCommands");
		REQUIRE(FileSystem::WriteBytes(directory / "Brick.png", Tests::CreateSolidPNG(8, 8, 200, 100, 50)));
		Harness harness;
		CHECK_FALSE(harness.Error("asset.import", { { "file", FileSystem::ToUTF8(directory / "Brick.png") } }).empty()); // No project
		harness.Run("project.create", { { "directory", FileSystem::ToUTF8(directory / "Game") }, { "name", "Game" } });

		const nlohmann::json imported = harness.Run("asset.import", { { "file", FileSystem::ToUTF8(directory / "Brick.png") }, { "directory", "Textures" } });
		CHECK(imported["path"] == "Textures/Brick.png");
		CHECK(imported["importError"] == "");
		CHECK_FALSE(harness.Error("asset.import", { { "file", FileSystem::ToUTF8(directory / "Missing.png") } }).empty());

		const std::string texture = imported["asset"].get<std::string>();
		harness.Run("asset.setImportSettings", { { "asset", texture }, { "settings", { { "MaxSize", 4 } } } });
		CHECK(harness.Run("asset.info", { { "asset", texture } })["importSettings"]["MaxSize"] == 4);
		harness.Run("asset.reimport", { { "asset", "Textures/Brick.png" } });

		harness.Run("asset.move", { { "asset", texture }, { "path", "Textures/Wall.png" } });
		CHECK(harness.Run("asset.info", { { "asset", texture } })["path"] == "Textures/Wall.png");
		CHECK(harness.Run("asset.list", { { "type", "Texture" }, { "includeBuiltin", false } })["assets"].size() == 1);
		CHECK_FALSE(harness.Error("asset.list", { { "type", "Spaceship" } }).empty());
		harness.Run("asset.delete", { { "asset", texture } });
		CHECK_FALSE(harness.Error("asset.info", { { "asset", texture } }).empty());
		CHECK_FALSE(FileSystem::Exists(directory / "Game" / "Assets" / "Textures" / "Wall.png"));
	}
}
