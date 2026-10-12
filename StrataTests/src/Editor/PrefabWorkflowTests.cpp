#include <doctest/doctest.h>

#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/PrefabSync.h"
#include "TestHelpers.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/Base64.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/PrefabInstances.h>
#include <Strata/Scene/Scene.h>

#include <filesystem>
#include <span>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	struct PrefabHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		std::filesystem::path Directory;

		explicit PrefabHarness(const std::string& name)
		{
			Directory = Tests::CreateTemporaryDirectory(name) / "Game";
			Run("project.create", { { "directory", FileSystem::ToUTF8(Directory) }, { "name", "Game" } });
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			REQUIRE_FALSE(result.IsPending());
			return result.Value;
		}

		EditorCommandResult Fail(std::string_view name, const nlohmann::json& parameters)
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			CHECK_FALSE(result.Success);
			return result;
		}

		void Frames(int count)
		{
			for (int frame = 0; frame < count; frame++)
				Context.Update(Timestep(1.0f / 60.0f));
		}

		std::string Create(const std::string& name, const nlohmann::json& parent = nullptr, float intensity = 1.0f)
		{
			nlohmann::json parameters = { { "name", name }, { "components", { { "PointLight", { { "Intensity", intensity } } } } } };
			if (!parent.is_null())
				parameters["parent"] = parent;
			return Run("entity.create", parameters)["id"].get<std::string>();
		}

		nlohmann::json Get(const std::string& entity) { return Run("entity.get", { { "entity", entity } }); }
		double Intensity(const std::string& entity) { return Get(entity)["components"]["PointLight"]["Intensity"].get<double>(); }
		void SetIntensity(const std::string& entity, float value)
		{
			Run("component.set", { { "entity", entity }, { "component", "PointLight" }, { "values", { { "Intensity", value } } } });
		}
		bool IsLinked(const std::string& entity) { return Get(entity)["components"].contains("PrefabInstance"); }
		nlohmann::json Status(const std::string& entity) { return Run("prefab.status", { { "entity", entity } }); }
		std::vector<std::string> History() { return Run("edit.history")["history"].get<std::vector<std::string>>(); }

		// The prefab's file as stored: its entities by name.
		nlohmann::json ReadPrefabEntity(const std::string& path, const std::string& name)
		{
			const std::optional<std::string> text = FileSystem::ReadText(Directory / "Assets" / FileSystem::FromUTF8(path));
			REQUIRE(text);
			const std::optional<nlohmann::json> document = JsonUtils::Parse(*text);
			REQUIRE(document);
			for (const nlohmann::json& entity : (*document)["Prefab"]["Entities"])
			{
				if (entity["Components"]["Name"]["Name"] == name)
					return entity;
			}
			FAIL("The prefab has no entity named ", name);
			return nullptr;
		}

		std::string ChildOf(const std::string& entity, size_t index = 0)
		{
			const nlohmann::json children = Get(entity)["children"];
			REQUIRE(children.size() > index);
			return children[index].get<std::string>();
		}
	};

	// A lamp with a bulb below it, saved as Prefabs/Lamp.stprefab (linked) and instantiated twice.
	struct LampSetup
	{
		std::string Lamp, Bulb, Prefab, First, Second;

		explicit LampSetup(PrefabHarness& harness)
		{
			Lamp = harness.Create("Lamp", nullptr, 2.0f);
			Bulb = harness.Create("Bulb", Lamp, 3.0f);
			const nlohmann::json created = harness.Run("prefab.create", { { "entities", { Lamp } }, { "path", "Prefabs/Lamp.stprefab" } });
			Prefab = created["asset"].get<std::string>();
			CHECK(created["linked"] == true);
			First = harness.Run("prefab.instantiate", { { "prefab", Prefab }, { "components", { { "Transform", { { "Translation", { 4, 0, 0 } } } } } } })["id"].get<std::string>();
			Second = harness.Run("prefab.instantiate", { { "prefab", Prefab }, { "components", { { "Transform", { { "Translation", { -4, 0, 0 } } } } } } })["id"]
				.get<std::string>();
		}
	};

}

