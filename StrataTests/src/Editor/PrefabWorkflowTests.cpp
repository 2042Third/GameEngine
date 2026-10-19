#include <doctest/doctest.h>

#include "Animation/SkinnedGltf.h"
#include "Editor/CommandJournal.h"
#include "Editor/CommandTestUtils.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/EditorPlacement.h"
#include "Editor/PrefabSync.h"
#include "Editor/PropertyEdit.h"
#include "Editor/SceneBounds.h"
#include "Editor/TransformEdit.h"
#include "TestHelpers.h"

#include <Strata/Animation/AnimationComponents.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/Base64.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/PrefabInstances.h>
#include <Strata/Scene/Scene.h>

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
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

		// Runs a command; one that answers in a later frame (scene.open loads in the background) is completed over editor
		// frames, as the runner completes it.
		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Tests::RunCommandToCompletion(Context, Commands, name, parameters);
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

	// The skinned arm (2 units tall at Scale 1) as Models/Arm.gltf of the harness's project, imported with `scale`.
	AssetHandle AddArmModel(PrefabHarness& harness, float scale)
	{
		REQUIRE(FileSystem::CreateDirectories(harness.Directory / "Assets" / "Models"));
		REQUIRE(FileSystem::WriteBytes(harness.Directory / "Assets" / "Models" / "Arm.gltf", Tests::CreateSkinnedArmGltf()));
		harness.Context.GetAssetManager()->Scan();
		const AssetHandle model = harness.Context.GetAssetManager()->FindAssetByPath("Models/Arm.gltf");
		REQUIRE(model.IsValid());
		harness.Run("asset.setImportSettings", { { "asset", UUIDToJson(model) }, { "settings", { { "Scale", scale } } } });
		return model;
	}

	// Editor frames until nothing loads, imports or waits for a rest pose (by the clock: CI's machines are slow).
	void SettleFrames(PrefabHarness& harness)
	{
		EditorAssetManager& assets = *harness.Context.GetAssetManager();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		do
			harness.Frames(1);
		while ((assets.HasPendingLoads() || assets.HasPendingImports() || harness.Context.GetAnimationPreview().HasWaitingRestPoses())
			&& std::chrono::steady_clock::now() < deadline);
		harness.Frames(PrefabSync::c_SettleFrames + 1);
	}

	// An entity's bounds once the meshes they need are in (measuring requests them).
	AABB GetBounds(PrefabHarness& harness, const std::string& entity)
	{
		Scene& scene = *harness.Context.GetEditScene();
		const Entity measured = scene.GetEntityByUUID(*UUIDFromJson(nlohmann::json(entity)));
		SceneBounds::GetEntityBounds(scene, measured);
		SettleFrames(harness);
		return SceneBounds::GetEntityBounds(scene, measured);
	}

	void CheckSameBounds(const AABB& a, const AABB& b)
	{
		for (int axis = 0; axis < 3; axis++)
		{
			CAPTURE(axis);
			CHECK(a.Min[axis] == doctest::Approx(b.Min[axis]).epsilon(1e-4));
			CHECK(a.Max[axis] == doctest::Approx(b.Max[axis]).epsilon(1e-4));
		}
	}

}