TEST_SUITE("Editor.Prefab")
{
	TEST_CASE("prefab.create links the entities in one undo step with the file, and undo unlinks them and trashes the file")
	{
		PrefabHarness harness("PrefabCreateLink");
		const std::string lamp = harness.Create("Lamp");
		const std::string bulb = harness.Create("Bulb", lamp);
		const std::vector<std::string> before = harness.History();

		const nlohmann::json created = harness.Run("prefab.create", { { "entities", { lamp } }, { "path", "Prefabs/Lamp.stprefab" } });
		CHECK(created["path"] == "Prefabs/Lamp.stprefab");
		CHECK(created["linked"] == true);
		const std::string prefab = created["asset"].get<std::string>();
		// Both entities are linked, with their own UUIDs as the prefab's ids, and the prefab stores no links.
		for (const std::string& entity : { lamp, bulb })
		{
			const nlohmann::json link = harness.Get(entity)["components"]["PrefabInstance"];
			CHECK(link["Prefab"] == prefab);
			CHECK(link["PrefabEntityID"] == entity);
		}
		CHECK_FALSE(harness.ReadPrefabEntity("Prefabs/Lamp.stprefab", "Lamp")["Components"].contains("PrefabInstance"));
		CHECK(harness.Status(bulb)["root"] == lamp);
		CHECK(harness.Status(lamp)["modified"] == false);
		std::vector<std::string> expected = before;
		expected.push_back("Create Prefab Prefabs/Lamp.stprefab");
		CHECK(harness.History() == expected);

		harness.Run("edit.undo");
		CHECK_FALSE(harness.IsLinked(lamp));
		CHECK_FALSE(harness.IsLinked(bulb));
		CHECK_FALSE(FileSystem::Exists(harness.Directory / "Assets" / "Prefabs" / "Lamp.stprefab"));
		harness.Run("edit.redo");
		CHECK(harness.IsLinked(lamp));
		CHECK(FileSystem::Exists(harness.Directory / "Assets" / "Prefabs" / "Lamp.stprefab"));
		CHECK(harness.Run("asset.info", { { "asset", "Prefabs/Lamp.stprefab" } })["asset"] == prefab); // The handle came back

		// link: false leaves the entities alone.
		const std::string plain = harness.Create("Plain");
		CHECK(harness.Run("prefab.create", { { "entities", { plain } }, { "path", "Prefabs/Plain.stprefab" }, { "link", false } })["linked"] == false);
		CHECK_FALSE(harness.IsLinked(plain));
	}

	TEST_CASE("prefab.instantiate returns the root's id, and the instance is linked and unmodified")
	{
		PrefabHarness harness("PrefabInstantiateId");
		LampSetup setup(harness);
		const nlohmann::json instantiated = harness.Run("prefab.instantiate", { { "prefab", setup.Prefab } });
		REQUIRE(instantiated["entities"].size() == 1);
		CHECK(instantiated["id"] == instantiated["entities"][0]);
		const std::string id = instantiated["id"].get<std::string>();
		const nlohmann::json status = harness.Status(id);
		CHECK(status["linked"] == true);
		CHECK(status["root"] == id);
		CHECK(status["prefab"] == setup.Prefab);
		CHECK(status["path"] == "Prefabs/Lamp.stprefab");
		CHECK(status["modified"] == false);
		CHECK(harness.Status(harness.ChildOf(id))["root"] == id);
		CHECK(harness.Run("prefab.status", { { "entity", harness.Create("Loose") } })["linked"] == false);
	}

	TEST_CASE("The root's transform and name are the instance's own: moving or renaming it does not modify it")
	{
		PrefabHarness harness("PrefabRootOwn");
		LampSetup setup(harness);
		harness.Run("component.set", { { "entity", setup.First }, { "component", "Transform" }, { "values", { { "Translation", { 9, 1, 2 } }, { "Scale", { 2, 2, 2 } } } } });
		harness.Run("entity.rename", { { "entity", setup.First }, { "name", "Desk Lamp" } });
		CHECK(harness.Status(setup.First)["modified"] == false);

		// A child's transform is the prefab's.
		const std::string bulb = harness.ChildOf(setup.First);
		harness.Run("component.set", { { "entity", bulb }, { "component", "Transform" }, { "values", { { "Translation", { 0, 2, 0 } } } } });
		const nlohmann::json status = harness.Status(setup.First);
		CHECK(status["modified"] == true);
		CHECK(status["changed"] == nlohmann::json::array({ bulb }));
		CHECK(status["added"].empty());
		CHECK(status["removed"].empty());
	}

	TEST_CASE("prefab.apply writes the instance into the prefab and updates unmodified instances, in one undo step")
	{
		PrefabHarness harness("PrefabApply");
		LampSetup setup(harness);
		// The second instance has a change of its own, so it keeps it.
		const std::string secondBulb = harness.ChildOf(setup.Second);
		harness.SetIntensity(secondBulb, 7.0f);
		// The first instance's bulb gets brighter, and the instance moves (which stays its own).
		const std::string firstBulb = harness.ChildOf(setup.First);
		harness.SetIntensity(firstBulb, 5.0f);
		harness.Run("component.set", { { "entity", setup.First }, { "component", "Transform" }, { "values", { { "Translation", { 1, 2, 3 } } } } });
		const std::vector<std::string> before = harness.History();
		const std::string originalBulb = setup.Bulb;

		const nlohmann::json applied = harness.Run("prefab.apply", { { "entity", firstBulb } });
		CHECK(applied["root"] == setup.First);
		CHECK(applied["path"] == "Prefabs/Lamp.stprefab");
		CHECK(applied["updatedInstances"] == 1); // The lamp the prefab was made from; the second instance was modified
		CHECK(harness.ReadPrefabEntity("Prefabs/Lamp.stprefab", "Bulb")["Components"]["PointLight"]["Intensity"] == 5.0);
		// The prefab keeps its own root transform.
		CHECK(harness.ReadPrefabEntity("Prefabs/Lamp.stprefab", "Lamp")["Components"]["Transform"]["Translation"] == nlohmann::json::array({ 0, 0, 0 }));
		CHECK(harness.Intensity(originalBulb) == 5.0); // Unmodified: updated, keeping its UUID
		CHECK(harness.Intensity(secondBulb) == 7.0);   // Modified: keeps its value
		CHECK(harness.Status(setup.First)["modified"] == false);
		CHECK(harness.Status(setup.Lamp)["modified"] == false);
		CHECK(harness.Status(setup.Second)["modified"] == true);
		std::vector<std::string> expected = before;
		expected.push_back("Apply Prefab Lamp");
		CHECK(harness.History() == expected);

		// Undo restores the file and the other instance; the applied instance keeps its edit, now a modification again.
		harness.Run("edit.undo");
		CHECK(harness.ReadPrefabEntity("Prefabs/Lamp.stprefab", "Bulb")["Components"]["PointLight"]["Intensity"] == 3.0);
		CHECK(harness.Intensity(originalBulb) == 3.0);
		CHECK(harness.Intensity(firstBulb) == 5.0);
		CHECK(harness.Status(setup.First)["modified"] == true);
		CHECK(harness.Status(setup.Lamp)["modified"] == false);
		// The sync does not take the undone file for a change to propagate.
		harness.Frames(3);
		CHECK(harness.Intensity(originalBulb) == 3.0);
		CHECK(harness.History() == expected);
		harness.Run("edit.redo");
		CHECK(harness.ReadPrefabEntity("Prefabs/Lamp.stprefab", "Bulb")["Components"]["PointLight"]["Intensity"] == 5.0);
		CHECK(harness.Intensity(originalBulb) == 5.0);
		harness.Frames(3);
		CHECK(harness.History() == expected);
	}

	TEST_CASE("prefab.apply links what was added to the instance, and the other instances get it")
	{
		PrefabHarness harness("PrefabApplyAdded");
		LampSetup setup(harness);
		const std::string shade = harness.Create("Shade", setup.First, 0.5f);
		const nlohmann::json status = harness.Status(setup.First);
		CHECK(status["added"] == nlohmann::json::array({ shade }));

		const nlohmann::json applied = harness.Run("prefab.apply", { { "entity", setup.First } });
		CHECK(applied["linked"] == 1);
		CHECK(applied["updatedInstances"] == 2);
		CHECK(harness.IsLinked(shade));
		CHECK(harness.ReadPrefabEntity("Prefabs/Lamp.stprefab", "Shade")["Components"]["PointLight"]["Intensity"] == 0.5);
		// The second instance has a shade of its own now.
		const nlohmann::json children = harness.Get(setup.Second)["children"];
		REQUIRE(children.size() == 2);
		CHECK(harness.Get(children[1].get<std::string>())["name"] == "Shade");
		CHECK(harness.Status(setup.Second)["modified"] == false);

		harness.Run("edit.undo");
		CHECK(harness.Get(setup.Second)["children"].size() == 1);
		CHECK_FALSE(harness.IsLinked(shade));
	}

	TEST_CASE("prefab.apply refuses while playing and for model instances; prefab.revert keeps UUIDs, the root's place, transform and name")
	{
		PrefabHarness harness("PrefabRevert");
		LampSetup setup(harness);
		const std::string bulb = harness.ChildOf(setup.Second);
		harness.SetIntensity(bulb, 9.0f);
		const std::string extra = harness.Create("Extra", setup.Second);
		harness.Run("component.set", { { "entity", setup.Second }, { "component", "Transform" }, { "values", { { "Translation", { 5, 6, 7 } } } } });
		harness.Run("entity.rename", { { "entity", setup.Second }, { "name", "Floor Lamp" } });
		const nlohmann::json modified = harness.Get(setup.Second);
		const std::vector<std::string> before = harness.History();

		const nlohmann::json reverted = harness.Run("prefab.revert", { { "entity", bulb } });
		CHECK(reverted["root"] == setup.Second);
		CHECK(reverted["created"].empty());
		CHECK(harness.Intensity(bulb) == 3.0); // Same UUID, the prefab's value
		CHECK(harness.Run("entity.find", { { "name", "Extra" } })["entities"].empty());
		const nlohmann::json root = harness.Get(setup.Second);
		CHECK(root["name"] == "Floor Lamp");
		CHECK(root["components"]["Transform"] == modified["components"]["Transform"]);
		CHECK(root["children"] == nlohmann::json::array({ bulb }));
		CHECK(harness.Status(setup.Second)["modified"] == false);
		CHECK(harness.History().size() == before.size() + 1);

		harness.Run("edit.undo");
		CHECK(harness.Intensity(bulb) == 9.0);
		CHECK(harness.Get(extra)["parent"] == setup.Second);
		harness.Run("edit.redo");

		// A removed entity comes back with a new UUID.
		harness.Run("entity.delete", { { "entities", { bulb } } });
		CHECK(harness.Status(setup.Second)["removed"].size() == 1);
		const nlohmann::json restored = harness.Run("prefab.revert", { { "entity", setup.Second } });
		REQUIRE(restored["created"].size() == 1);
		CHECK(harness.Intensity(restored["created"][0].get<std::string>()) == 3.0);

		// Not while playing, and only for entities of instances.
		harness.Run("play.start");
		CHECK(harness.Fail("prefab.apply", { { "entity", setup.Second } }).Error.find("Stop playing") != std::string::npos);
		harness.Run("play.stop");
		CHECK(harness.Fail("prefab.apply", { { "entity", harness.Create("Loose") } }).ErrorKind == EditorCommandError::InvalidParameters);
	}

	TEST_CASE("Instances of imported models revert to the model and cannot be applied to it")
	{
		PrefabHarness harness("PrefabModel");
		// A one-triangle model with a node named Tri.
		const std::vector<float> positions = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
		const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(positions.data()), positions.size() * sizeof(float));
		const nlohmann::json gltf = {
			{ "asset", { { "version", "2.0" } } },
			{ "scene", 0 },
			{ "scenes", { { { "nodes", { 0 } } } } },
			{ "nodes", { { { "name", "Tri" }, { "mesh", 0 } } } },
			{ "meshes", { { { "name", "Tri" }, { "primitives", { { { "attributes", { { "POSITION", 0 } } } } } } } } },
			{ "accessors", { { { "bufferView", 0 }, { "componentType", 5126 }, { "count", 3 }, { "type", "VEC3" }, { "min", { 0, 0, 0 } }, { "max", { 1, 1, 0 } } } } },
			{ "bufferViews", { { { "buffer", 0 }, { "byteLength", bytes.size() } } } },
			{ "buffers", { { { "byteLength", bytes.size() }, { "uri", "data:application/octet-stream;base64," + Base64::Encode(bytes) } } } } };
		REQUIRE(FileSystem::CreateDirectories(harness.Directory / "Assets" / "Models"));
		REQUIRE(FileSystem::WriteText(harness.Directory / "Assets" / "Models" / "Tri.gltf", gltf.dump()));
		harness.Context.GetAssetManager()->Scan();

		const std::string instance = harness.Run("prefab.instantiate", { { "prefab", "Models/Tri.gltf" } })["id"].get<std::string>();
		CHECK(harness.Status(instance)["modified"] == false);
		const EditorCommandResult refused = harness.Fail("prefab.apply", { { "entity", instance } });
		CHECK(refused.ErrorKind == EditorCommandError::Failed);
		CHECK(refused.Error.find("imported model") != std::string::npos);

		// The node's entity (the instance's root, or its child when the model adds a root of its own).
		const std::string mesh = harness.Get(instance)["components"].contains("MeshRenderer") ? instance : harness.ChildOf(instance);
		harness.Run("component.set", { { "entity", mesh }, { "component", "MeshRenderer" }, { "values", { { "CastShadows", false } } } });
		CHECK(harness.Status(instance)["modified"] == true);
		harness.Run("prefab.revert", { { "entity", mesh } });
		CHECK(harness.Status(instance)["modified"] == false);
		CHECK(harness.Get(mesh)["components"]["MeshRenderer"]["CastShadows"] == true);
	}

	TEST_CASE("prefab.unpack ends the link in one undo step")
	{
		PrefabHarness harness("PrefabUnpack");
		LampSetup setup(harness);
		const std::string bulb = harness.ChildOf(setup.First);
		const nlohmann::json unpacked = harness.Run("prefab.unpack", { { "entity", bulb } });
		CHECK(unpacked["root"] == setup.First);
		CHECK(unpacked["entities"] == 2);
		CHECK_FALSE(harness.IsLinked(setup.First));
		CHECK_FALSE(harness.IsLinked(bulb));
		CHECK(harness.Status(setup.First)["linked"] == false);
		harness.Run("edit.undo");
		CHECK(harness.IsLinked(setup.First));
		CHECK(harness.IsLinked(bulb));

		// prefab.select leads to the root, or to the prefab.
		CHECK(harness.Run("prefab.select", { { "entity", bulb } })["root"] == setup.First);
		CHECK(harness.Context.GetSelection() == std::vector<UUID> { *UUIDFromJson(setup.First) });
		harness.Run("prefab.select", { { "entity", bulb }, { "asset", true } });
		CHECK(UUIDToJson(harness.Context.GetInspectedAsset()) == setup.Prefab);

		// While playing, unpacking and reverting change the running copy, and their answers say so.
		harness.Run("play.start");
		const nlohmann::json running = harness.Run("prefab.unpack", { { "entity", setup.Second } });
		CHECK(running["warning"].get<std::string>().find("running copy") != std::string::npos);
		CHECK(harness.Run("prefab.revert", { { "entity", setup.First } })["warning"].get<std::string>().find("running copy") != std::string::npos);
		harness.Run("play.stop");
		CHECK(harness.IsLinked(setup.Second));
		CHECK_FALSE(harness.Run("prefab.unpack", { { "entity", setup.Second } }).contains("warning"));
	}

	TEST_CASE("When a prefab's file changes, unmodified instances take its values and modified ones keep theirs")
	{
		PrefabHarness harness("PrefabPropagate");
		LampSetup setup(harness);
		const std::string modifiedBulb = harness.ChildOf(setup.Second);
		harness.SetIntensity(modifiedBulb, 8.0f);
		harness.Frames(2); // The sync reads the prefab it compares with
		const uint64_t propagated = harness.Context.GetPrefabSync().GetPropagationCount();
		const std::vector<std::string> before = harness.History();

		// The file changes outside the editor (another tool, version control): the bulb gets brighter.
		const std::filesystem::path file = harness.Directory / "Assets" / "Prefabs" / "Lamp.stprefab";
		std::optional<std::string> text = FileSystem::ReadText(file);
		REQUIRE(text);
		std::optional<nlohmann::json> document = JsonUtils::Parse(*text);
		REQUIRE(document);
		for (nlohmann::json& entity : (*document)["Prefab"]["Entities"])
		{
			if (entity["Components"]["Name"]["Name"] == "Bulb")
				entity["Components"]["PointLight"]["Intensity"] = 6.0;
		}
		REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(*document, 1, '\t') + "\n"));
		const AssetHandle prefab = *UUIDFromJson(setup.Prefab);
		REQUIRE(harness.Context.GetAssetManager()->ReimportAsset(prefab));
		harness.Frames(2);

		CHECK(harness.Intensity(harness.ChildOf(setup.First)) == 6.0);
		CHECK(harness.Intensity(setup.Bulb) == 6.0);
		CHECK(harness.Intensity(modifiedBulb) == 8.0);
		CHECK(harness.Context.GetPrefabSync().GetPropagationCount() == propagated + 2);
		std::vector<std::string> expected = before;
		expected.push_back("Update Instances of Lamp");
		CHECK(harness.History() == expected);
		// The roots kept their places.
		CHECK(harness.Get(setup.First)["components"]["Transform"]["Translation"] == nlohmann::json::array({ 4, 0, 0 }));

		// The update is undoable like any edit; the prefab stays as it is on disk.
		harness.Run("edit.undo");
		CHECK(harness.Intensity(setup.Bulb) == 3.0);
		CHECK(harness.Status(setup.Lamp)["modified"] == true);
		harness.Frames(2);
		CHECK(harness.Intensity(setup.Bulb) == 3.0);
	}

	TEST_CASE("A prefab applied in another scene reaches unmodified instances when their scene opens; modified ones are outdated")
	{
		PrefabHarness harness("PrefabClosedScene");
		const std::string lamp = harness.Create("Lamp", nullptr, 2.0f);
		const std::string prefab = harness.Run("prefab.create", { { "entities", { lamp } }, { "path", "Prefabs/Lamp.stprefab" } })["asset"].get<std::string>();
		harness.Run("scene.saveAs", { { "path", "Scenes/A.stscene" } });
		harness.Run("scene.new");
		const std::string plain = harness.Run("prefab.instantiate", { { "prefab", prefab } })["id"].get<std::string>();
		const std::string edited = harness.Run("prefab.instantiate", { { "prefab", prefab } })["id"].get<std::string>();
		harness.SetIntensity(edited, 4.0f);
		CHECK(harness.Status(plain)["outdated"] == false);
		CHECK_FALSE(harness.Get(plain)["components"]["PrefabInstance"]["PrefabVersion"].get<std::string>().empty());
		harness.Run("scene.saveAs", { { "path", "Scenes/B.stscene" } });

		// Changed and applied in scene A, while B is closed.
		harness.Run("scene.open", { { "scene", "Scenes/A.stscene" } });
		harness.Frames(2);
		harness.SetIntensity(lamp, 9.0f);
		CHECK(harness.Run("prefab.apply", { { "entity", lamp } })["updatedInstances"] == 0);
		harness.Run("scene.save");

		// B opens: its unmodified instance takes the change in one undo step, the modified one keeps its values and is outdated.
		harness.Run("scene.open", { { "scene", "Scenes/B.stscene" } });
		const uint64_t checks = harness.Context.GetPrefabSync().GetCatchUpCheckCount();
		harness.Frames(2);
		CHECK(harness.Context.GetPrefabSync().GetCatchUpCheckCount() == checks + 2);
		CHECK(harness.Intensity(plain) == 9.0);
		CHECK(harness.Status(plain)["modified"] == false);
		CHECK(harness.Status(plain)["outdated"] == false);
		CHECK(harness.Intensity(edited) == 4.0);
		CHECK(harness.Status(edited)["modified"] == true);
		CHECK(harness.Status(edited)["outdated"] == true);
		CHECK(harness.History() == std::vector<std::string> { "Update Instances of Lamp" });
		CHECK(harness.Run("scene.info")["modified"] == true);
		harness.Frames(4); // Once per scene: nothing more happens
		CHECK(harness.History().size() == 1);

		// The outdated instance would write its older values over the prefab's: refused, unless forced.
		const EditorCommandResult refused = harness.Fail("prefab.apply", { { "entity", edited } });
		CHECK(refused.Error.find("older version") != std::string::npos);
		CHECK(harness.ReadPrefabEntity("Prefabs/Lamp.stprefab", "Lamp")["Components"]["PointLight"]["Intensity"] == 9.0);
		// Reverting brings it up to date.
		harness.Run("prefab.revert", { { "entity", edited } });
		CHECK(harness.Intensity(edited) == 9.0);
		CHECK(harness.Status(edited)["outdated"] == false);

		// The catch-up is an undo step like any edit.
		harness.Run("edit.undo");
		harness.Run("edit.undo");
		CHECK(harness.Intensity(plain) == 2.0);
		CHECK(harness.Status(plain)["outdated"] == true);
	}

	TEST_CASE("A prefab changed on disk while its scene was closed reaches the scene's unmodified instances; unknown ones keep theirs")
	{
		PrefabHarness harness("PrefabChangedOnDisk");
		const std::string lamp = harness.Create("Lamp", nullptr, 2.0f);
		const std::string prefab = harness.Run("prefab.create", { { "entities", { lamp } }, { "path", "Prefabs/Lamp.stprefab" } })["asset"].get<std::string>();
		const std::string old = harness.Run("prefab.instantiate", { { "prefab", prefab } })["id"].get<std::string>();
		// An instance from before instances recorded what they took (a scene of an older editor): nothing known about it.
		Scene& scene = *harness.Context.GetEditScene();
		PrefabInstanceComponent& link = scene.GetEntityByUUID(*UUIDFromJson(old)).GetComponent<PrefabInstanceComponent>();
		link.PrefabVersion = 0;
		link.SyncedHash = 0;
		harness.Run("scene.saveAs", { { "path", "Scenes/A.stscene" } });
		harness.Run("scene.new");

		// Version control brings a brighter lamp.
		const std::filesystem::path file = harness.Directory / "Assets" / "Prefabs" / "Lamp.stprefab";
		std::optional<std::string> text = FileSystem::ReadText(file);
		REQUIRE(text);
		std::optional<nlohmann::json> document = JsonUtils::Parse(*text);
		REQUIRE(document);
		(*document)["Prefab"]["Entities"][0]["Components"]["PointLight"]["Intensity"] = 7.0;
		REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(*document, 1, '\t') + "\n"));
		REQUIRE(harness.Context.GetAssetManager()->ReimportAsset(*UUIDFromJson(prefab)));

		harness.Run("scene.open", { { "scene", "Scenes/A.stscene" } });
		harness.Frames(2);
		CHECK(harness.Intensity(lamp) == 7.0);
		CHECK(harness.Intensity(old) == 2.0);
		CHECK(harness.Status(old)["outdated"] == false); // Not known
		CHECK(harness.Status(old)["modified"] == true);
	}

	TEST_CASE("Paths made from entity names are valid, free file names")
	{
		PrefabHarness harness("PrefabPaths");
		EditorAssetManager& assets = *harness.Context.GetAssetManager();
		CHECK(CommandUtils::MakeUniqueAssetPath(assets, "Prefabs", "Lamp", ".stprefab", "Prefab") == "Prefabs/Lamp.stprefab");
		CHECK(CommandUtils::MakeUniqueAssetPath(assets, "", "Lamp", ".stprefab", "Prefab") == "Lamp.stprefab");
		CHECK(CommandUtils::MakeUniqueAssetPath(assets, "Prefabs", " a/b:c*d? ", ".stprefab", "Prefab") == "Prefabs/a_b_c_d_.stprefab");
		CHECK(CommandUtils::MakeUniqueAssetPath(assets, "Prefabs", "Dots...", ".stprefab", "Prefab") == "Prefabs/Dots.stprefab");
		CHECK(CommandUtils::MakeUniqueAssetPath(assets, "Prefabs", "", ".stprefab", "Prefab") == "Prefabs/Prefab.stprefab");
		CHECK(CommandUtils::MakeUniqueAssetPath(assets, "Prefabs", "CON", ".stprefab", "Prefab") == "Prefabs/Prefab.stprefab");
		// Long names are cut at a character boundary (each "é" is two bytes).
		std::string longName;
		for (int index = 0; index < 60; index++)
			longName += "\xC3\xA9";
		const std::string cut = CommandUtils::MakeUniqueAssetPath(assets, "", longName, ".stprefab", "Prefab");
		CHECK(cut == longName.substr(0, 96) + ".stprefab");
		// Taken names get a number.
		const std::string lamp = harness.Create("Lamp");
		harness.Run("prefab.create", { { "entities", { lamp } }, { "path", "Prefabs/Lamp.stprefab" } });
		CHECK(CommandUtils::MakeUniqueAssetPath(assets, "Prefabs", "Lamp", ".stprefab", "Prefab") == "Prefabs/Lamp (1).stprefab");
	}

	TEST_CASE("The modified mark follows edits and prefab changes")
	{
		PrefabHarness harness("PrefabModifiedMark");
		LampSetup setup(harness);
		harness.Frames(1);
		PrefabSync& sync = harness.Context.GetPrefabSync();
		Scene& scene = *harness.Context.GetEditScene();
		const Entity first = scene.GetEntityByUUID(*UUIDFromJson(setup.First));
		CHECK_FALSE(sync.IsModified(harness.Context, first));
		harness.SetIntensity(harness.ChildOf(setup.First), 4.0f);
		CHECK(sync.IsModified(harness.Context, first));
		harness.Run("edit.undo");
		CHECK_FALSE(sync.IsModified(harness.Context, first));
		// Not instance roots: never marked.
		CHECK_FALSE(sync.IsModified(harness.Context, scene.GetEntityByUUID(*UUIDFromJson(harness.ChildOf(setup.First)))));
	}

	TEST_CASE("A run of edits, one per frame, compares instances and walks the links once it pauses, not every frame")
	{
		PrefabHarness harness("PrefabEditRun");
		LampSetup setup(harness);
		harness.Frames(static_cast<int>(PrefabSync::c_SettleFrames) + 2);
		PrefabSync& sync = harness.Context.GetPrefabSync();
		Scene& scene = *harness.Context.GetEditScene();
		const Entity first = scene.GetEntityByUUID(*UUIDFromJson(setup.First));
		const Entity second = scene.GetEntityByUUID(*UUIDFromJson(setup.Second));
		CHECK_FALSE(sync.IsModified(harness.Context, first));
		CHECK_FALSE(sync.IsModified(harness.Context, second));
		const uint64_t compares = sync.GetCompareCount();
		const uint64_t scans = sync.GetWatchScanCount();

		// A drag: an edit in every frame, with the Hierarchy asking for both marks.
		const std::string bulb = harness.ChildOf(setup.First);
		for (int frame = 0; frame < 30; frame++)
		{
			harness.SetIntensity(bulb, 4.0f + static_cast<float>(frame));
			harness.Frames(1);
			sync.IsModified(harness.Context, first);
			sync.IsModified(harness.Context, second);
		}
		// The first edit after the pause is looked at at once (each instance compared, the links walked); the rest of the run
		// is not.
		CHECK(sync.GetCompareCount() - compares == 2);
		CHECK(sync.GetWatchScanCount() - scans == 1);

		// Once the edits pause, the marks follow them.
		harness.Frames(static_cast<int>(PrefabSync::c_SettleFrames));
		CHECK(sync.IsModified(harness.Context, first));
		CHECK_FALSE(sync.IsModified(harness.Context, second));
		CHECK(sync.GetCompareCount() - compares == 4);
		CHECK(sync.GetWatchScanCount() - scans == 2);
		// Links that come or go are found at once, also during a run.
		harness.Run("prefab.unpack", { { "entity", setup.Second } });
		harness.Frames(1);
		CHECK(sync.GetWatchScanCount() - scans == 3);
	}
}