TEST_SUITE("Editor.Prefab")
{
	TEST_CASE("A model's import scale reaches its placed instances, whose root transforms stay theirs")
	{
		PrefabHarness harness("PrefabImportScale");
		const AssetHandle model = AddArmModel(harness, 0.01f);
		const nlohmann::json placement = { { "Transform", { { "Translation", { 3, 0, 1 } }, { "Rotation", { 0, 30, 0 } } } } };
		const std::string instance = harness.Run("prefab.instantiate", { { "prefab", UUIDToJson(model) }, { "components", placement } })["id"].get<std::string>();
		SettleFrames(harness);
		const nlohmann::json transform = harness.Get(instance)["components"]["Transform"];
		CHECK(transform["Scale"] == nlohmann::json::array({ 1, 1, 1 }));
		const AABB before = GetBounds(harness, instance);
		REQUIRE(before.IsValid());
		CHECK(before.GetExtents().y * 2.0f == doctest::Approx(0.02f).epsilon(1e-3));

		harness.Run("asset.setImportSettings", { { "asset", UUIDToJson(model) }, { "settings", { { "Scale", 0.02 } } } });
		SettleFrames(harness);
		const AABB after = GetBounds(harness, instance);
		// Twice the size around the instance's origin, which stayed where it was placed.
		const glm::vec3 origin(3.0f, 0.0f, 1.0f);
		for (int axis = 0; axis < 3; axis++)
		{
			CAPTURE(axis);
			CHECK(after.Min[axis] - origin[axis] == doctest::Approx((before.Min[axis] - origin[axis]) * 2.0f).epsilon(1e-4));
			CHECK(after.Max[axis] - origin[axis] == doctest::Approx((before.Max[axis] - origin[axis]) * 2.0f).epsilon(1e-4));
		}
		CHECK(harness.Get(instance)["components"]["Transform"] == transform);
		CHECK(harness.Status(instance)["modified"] == false);
	}

	TEST_CASE("Instances placed before the import scale was baked keep their size through a prefab change and a scene open")
	{
		PrefabHarness harness("PrefabImportScaleMigration");
		const AssetHandle model = AddArmModel(harness, 0.01f);
		const nlohmann::json placement = { { "Transform", { { "Translation", { 2, 0, 0 } } } } };
		const std::string reference = harness.Run("prefab.instantiate", { { "prefab", UUIDToJson(model) }, { "components", placement } })["id"].get<std::string>();
		const std::string old = harness.Run("prefab.instantiate", { { "prefab", UUIDToJson(model) }, { "components", placement } })["id"].get<std::string>();
		SettleFrames(harness);
		const AABB expected = GetBounds(harness, reference);
		REQUIRE(expected.IsValid());

		// The previous importer put the import scale on the model's root, and instances took it: a root Scale of 0.01.
		const nlohmann::json* current = harness.Context.GetPrefabSync().GetBaseline(harness.Context, model);
		REQUIRE(current);
		nlohmann::json previous = *current;
		const UUID templateRoot = harness.Context.GetEditScene()->GetEntityByUUID(*UUIDFromJson(nlohmann::json(old))).GetComponent<PrefabInstanceComponent>().PrefabEntityID;
		bool found = false;
		for (nlohmann::json& entity : previous["Entities"])
		{
			if (UUIDFromJson(entity["ID"]) == std::optional<UUID>(templateRoot))
			{
				entity["Components"]["Transform"]["Scale"] = { 0.01, 0.01, 0.01 };
				found = true;
			}
		}
		REQUIRE(found);
		harness.Run("component.set", { { "entity", old }, { "component", "Transform" }, { "values", { { "Scale", { 0.01, 0.01, 0.01 } } } } });

		SUBCASE("Propagate: the model's document changes while the scene is open")
		{
			harness.Context.GetPrefabSync().SetBaseline(harness.Context, model, previous);
			const size_t steps = harness.History().size();
			REQUIRE(harness.Context.GetAssetManager()->ReimportAsset(model));
			SettleFrames(harness);
			CHECK(harness.Get(old)["components"]["Transform"]["Scale"] == nlohmann::json::array({ 1, 1, 1 }));
			CheckSameBounds(GetBounds(harness, old), expected);
			// One step: the update of the instances.
			REQUIRE(harness.History().size() == steps + 1);
			CHECK(harness.History().back() == "Update Instances of Arm");
		}
		SUBCASE("CatchUp: the scene opens after the model changed")
		{
			// Saved with a part version of the old document, so the instance is outdated once the scene opens.
			harness.Run("scene.saveAs", { { "path", "Scenes/Old.stscene" } });
			const std::filesystem::path file = harness.Directory / "Assets" / "Scenes" / "Old.stscene";
			std::optional<std::string> text = FileSystem::ReadText(file);
			REQUIRE(text);
			std::optional<nlohmann::json> document = JsonUtils::Parse(*text);
			REQUIRE(document);
			for (nlohmann::json& entity : (*document)["Scene"]["Entities"])
			{
				if (entity["ID"] == old)
				{
					std::string version = entity["Components"]["PrefabInstance"]["PrefabVersion"].get<std::string>();
					REQUIRE_FALSE(version.empty());
					version.back() = version.back() == '1' ? '2' : '1';
					entity["Components"]["PrefabInstance"]["PrefabVersion"] = version;
				}
			}
			REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(*document, 1, '\t') + "\n"));
			harness.Run("scene.new", { { "force", true } });
			harness.Run("scene.open", { { "scene", "Scenes/Old.stscene" }, { "force", true } });
			SettleFrames(harness);
			CHECK(harness.Get(old)["components"]["Transform"]["Scale"] == nlohmann::json::array({ 1, 1, 1 }));
			CheckSameBounds(GetBounds(harness, old), expected);
			CHECK(harness.Get(reference)["components"]["Transform"]["Scale"] == nlohmann::json::array({ 1, 1, 1 }));
		}
	}

	TEST_CASE("A model's placement bounds never wait for its skeleton, and stand it in its rest pose once it is resident")
	{
		PrefabHarness harness("PrefabPlacementBounds");
		const AssetHandle model = AddArmModel(harness, 1.0f);
		SettleFrames(harness);
		EditorAssetManager& assets = *harness.Context.GetAssetManager();
		// The model's skeleton, from its template's animator.
		const Ref<EntityTemplate> asset = std::static_pointer_cast<EntityTemplate>(assets.LoadAssetSync(model));
		REQUIRE(asset);
		Scene scratch;
		REQUIRE_FALSE(asset->Instantiate(scratch).empty());
		AssetHandle skeleton = UUID::Null();
		for (const auto [handle, animator] : scratch.GetRegistry().view<AnimatorComponent>().each())
			skeleton = animator.Skeleton;
		REQUIRE(skeleton.IsValid());
		REQUIRE(assets.GetAssetState(skeleton) != AssetState::Ready);

		// Measured before the skeleton is resident: it is requested, not waited for.
		EditorPlacement::GetTemplateBounds(model);
		CHECK(assets.GetAssetState(skeleton) != AssetState::Ready);
		// Once it is, the placement bounds are the arm standing 2 units tall from its root, as the template bounds are then.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!harness.Context.GetAssetBounds().Measure(assets, model).Bounds.IsValid() || harness.Context.GetAssetBounds().Measure(assets, model).Pending)
		{
			REQUIRE(std::chrono::steady_clock::now() < deadline);
			harness.Frames(1);
		}
		const AABB placed = EditorPlacement::GetPlacementBounds(harness.Context, model);
		REQUIRE(placed.IsValid());
		CHECK(placed.Max.y - placed.Min.y == doctest::Approx(2.0f).epsilon(1e-3));
		CheckSameBounds(placed, EditorPlacement::GetTemplateBounds(model));
	}

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

	TEST_CASE("When a prefab's file changes, every instance takes its values except what it overrides")
	{
		PrefabHarness harness("PrefabPropagate");
		LampSetup setup(harness);
		const std::string modifiedBulb = harness.ChildOf(setup.Second);
		harness.SetIntensity(modifiedBulb, 8.0f);
		harness.Frames(2); // The sync reads the prefab it compares with
		const uint64_t propagated = harness.Context.GetPrefabSync().GetPropagationCount();
		const std::vector<std::string> before = harness.History();
		const EditorContext::OverrideUpkeepStats upkeep = harness.Context.GetOverrideUpkeepStats();

		// The file changes outside the editor (another tool, version control): the bulb gets brighter.
		const std::filesystem::path file = harness.Directory / "Assets" / "Prefabs" / "Lamp.stprefab";
		std::optional<std::string> text = FileSystem::ReadText(file);
		REQUIRE(text);
		std::optional<nlohmann::json> document = JsonUtils::Parse(*text);
		REQUIRE(document);
		for (nlohmann::json& entity : (*document)["Prefab"]["Entities"])
		{
			if (entity["Components"]["Name"]["Name"] == "Bulb")
			{
				entity["Components"]["PointLight"]["Intensity"] = 6.0;
				entity["Components"]["PointLight"]["Range"] = 4.0;
			}
		}
		REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(*document, 1, '\t') + "\n"));
		const AssetHandle prefab = *UUIDFromJson(setup.Prefab);
		REQUIRE(harness.Context.GetAssetManager()->ReimportAsset(prefab));
		harness.Frames(2);

		CHECK(harness.Intensity(harness.ChildOf(setup.First)) == 6.0);
		CHECK(harness.Intensity(setup.Bulb) == 6.0);
		// The modified instance keeps its intensity (an override) and takes the range.
		CHECK(harness.Intensity(modifiedBulb) == 8.0);
		CHECK(harness.Get(modifiedBulb)["components"]["PointLight"]["Range"] == 4.0);
		CHECK(harness.Status(setup.Second)["overrides"].size() == 1);
		// Propagate left every list current: recording the step works none out again.
		CHECK(harness.Context.GetOverrideUpkeepStats().WholeLists == upkeep.WholeLists);
		CHECK(harness.Context.GetOverrideUpkeepStats().Entities == upkeep.Entities);
		CHECK(harness.Context.GetPrefabSync().GetPropagationCount() == propagated + 3);
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

	TEST_CASE("A prefab applied in another scene reaches its instances when their scene opens, except their overrides; ones without a list are outdated")
	{
		PrefabHarness harness("PrefabClosedScene");
		const std::string lamp = harness.Create("Lamp", nullptr, 2.0f);
		const std::string prefab = harness.Run("prefab.create", { { "entities", { lamp } }, { "path", "Prefabs/Lamp.stprefab" } })["asset"].get<std::string>();
		harness.Run("scene.saveAs", { { "path", "Scenes/A.stscene" } });
		harness.Run("scene.new");
		const std::string plain = harness.Run("prefab.instantiate", { { "prefab", prefab } })["id"].get<std::string>();
		const std::string edited = harness.Run("prefab.instantiate", { { "prefab", prefab } })["id"].get<std::string>();
		const std::string legacy = harness.Run("prefab.instantiate", { { "prefab", prefab } })["id"].get<std::string>();
		harness.SetIntensity(edited, 4.0f);
		harness.SetIntensity(legacy, 5.0f);
		CHECK(harness.Status(plain)["outdated"] == false);
		CHECK_FALSE(harness.Get(plain)["components"]["PrefabInstance"]["PrefabVersion"].get<std::string>().empty());
		harness.Run("scene.saveAs", { { "path", "Scenes/B.stscene" } });
		// The legacy instance as a scene from before override lists stores it: without its list.
		{
			const std::filesystem::path file = harness.Directory / "Assets" / "Scenes" / "B.stscene";
			std::optional<nlohmann::json> document = JsonUtils::Parse(*FileSystem::ReadText(file));
			REQUIRE(document);
			for (nlohmann::json& entity : (*document)["Scene"]["Entities"])
			{
				if (entity["ID"] == legacy)
					entity["Components"]["PrefabInstance"].erase("Overrides");
			}
			REQUIRE(FileSystem::WriteText(file, JsonUtils::Dump(*document, 1, '\t') + "\n"));
		}

		// Changed (the color and the intensity) and applied in scene A, while B is closed.
		harness.Run("scene.open", { { "scene", "Scenes/A.stscene" } });
		harness.Frames(2);
		harness.SetIntensity(lamp, 9.0f);
		harness.Run("component.set", { { "entity", lamp }, { "component", "PointLight" }, { "values", { { "Color", { 0, 1, 0 } } } } });
		CHECK(harness.Run("prefab.apply", { { "entity", lamp } })["updatedInstances"] == 0);
		harness.Run("scene.save");

		// B opens: by the time scene.open answers, in one undo step, its unmodified instance took the change, the one with an
		// override took all but it, and the one without a list keeps its values and is outdated.
		const uint64_t checks = harness.Context.GetPrefabSync().GetCatchUpCheckCount();
		const uint64_t outsideCommands = harness.Context.GetJournal().GetEditsOutsideCommands();
		harness.Run("scene.open", { { "scene", "Scenes/B.stscene" } });
		const uint64_t openEntry = harness.Context.GetJournal().GetLatestId();
		CHECK(harness.Context.GetPrefabSync().GetCatchUpCheckCount() == checks + 3);
		harness.Frames(2);
		CHECK(harness.Context.GetPrefabSync().GetCatchUpCheckCount() == checks + 3);
		// The catch-up happened between frames, while scene.open waited: it is the open's work (its undo step), which a replay
		// of the open repeats, not an edit outside commands.
		const CommandJournalEntry* opened = harness.Context.GetJournal().Find(openEntry);
		REQUIRE(opened != nullptr);
		CHECK(opened->Command == "scene.open");
		CHECK(opened->UndoStep == "Update Instances of Lamp");
		CHECK(harness.Intensity(plain) == 9.0);
		CHECK(harness.Context.GetJournal().GetEditsOutsideCommands() == outsideCommands);
		CHECK(harness.Status(plain)["modified"] == false);
		CHECK(harness.Status(plain)["outdated"] == false);
		CHECK(harness.Intensity(edited) == 4.0);
		CHECK(harness.Get(edited)["components"]["PointLight"]["Color"] == nlohmann::json::array({ 0.0, 1.0, 0.0 }));
		CHECK(harness.Status(edited)["modified"] == true);
		CHECK(harness.Status(edited)["outdated"] == false);
		CHECK(harness.Status(edited)["overrides"].size() == 1);
		CHECK(harness.Intensity(legacy) == 5.0);
		CHECK(harness.Get(legacy)["components"]["PointLight"]["Color"] == nlohmann::json::array({ 1.0, 1.0, 1.0 }));
		CHECK(harness.Status(legacy)["outdated"] == true);
		CHECK(harness.History() == std::vector<std::string> { "Update Instances of Lamp" });
		CHECK(harness.Run("scene.info")["modified"] == true);
		harness.Frames(4); // Once per scene: nothing more happens
		CHECK(harness.History().size() == 1);

		// The outdated instance would write its older values over the prefab's: refused, unless forced.
		const EditorCommandResult refused = harness.Fail("prefab.apply", { { "entity", legacy } });
		CHECK(refused.Error.find("older version") != std::string::npos);
		CHECK(harness.ReadPrefabEntity("Prefabs/Lamp.stprefab", "Lamp")["Components"]["PointLight"]["Intensity"] == 9.0);
		// Reverting brings it up to date.
		harness.Run("prefab.revert", { { "entity", legacy } });
		CHECK(harness.Intensity(legacy) == 9.0);
		CHECK(harness.Status(legacy)["outdated"] == false);

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

	TEST_CASE("Every prefab an opened scene uses catches its instances up as the scene replaces the edited one, before anything runs")
	{
		PrefabHarness harness("PrefabCatchUpAtOpen");
		// Three prefabs whose sources are in one scene, and an instance of each in another.
		std::vector<std::string> sources;
		std::vector<std::string> prefabs;
		for (int index = 0; index < 3; index++)
		{
			const std::string name = "Lamp" + std::to_string(index);
			sources.push_back(harness.Create(name, nullptr, 2.0f));
			prefabs.push_back(harness.Run("prefab.create", { { "entities", { sources.back() } }, { "path", "Prefabs/" + name + ".stprefab" } })["asset"]
				.get<std::string>());
		}
		harness.Run("scene.saveAs", { { "path", "Scenes/Sources.stscene" } });
		harness.Run("scene.new");
		std::vector<std::string> instances;
		for (const std::string& prefab : prefabs)
			instances.push_back(harness.Run("prefab.instantiate", { { "prefab", prefab } })["id"].get<std::string>());
		const std::optional<UUID> scene = UUIDFromJson(harness.Run("scene.saveAs", { { "path", "Scenes/A.stscene" } })["scene"]);
		REQUIRE(scene);

		// All three change while A is closed.
		harness.Run("scene.open", { { "scene", "Scenes/Sources.stscene" } });
		for (const std::string& source : sources)
		{
			harness.SetIntensity(source, 6.0f);
			harness.Run("prefab.apply", { { "entity", source } });
		}
		harness.Run("scene.save");

		// Opened without an editor frame (EditorContext::OpenScene, as tools open scenes): when it returns, every instance has
		// its prefab's values, one undo step per prefab, and later frames add nothing.
		std::string error;
		REQUIRE_MESSAGE(harness.Context.OpenScene(*scene, &error), error);
		for (const std::string& instance : instances)
			CHECK(harness.Intensity(instance) == 6.0);
		CHECK(harness.History().size() == 3);
		harness.Frames(3);
		CHECK(harness.History().size() == 3);
	}

	TEST_CASE("While a scene is being opened, prefab commands that change the edited scene are refused before anything changes")
	{
		PrefabHarness harness("PrefabWhileOpening");
		LampSetup setup(harness);
		harness.SetIntensity(setup.First, 5.0f);
		harness.Run("scene.saveAs", { { "path", "Scenes/A.stscene" } });

		EditorCommandResult open = harness.Commands.Execute(harness.Context, "scene.open", { { "scene", "Scenes/A.stscene" } });
		REQUIRE(open.Success);
		REQUIRE(open.IsPending());
		REQUIRE(harness.Context.IsSceneLoading());
		const auto refused = [&harness](std::string_view name, const nlohmann::json& parameters)
		{
			const EditorCommandResult result = harness.Fail(name, parameters);
			INFO(name, ": ", result.Error);
			CHECK(result.ErrorKind == EditorCommandError::Failed);
			CHECK(result.Error.find("read-only") != std::string::npos);
		};
		refused("prefab.apply", { { "entity", setup.First } });
		refused("prefab.revert", { { "entity", setup.First } });
		refused("prefab.unpack", { { "entity", setup.First } });
		refused("prefab.create", { { "entities", { setup.Second } }, { "path", "Prefabs/Copy.stprefab" } });
		CHECK(harness.Intensity(setup.First) == 5.0);
		CHECK(harness.IsLinked(setup.First));
		CHECK(harness.ReadPrefabEntity("Prefabs/Lamp.stprefab", "Lamp")["Components"]["PointLight"]["Intensity"] == 2.0);
		CHECK_FALSE(FileSystem::Exists(harness.Directory / "Assets" / "Prefabs" / "Copy.stprefab"));
		// Without the link, prefab.create only reads the scene: it writes the prefab.
		harness.Run("prefab.create", { { "entities", { setup.Second } }, { "path", "Prefabs/Copy.stprefab" }, { "link", false } });
		CHECK(FileSystem::Exists(harness.Directory / "Assets" / "Prefabs" / "Copy.stprefab"));

		open = Tests::CompleteCommand(harness.Context, std::move(open));
		REQUIRE_MESSAGE(open.Success, open.Error);
		harness.Run("prefab.unpack", { { "entity", setup.First } });
		CHECK_FALSE(harness.IsLinked(setup.First));
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
		// The first edit after the pause is looked at at once (the links walked; the edited instance's override list says it is
		// modified, the other is compared); the rest of the run is not.
		CHECK(sync.GetCompareCount() - compares == 1);
		CHECK(sync.GetWatchScanCount() - scans == 1);

		// Once the edits pause, the marks follow them.
		harness.Frames(static_cast<int>(PrefabSync::c_SettleFrames));
		CHECK(sync.IsModified(harness.Context, first));
		CHECK_FALSE(sync.IsModified(harness.Context, second));
		CHECK(sync.GetCompareCount() - compares == 2);
		CHECK(sync.GetWatchScanCount() - scans == 2);
		// Links that come or go are found at once, also during a run.
		harness.Run("prefab.unpack", { { "entity", setup.Second } });
		harness.Frames(1);
		CHECK(sync.GetWatchScanCount() - scans == 3);
	}

	TEST_CASE("Edits keep the instance's override list on its root; undo restores it and scenes store it")
	{
		PrefabHarness harness("PrefabOverrideList");
		LampSetup setup(harness);
		Scene& scene = *harness.Context.GetEditScene();
		const Entity first = scene.GetEntityByUUID(*UUIDFromJson(setup.First));
		const auto overrides = [&]() { return first.GetComponent<PrefabInstanceComponent>().Overrides; };
		CHECK(overrides().empty());
		CHECK(harness.Status(setup.First)["overrides"].empty());

		// The root's transform and name are its own; a value below it is an override, reported with the scene's entity.
		harness.Run("component.set", { { "entity", setup.First }, { "component", "Transform" }, { "values", { { "Translation", { 9, 0, 0 } } } } });
		harness.Run("entity.rename", { { "entity", setup.First }, { "name", "Front Lamp" } });
		CHECK(overrides().empty());
		const std::string bulb = harness.ChildOf(setup.First);
		harness.SetIntensity(bulb, 6.0f);
		const UUID bulbTemplate = scene.GetEntityByUUID(*UUIDFromJson(bulb)).GetComponent<PrefabInstanceComponent>().PrefabEntityID;
		CHECK(overrides() == std::vector<PrefabOverride> { { bulbTemplate, "PointLight", "Intensity", PrefabOverrideKind::Property } });
		nlohmann::json status = harness.Status(bulb);
		REQUIRE(status["overrides"].size() == 1);
		CHECK(status["overrides"][0] == nlohmann::json { { "entity", bulb }, { "prefabEntity", bulbTemplate.ToString() }, { "component", "PointLight" },
			{ "property", "Intensity" }, { "kind", "Property" } });

		// A component added, then the entity removed: the list follows; undo and redo restore it with the entities.
		harness.Run("component.add", { { "entity", bulb }, { "component", "SpotLight" } });
		CHECK(overrides().size() == 2);
		harness.Run("entity.delete", { { "entities", { bulb } } });
		CHECK(overrides() == std::vector<PrefabOverride> { { bulbTemplate, "", "", PrefabOverrideKind::RemovedEntity } });
		CHECK(harness.Status(setup.First)["overrides"][0]["entity"].is_null());
		harness.Run("edit.undo");
		CHECK(overrides().size() == 2);
		harness.Run("edit.undo");
		harness.Run("edit.undo");
		CHECK(overrides().empty());
		harness.Run("edit.redo");
		CHECK(overrides().size() == 1);
		// The other instance is untouched; the marks read the list.
		const Entity second = scene.GetEntityByUUID(*UUIDFromJson(setup.Second));
		CHECK(second.GetComponent<PrefabInstanceComponent>().Overrides.empty());
		PrefabSync& sync = harness.Context.GetPrefabSync();
		const uint64_t compares = sync.GetCompareCount();
		CHECK(sync.IsModified(harness.Context, first));
		CHECK(sync.GetCompareCount() == compares);

		// Saved with the scene, only where there are overrides.
		harness.Run("scene.saveAs", { { "path", "Scenes/Lamps.stscene" } });
		const std::optional<std::string> text = FileSystem::ReadText(harness.Directory / "Assets" / "Scenes" / "Lamps.stscene");
		REQUIRE(text);
		CHECK(text->find("\"Overrides\"") != std::string::npos);
		CHECK(text->find("\"Overrides\"") == text->rfind("\"Overrides\""));
		harness.Run("scene.new");
		harness.Run("scene.open", { { "scene", "Scenes/Lamps.stscene" } });
		const Entity reopened = harness.Context.GetEditScene()->GetEntityByUUID(*UUIDFromJson(setup.First));
		REQUIRE(reopened);
		CHECK(reopened.GetComponent<PrefabInstanceComponent>().Overrides == std::vector<PrefabOverride> { { bulbTemplate, "PointLight", "Intensity",
			PrefabOverrideKind::Property } });

		// Edits while playing record nothing.
		harness.Run("play.start", { { "build", false } });
		const Entity running = harness.Context.GetActiveScene()->GetEntityByUUID(*UUIDFromJson(setup.First));
		harness.Run("component.set", { { "entity", harness.ChildOf(setup.First) }, { "component", "PointLight" }, { "values", { { "Range", 3 } } } });
		CHECK(running.GetComponent<PrefabInstanceComponent>().Overrides.size() == 1);
		harness.Run("play.stop");
	}

	TEST_CASE("A gizmo or Inspector drag on an instance's child is one undo step, and its override follows it")
	{
		PrefabHarness harness("PrefabOverrideDrag");
		LampSetup setup(harness);
		Scene& scene = *harness.Context.GetEditScene();
		const Entity first = scene.GetEntityByUUID(*UUIDFromJson(setup.First));
		const auto overrides = [&]() { return first.GetComponent<PrefabInstanceComponent>().Overrides; };
		Entity bulb = scene.GetEntityByUUID(*UUIDFromJson(harness.ChildOf(setup.First)));
		const UUID bulbTemplate = bulb.GetComponent<PrefabInstanceComponent>().PrefabEntityID;
		const glm::vec3 start = bulb.GetComponent<TransformComponent>().Translation;
		UndoStack& undo = harness.Context.GetUndoStack();
		const size_t steps = undo.GetHistory().size();
		const std::vector<PrefabOverride> moved = { { bulbTemplate, "Transform", "Translation", PrefabOverrideKind::Property } };
		const ComponentInfo* transform = ComponentRegistry::Find("Transform");
		REQUIRE(transform);
		const PropertyInfo* translation = transform->FindProperty("Translation");
		REQUIRE(translation);
		std::string error;

		SUBCASE("Gizmo")
		{
			harness.Context.Select(bulb.GetUUID());
			std::optional<TransformDrag> drag = TransformDrag::Begin(harness.Context, GizmoOperation::Translate);
			REQUIRE(drag);
			for (int frame = 1; frame <= 5; frame++)
			{
				const glm::mat4 gizmo = glm::translate(glm::mat4(1.0f), glm::vec3(0.5f * static_cast<float>(frame), 0.0f, 0.0f)) * drag->GetStartTransform();
				REQUIRE_MESSAGE(drag->Update(harness.Context, gizmo, &error), error);
				CHECK(overrides() == moved);
				CHECK(undo.GetHistory().size() == steps + 1);
			}
			drag->End(harness.Context);
		}
		SUBCASE("Inspector")
		{
			for (int frame = 1; frame <= 5; frame++)
			{
				const PropertyValue value(start + glm::vec3(0.5f * static_cast<float>(frame), 0.0f, 0.0f));
				REQUIRE_MESSAGE(SetPropertyWithUndo(harness.Context, bulb, *transform, *translation, value, &error), error);
				CHECK(overrides() == moved);
				CHECK(undo.GetHistory().size() == steps + 1);
			}
			undo.BreakMerge();
		}

		// One undo puts the child and the list back where they were before the drag; redo brings both.
		CHECK(undo.GetHistory().size() == steps + 1);
		harness.Run("edit.undo");
		CHECK(bulb.GetComponent<TransformComponent>().Translation == start);
		CHECK(overrides().empty());
		harness.Run("edit.redo");
		CHECK(bulb.GetComponent<TransformComponent>().Translation != start);
		CHECK(overrides() == moved);
	}

	TEST_CASE("An edit back to the prefab's value clears its override, also within a drag")
	{
		PrefabHarness harness("PrefabOverrideBack");
		LampSetup setup(harness);
		Scene& scene = *harness.Context.GetEditScene();
		const Entity first = scene.GetEntityByUUID(*UUIDFromJson(setup.First));
		const auto overrides = [&]() { return first.GetComponent<PrefabInstanceComponent>().Overrides; };
		const std::string bulbId = harness.ChildOf(setup.First);
		Entity bulb = scene.GetEntityByUUID(*UUIDFromJson(bulbId));
		harness.SetIntensity(bulbId, 6.0f);
		CHECK(overrides().size() == 1);
		harness.SetIntensity(bulbId, 3.0f); // The prefab's
		CHECK(overrides().empty());

		// A drag that comes back to where it started leaves neither an override nor a step.
		const size_t steps = harness.Context.GetUndoStack().GetHistory().size();
		harness.Context.Select(bulb.GetUUID());
		std::optional<TransformDrag> drag = TransformDrag::Begin(harness.Context, GizmoOperation::Translate);
		REQUIRE(drag);
		REQUIRE(drag->Update(harness.Context, glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f)) * drag->GetStartTransform()));
		CHECK(overrides().size() == 1);
		REQUIRE(drag->Update(harness.Context, drag->GetStartTransform()));
		drag->End(harness.Context);
		CHECK(overrides().empty());
		CHECK(harness.Context.GetUndoStack().GetHistory().size() == steps);
	}

	TEST_CASE("Edits of an instance work out the entries of the entities they change, not the whole instance")
	{
		PrefabHarness harness("PrefabOverrideUpkeep");
		const std::string rack = harness.Create("Rack");
		for (int slot = 0; slot < 40; slot++)
			harness.Create(fmt::format("Slot {}", slot), rack);
		const nlohmann::json created = harness.Run("prefab.create", { { "entities", { rack } }, { "path", "Prefabs/Rack.stprefab" } });
		std::vector<std::string> instances;
		for (int index = 0; index < 3; index++)
			instances.push_back(harness.Run("prefab.instantiate", { { "prefab", created["asset"] } })["id"].get<std::string>());
		Scene& scene = *harness.Context.GetEditScene();
		const EditorContext::OverrideUpkeepStats before = harness.Context.GetOverrideUpkeepStats();

		// A drag of one slot of an instance, then of all three roots: one entity per instance and frame.
		const Entity slot = scene.GetEntityByUUID(*UUIDFromJson(harness.ChildOf(instances[0], 7)));
		harness.Context.Select(slot.GetUUID());
		std::optional<TransformDrag> drag = TransformDrag::Begin(harness.Context, GizmoOperation::Translate);
		REQUIRE(drag);
		for (int frame = 1; frame <= 10; frame++)
			REQUIRE(drag->Update(harness.Context, glm::translate(glm::mat4(1.0f), glm::vec3(0.1f * static_cast<float>(frame), 0.0f, 0.0f)) * drag->GetStartTransform()));
		drag->End(harness.Context);
		harness.Context.SetSelection({ *UUIDFromJson(instances[0]), *UUIDFromJson(instances[1]), *UUIDFromJson(instances[2]) });
		drag = TransformDrag::Begin(harness.Context, GizmoOperation::Translate);
		REQUIRE(drag);
		for (int frame = 1; frame <= 10; frame++)
			REQUIRE(drag->Update(harness.Context, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.1f * static_cast<float>(frame), 0.0f)) * drag->GetStartTransform()));
		drag->End(harness.Context);
		const EditorContext::OverrideUpkeepStats& after = harness.Context.GetOverrideUpkeepStats();
		CHECK(after.WholeLists == before.WholeLists);
		CHECK(after.Entities - before.Entities == 10 + 10 * 3);
		CHECK(scene.GetEntityByUUID(*UUIDFromJson(instances[0])).GetComponent<PrefabInstanceComponent>().Overrides.size() == 1);

		// An edit that changes which entities an instance has works its whole list out.
		harness.Run("entity.delete", { { "entities", { harness.ChildOf(instances[1], 3) } } });
		CHECK(harness.Context.GetOverrideUpkeepStats().WholeLists == before.WholeLists + 1);
		CHECK(scene.GetEntityByUUID(*UUIDFromJson(instances[1])).GetComponent<PrefabInstanceComponent>().Overrides.size() == 1);
	}

	TEST_CASE("prefab.apply can write one override into the prefab; every other instance takes it unless it overrides it")
	{
		PrefabHarness harness("PrefabApplyOne");
		LampSetup setup(harness);
		const std::string firstBulb = harness.ChildOf(setup.First);
		const std::string secondBulb = harness.ChildOf(setup.Second);
		harness.SetIntensity(firstBulb, 6.0f);
		harness.Run("component.set", { { "entity", firstBulb }, { "component", "PointLight" }, { "values", { { "Range", 3 } } } });
		harness.SetIntensity(secondBulb, 7.0f);
		CHECK(harness.Status(setup.First)["overrides"].size() == 2);

		// Only the first bulb's range goes into the prefab: the original lamp and the second instance take it, the first keeps
		// its intensity override.
		const size_t steps = harness.History().size();
		const nlohmann::json applied = harness.Run("prefab.apply", { { "entity", firstBulb }, { "component", "PointLight" }, { "property", "Range" } });
		CHECK(applied["updatedInstances"] == 2);
		CHECK(harness.ReadPrefabEntity("Prefabs/Lamp.stprefab", "Bulb")["Components"]["PointLight"]["Range"] == 3.0);
		CHECK(harness.ReadPrefabEntity("Prefabs/Lamp.stprefab", "Bulb")["Components"]["PointLight"]["Intensity"] == 3.0);
		CHECK(harness.Get(secondBulb)["components"]["PointLight"]["Range"] == 3.0);
		CHECK(harness.Intensity(secondBulb) == 7.0);
		CHECK(harness.Get(setup.Bulb)["components"]["PointLight"]["Range"] == 3.0);
		const nlohmann::json overrides = harness.Status(setup.First)["overrides"];
		REQUIRE(overrides.size() == 1);
		CHECK(overrides[0]["property"] == "Intensity");
		CHECK(harness.Status(setup.First)["outdated"] == false);
		// One step for the file and every instance.
		CHECK(harness.History().size() == steps + 1);
		harness.Run("edit.undo");
		CHECK(harness.Get(secondBulb)["components"]["PointLight"]["Range"] != 3.0);
		CHECK(harness.ReadPrefabEntity("Prefabs/Lamp.stprefab", "Bulb")["Components"]["PointLight"].value("Range", 0.0) != 3.0);
		CHECK(harness.Status(setup.First)["overrides"].size() == 2);

		// An entity added to the instance cannot apply one override: it is not in the prefab.
		const std::string sticker = harness.Create("Sticker", setup.First);
		CHECK(harness.Fail("prefab.apply", { { "entity", sticker }, { "component", "PointLight" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("prefab.apply", { { "entity", firstBulb }, { "property", "Range" } }).ErrorKind == EditorCommandError::InvalidParameters);
	}

	TEST_CASE("prefab.revert can revert one property or one component of an entity")
	{
		PrefabHarness harness("PrefabRevertOne");
		LampSetup setup(harness);
		const std::string bulb = harness.ChildOf(setup.First);
		harness.SetIntensity(bulb, 6.0f);
		harness.Run("component.set", { { "entity", bulb }, { "component", "PointLight" }, { "values", { { "Range", 3 } } } });
		harness.Run("component.add", { { "entity", bulb }, { "component", "SpotLight" } });
		REQUIRE(harness.Status(setup.First)["overrides"].size() == 3);

		harness.Run("prefab.revert", { { "entity", bulb }, { "component", "PointLight" }, { "property", "Intensity" } });
		CHECK(harness.Intensity(bulb) == 3.0);
		CHECK(harness.Get(bulb)["components"]["PointLight"]["Range"] == 3.0);
		CHECK(harness.Status(setup.First)["overrides"].size() == 2);
		CHECK(harness.Run("edit.history")["undo"] == "Revert PointLight.Intensity of Bulb");
		// A component the prefab lacks goes.
		harness.Run("prefab.revert", { { "entity", bulb }, { "component", "SpotLight" } });
		CHECK_FALSE(harness.Get(bulb)["components"].contains("SpotLight"));
		CHECK(harness.Status(setup.First)["overrides"].size() == 1);
		harness.Run("edit.undo");
		CHECK(harness.Get(bulb)["components"].contains("SpotLight"));
		CHECK(harness.Status(setup.First)["overrides"].size() == 2);
		CHECK(harness.Fail("prefab.revert", { { "entity", bulb }, { "component", "SpotLight" }, { "property", "Intensity" } }).ErrorKind
			== EditorCommandError::InvalidParameters);
	}
}
