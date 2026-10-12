#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	struct AssetHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		std::filesystem::path Directory;

		explicit AssetHarness(const std::string& name, const std::string& templateId = "empty")
		{
			Directory = Tests::CreateTemporaryDirectory(name) / "Game";
			Run("project.create", { { "directory", FileSystem::ToUTF8(Directory) }, { "name", "Game" }, { "template", templateId } });
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

		EditorAssetManager& Assets() { return *Context.GetAssetManager(); }
		std::filesystem::path AssetPath(const std::string& relative) const { return Directory / "Assets" / FileSystem::FromUTF8(relative); }
		std::filesystem::path Meta(const std::string& relative) const
		{
			std::filesystem::path path = AssetPath(relative);
			path += ".meta";
			return path;
		}
		std::filesystem::path Trash() const { return Directory / ".strata" / "Trash"; }

		// The trash folders, by name.
		std::vector<std::filesystem::path> TrashEntries() const
		{
			std::vector<std::filesystem::path> entries;
			std::error_code error;
			for (std::filesystem::directory_iterator it(Trash(), error), end; !error && it != end; it.increment(error))
				entries.push_back(it->path());
			std::sort(entries.begin(), entries.end());
			return entries;
		}

		std::vector<std::string> History()
		{
			return Run("edit.history")["history"].get<std::vector<std::string>>();
		}
	};

	AssetHandle ToHandle(const nlohmann::json& value)
	{
		const std::optional<UUID> handle = UUIDFromJson(value);
		REQUIRE(handle);
		return *handle;
	}

}

TEST_SUITE("Editor.AssetOperations")
{
	TEST_CASE("Deleting a material a scene uses fails listing the scene; with force it goes to the trash, and undo brings it back")
	{
		AssetHarness harness("AssetOpsDeleteReferenced", "basic3d");
		const std::string ground = "Materials/Ground.stmat";
		const AssetHandle material = ToHandle(harness.Run("asset.info", { { "asset", ground } })["asset"]);
		const AssetHandle scene = harness.Context.GetSceneHandle();
		REQUIRE(scene.IsValid());
		const std::vector<std::string> historyBefore = harness.History();

		// The scene that uses it, with the entity, blocks the delete.
		const EditorCommandResult refused = harness.Fail("asset.delete", { { "asset", ground } });
		CHECK(refused.ErrorKind == EditorCommandError::Failed);
		CHECK(refused.Error.find("Scenes/Main.stscene") != std::string::npos);
		CHECK(refused.Error.find("force") != std::string::npos);
		REQUIRE(refused.ErrorData.is_object());
		const nlohmann::json& referencedBy = refused.ErrorData["referencedBy"];
		REQUIRE(referencedBy.size() == 1);
		CHECK(referencedBy[0]["path"] == "Scenes/Main.stscene");
		CHECK(referencedBy[0]["type"] == "Scene");
		CHECK(referencedBy[0]["openScene"] == true);
		REQUIRE(referencedBy[0]["entities"].size() == 1);
		CHECK(referencedBy[0]["entities"][0]["name"] == "Ground");
		CHECK(referencedBy[0]["entities"][0]["component"] == "MeshRenderer");
		CHECK(referencedBy[0]["entities"][0]["property"] == "Material");
		CHECK(FileSystem::IsRegularFile(harness.AssetPath(ground)));
		CHECK(harness.History() == historyBefore);
		// asset.findReferences answers the same.
		CHECK(harness.Run("asset.findReferences", { { "asset", ground } })["referencedBy"] == referencedBy);

		// Forced: the file and its .meta are in the trash, and the asset is gone from the project.
		const nlohmann::json deleted = harness.Run("asset.delete", { { "asset", ground }, { "force", true } });
		CHECK(deleted["referencedBy"] == referencedBy);
		CHECK_FALSE(FileSystem::Exists(harness.AssetPath(ground)));
		CHECK_FALSE(FileSystem::Exists(harness.Meta(ground)));
		const std::vector<std::filesystem::path> entries = harness.TrashEntries();
		REQUIRE(entries.size() == 1);
		CHECK(FileSystem::ToUTF8(entries[0]) == deleted["trash"].get<std::string>());
		CHECK(FileSystem::IsRegularFile(entries[0] / "Assets" / "Materials" / "Ground.stmat"));
		CHECK(FileSystem::IsRegularFile(entries[0] / "Assets" / "Materials" / "Ground.stmat.meta"));
		CHECK_FALSE(harness.Assets().IsHandleValid(material));
		CHECK_FALSE(harness.Commands.Execute(harness.Context, "asset.info", { { "asset", UUIDToJson(material) } }).Success);
		// One undo step that leaves the scene as it was saved.
		std::vector<std::string> history = harness.History();
		REQUIRE(history.size() == historyBefore.size() + 1);
		CHECK(history.back() == "Delete Materials/Ground.stmat");
		CHECK(harness.Run("scene.info")["modified"] == false);

		// Undo: the file, the .meta and the handle, so the scene's reference works again.
		harness.Run("edit.undo");
		CHECK(FileSystem::IsRegularFile(harness.AssetPath(ground)));
		CHECK(FileSystem::IsRegularFile(harness.Meta(ground)));
		CHECK(harness.TrashEntries().empty());
		const nlohmann::json restored = harness.Run("asset.info", { { "asset", ground } });
		CHECK(ToHandle(restored["asset"]) == material);
		CHECK(harness.Assets().GetAssetType(material) == AssetType::Material);
		const std::vector<AssetHandle> dependencies = harness.Assets().GetDependencies(scene);
		CHECK(std::find(dependencies.begin(), dependencies.end(), material) != dependencies.end());
		CHECK(harness.Assets().GetReferencers(material) == std::vector<AssetHandle> { scene });
		CHECK(AssetManager::GetActive()->LoadAssetSync(material) != nullptr);

		// Redo deletes it again (into a new trash folder), and undo once more restores it.
		harness.Run("edit.redo");
		CHECK_FALSE(FileSystem::Exists(harness.AssetPath(ground)));
		CHECK(harness.TrashEntries().size() == 1);
		harness.Run("edit.undo");
		CHECK(ToHandle(harness.Run("asset.info", { { "asset", ground } })["asset"]) == material);
	}

	TEST_CASE("An asset nothing uses is deleted without force; the open scene's unsaved uses count")
	{
		AssetHarness harness("AssetOpsDeleteUnused");
		const AssetHandle red = ToHandle(harness.Run("material.create", { { "path", "Materials/Red.stmat" } })["asset"]);
		const AssetHandle blue = ToHandle(harness.Run("material.create", { { "path", "Materials/Blue.stmat" } })["asset"]);

		// An entity of the scene, never saved, uses Blue: the open scene is listed without a file.
		harness.Run("entity.create", { { "name", "Crate" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) },
			{ "Material", UUIDToJson(blue) } } } } } });
		const nlohmann::json references = harness.Run("asset.findReferences", { { "asset", "Materials/Blue.stmat" } })["referencedBy"];
		REQUIRE(references.size() == 1);
		CHECK(references[0]["asset"].is_null());
		CHECK(references[0]["openScene"] == true);
		CHECK(references[0]["entities"][0]["name"] == "Crate");
		const EditorCommandResult refused = harness.Fail("asset.delete", { { "asset", UUIDToJson(blue) } });
		CHECK(refused.Error.find("Crate") != std::string::npos);

		// Red is used by nothing: deleted at once.
		CHECK(harness.Run("asset.findReferences", { { "asset", UUIDToJson(red) } })["referencedBy"].empty());
		const nlohmann::json deleted = harness.Run("asset.delete", { { "asset", UUIDToJson(red) } });
		CHECK(deleted["referencedBy"].empty());
		CHECK_FALSE(harness.Assets().IsHandleValid(red));
		// Sub-assets and built-in assets are no files of the project.
		CHECK(harness.Fail("asset.delete", { { "asset", "Builtin/Cube" } }).ErrorKind == EditorCommandError::InvalidParameters);
	}

	TEST_CASE("Moving folders and assets keeps every handle and reference, each in one undo step")
	{
		AssetHarness harness("AssetOpsMoveFolder");
		const AssetHandle a = ToHandle(harness.Run("material.create", { { "path", "Materials/A.stmat" } })["asset"]);
		const AssetHandle b = ToHandle(harness.Run("material.create", { { "path", "Materials/Metals/B.stmat" } })["asset"]);
		const std::string cube = harness.Run("entity.create", { { "name", "Cube" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) },
			{ "Material", UUIDToJson(b) } } } } } })["id"].get<std::string>();
		const AssetHandle prefab = ToHandle(harness.Run("prefab.create", { { "entities", { cube } }, { "path", "Prefabs/Cube.stprefab" } })["asset"]);
		harness.Run("entity.create", { { "name", "Sphere" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::SphereMesh) },
			{ "Material", UUIDToJson(a) } } } } } });
		const AssetHandle scene = ToHandle(harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } })["scene"]);
		EditorAssetManager& assets = harness.Assets();
		const std::vector<AssetHandle> sceneDependencies = assets.GetDependencies(scene);
		const size_t history = harness.History().size();

		const nlohmann::json moved = harness.Run("asset.moveFolder", { { "path", "Materials" }, { "to", "Art/Materials" } });
		CHECK(moved["path"] == "Art/Materials");
		CHECK(moved["assets"] == 2);
		CHECK(assets.GetMetadata(a)->Path == "Art/Materials/A.stmat");
		CHECK(assets.GetMetadata(b)->Path == "Art/Materials/Metals/B.stmat");
		CHECK(FileSystem::IsRegularFile(harness.Meta("Art/Materials/Metals/B.stmat")));
		CHECK_FALSE(FileSystem::Exists(harness.AssetPath("Materials")));
		CHECK(assets.GetDependencies(scene) == sceneDependencies);
		std::vector<AssetHandle> users = { prefab, scene };
		std::sort(users.begin(), users.end()); // GetReferencers sorts by handle
		CHECK(assets.GetReferencers(b) == users);
		const std::vector<std::string> folders = assets.GetFolders();
		CHECK(std::find(folders.begin(), folders.end(), "Art/Materials/Metals") != folders.end());
		CHECK(std::find(folders.begin(), folders.end(), "Materials") == folders.end());
		CHECK(assets.GetAssetsInFolder("Art/Materials") == std::vector<AssetHandle> { a });
		CHECK(harness.Run("scene.info")["modified"] == false);
		// The scene and the prefab still load with their references.
		CHECK(harness.Run("material.get", { { "material", "Art/Materials/Metals/B.stmat" } })["asset"] == UUIDToJson(b));
		REQUIRE(harness.History().size() == history + 1);

		harness.Run("edit.undo");
		CHECK(assets.GetMetadata(b)->Path == "Materials/Metals/B.stmat");
		CHECK(FileSystem::IsRegularFile(harness.AssetPath("Materials/A.stmat")));
		CHECK_FALSE(FileSystem::Exists(harness.AssetPath("Art/Materials")));
		harness.Run("edit.redo");
		CHECK(assets.GetMetadata(a)->Path == "Art/Materials/A.stmat");

		// A folder cannot move into itself or onto something that exists.
		CHECK_FALSE(harness.Fail("asset.moveFolder", { { "path", "Art" }, { "to", "Art/Materials/Inner" } }).Error.empty());
		CHECK_FALSE(harness.Fail("asset.moveFolder", { { "path", "Art/Materials" }, { "to", "Prefabs" } }).Error.empty());
		CHECK(harness.Fail("asset.moveFolder", { { "path", "Missing" }, { "to", "Elsewhere" } }).ErrorKind == EditorCommandError::InvalidParameters);

		// asset.move of one asset, undoable too.
		const nlohmann::json renamed = harness.Run("asset.move", { { "asset", UUIDToJson(a) }, { "path", "Art/Red.stmat" } });
		CHECK(renamed["path"] == "Art/Red.stmat");
		CHECK(assets.GetDependencies(scene) == sceneDependencies);
		harness.Run("edit.undo");
		CHECK(assets.GetMetadata(a)->Path == "Art/Materials/A.stmat");
	}

	TEST_CASE("Moving to the own path changes nothing, and names that differ only in case rename in place")
	{
		AssetHarness harness("AssetOpsMoveCase");
		const AssetHandle crate = ToHandle(harness.Run("material.create", { { "path", "Materials/crate.stmat" } })["asset"]);
		EditorAssetManager& assets = harness.Assets();
		const size_t history = harness.History().size();

		// The own path: nothing moves, and no undo step is recorded.
		CHECK(harness.Run("asset.move", { { "asset", UUIDToJson(crate) }, { "path", "Materials/crate.stmat" } })["path"] == "Materials/crate.stmat");
		CHECK(harness.Run("asset.moveFolder", { { "path", "Materials" }, { "to", "Materials" } })["path"] == "Materials");
		CHECK(harness.History().size() == history);

		// Only the case changes (on file systems that ignore case the destination "exists": it is the asset's own file).
		CHECK(harness.Run("asset.move", { { "asset", UUIDToJson(crate) }, { "path", "Materials/Crate.stmat" } })["path"] == "Materials/Crate.stmat");
		CHECK(assets.GetMetadata(crate)->Path == "Materials/Crate.stmat");
		CHECK(assets.FindAssetByPath("Materials/Crate.stmat") == crate);
		CHECK(harness.Run("asset.moveFolder", { { "path", "Materials" }, { "to", "materials" } })["path"] == "materials");
		CHECK(assets.GetMetadata(crate)->Path == "materials/Crate.stmat");
		CHECK(harness.Run("material.get", { { "material", "materials/Crate.stmat" } })["asset"] == UUIDToJson(crate));
		// The names on disk follow.
		std::vector<std::string> names;
		std::error_code error;
		for (std::filesystem::directory_iterator it(harness.AssetPath(""), error), end; !error && it != end; it.increment(error))
			names.push_back(FileSystem::ToUTF8(it->path().filename()));
		CHECK(std::find(names.begin(), names.end(), "materials") != names.end());
		CHECK(std::find(names.begin(), names.end(), "Materials") == names.end());
		REQUIRE(harness.History().size() == history + 2);

		harness.Run("edit.undo");
		harness.Run("edit.undo");
		CHECK(assets.GetMetadata(crate)->Path == "Materials/crate.stmat");

		// Another file at the destination still refuses the move.
		harness.Run("material.create", { { "path", "Materials/Other.stmat" } });
		CHECK_FALSE(harness.Fail("asset.move", { { "asset", UUIDToJson(crate) }, { "path", "Materials/Other.stmat" } }).Error.empty());
	}

	TEST_CASE("Folders are created, deleted with their assets and restored by undo")
	{
		AssetHarness harness("AssetOpsFolders");
		harness.Run("asset.createFolder", { { "path", "Levels/Forest" } });
		CHECK(FileSystem::IsDirectory(harness.AssetPath("Levels/Forest")));
		CHECK(harness.Assets().HasFolder("Levels/Forest"));
		CHECK(harness.Fail("asset.createFolder", { { "path", "Levels/Forest" } }).Error.find("exists") != std::string::npos);
		CHECK_FALSE(harness.Fail("asset.createFolder", { { "path", "../Outside" } }).Error.empty());
		CHECK_FALSE(harness.Fail("asset.createFolder", { { "path", "Levels/.hidden" } }).Error.empty());
		harness.Run("edit.undo");
		CHECK_FALSE(FileSystem::Exists(harness.AssetPath("Levels/Forest")));
		CHECK_FALSE(harness.Assets().HasFolder("Levels/Forest"));
		harness.Run("edit.redo");
		CHECK(FileSystem::IsDirectory(harness.AssetPath("Levels/Forest")));

		// A folder with assets, one of which the (unsaved) scene uses.
		const AssetHandle moss = ToHandle(harness.Run("material.create", { { "path", "Levels/Forest/Moss.stmat" } })["asset"]);
		const AssetHandle bark = ToHandle(harness.Run("material.create", { { "path", "Levels/Bark.stmat" } })["asset"]);
		harness.Run("entity.create", { { "name", "Rock" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) },
			{ "Material", UUIDToJson(moss) } } } } } });
		const EditorCommandResult refused = harness.Fail("asset.deleteFolder", { { "path", "Levels" } });
		CHECK(refused.ErrorData["referencedBy"].size() == 1);
		CHECK(FileSystem::IsDirectory(harness.AssetPath("Levels")));

		const nlohmann::json deleted = harness.Run("asset.deleteFolder", { { "path", "Levels" }, { "force", true } });
		CHECK(deleted["assets"] == 2);
		CHECK_FALSE(FileSystem::Exists(harness.AssetPath("Levels")));
		CHECK_FALSE(harness.Assets().IsHandleValid(moss));
		CHECK_FALSE(harness.Assets().IsHandleValid(bark));
		CHECK_FALSE(harness.Assets().HasFolder("Levels/Forest"));
		REQUIRE(harness.TrashEntries().size() == 1);
		CHECK(FileSystem::IsRegularFile(harness.TrashEntries()[0] / "Assets" / "Levels" / "Forest" / "Moss.stmat.meta"));
		CHECK(harness.History().back() == "Delete Folder Levels");

		harness.Run("edit.undo");
		CHECK(harness.Assets().GetMetadata(moss)->Path == "Levels/Forest/Moss.stmat");
		CHECK(harness.Assets().GetMetadata(bark)->Path == "Levels/Bark.stmat");
		CHECK(harness.Assets().HasFolder("Levels/Forest"));
		CHECK(harness.TrashEntries().empty());
		// The entity's material works again.
		CHECK(harness.Assets().GetReferencers(moss).empty()); // The scene was never saved: only the scene in memory uses it
		CHECK(harness.Run("asset.findReferences", { { "asset", UUIDToJson(moss) } })["referencedBy"][0]["openScene"] == true);
	}

	TEST_CASE("Undoing a delete fails while another asset is at its path, and the step stays")
	{
		AssetHarness harness("AssetOpsUndoBlocked");
		const AssetHandle first = ToHandle(harness.Run("material.create", { { "path", "Materials/Red.stmat" } })["asset"]);
		harness.Run("asset.delete", { { "asset", UUIDToJson(first) } });
		// A new asset at the path (material.create records no step of its own).
		const AssetHandle second = ToHandle(harness.Run("material.create", { { "path", "Materials/Red.stmat" } })["asset"]);
		const nlohmann::json before = harness.Run("edit.history");
		REQUIRE(before["undo"] == "Delete Materials/Red.stmat");

		const EditorCommandResult undo = harness.Fail("edit.undo", nlohmann::json::object());
		CHECK(undo.ErrorKind == EditorCommandError::Failed);
		CHECK(undo.Error.find("Something else is at 'Materials/Red.stmat'") != std::string::npos);
		CHECK(harness.Run("edit.history") == before);
		CHECK(harness.Assets().GetMetadata(second)->Path == "Materials/Red.stmat");
		CHECK_FALSE(harness.Assets().IsHandleValid(first));
		CHECK(harness.TrashEntries().size() == 1);
		// Nothing was undone, so there is nothing to redo, and the new material stays.
		CHECK(harness.Fail("edit.redo", nlohmann::json::object()).Error == "Nothing to redo");
		CHECK(harness.Assets().GetMetadata(second)->Path == "Materials/Red.stmat");
		CHECK(FileSystem::IsRegularFile(harness.AssetPath("Materials/Red.stmat")));
	}

	TEST_CASE("Undoing prefab.create leaves another asset at the prefab's path alone, and the step stays whole")
	{
		AssetHarness harness("AssetOpsCreateUndoBlocked");
		const UUID crate = *UUIDFromJson(harness.Run("entity.create", { { "name", "Crate" } })["id"]);
		const AssetHandle other = ToHandle(harness.Run("prefab.create", { { "entities", { UUIDToJson(crate) } }, { "path", "Prefabs/Crate.stprefab" },
			{ "link", false } })["asset"]);
		const UUID lamp = *UUIDFromJson(harness.Run("entity.create", { { "name", "Lamp" } })["id"]);
		const AssetHandle prefab = ToHandle(harness.Run("prefab.create", { { "entities", { UUIDToJson(lamp) } }, { "path", "Prefabs/Lamp.stprefab" } })["asset"]);
		const nlohmann::json before = harness.Run("edit.history");
		REQUIRE(before["undo"] == "Create Prefab Prefabs/Lamp.stprefab");

		// Replaced outside the history (a file manager, version control): the lamp's file goes and the crate's comes to its path.
		TrashedAssets trashed;
		std::string error;
		REQUIRE_MESSAGE(harness.Assets().MoveToTrash("Prefabs/Lamp.stprefab", trashed, &error), error);
		REQUIRE_MESSAGE(harness.Assets().MoveAsset(other, "Prefabs/Lamp.stprefab", &error), error);
		REQUIRE_FALSE(harness.Assets().IsHandleValid(prefab));

		// The undo would trash the crate's prefab: it fails, the step stays, and the entity keeps its link (the group's scene
		// part, undone first, is applied again).
		const EditorCommandResult undo = harness.Fail("edit.undo", nlohmann::json::object());
		CHECK(undo.Error.find("no longer the asset this step created") != std::string::npos);
		CHECK(harness.Run("edit.history") == before);
		CHECK(harness.Assets().FindAssetByPath("Prefabs/Lamp.stprefab") == other);
		CHECK(harness.TrashEntries().size() == 1);
		CHECK(harness.Context.GetEditScene()->GetEntityByUUID(lamp).HasComponent<PrefabInstanceComponent>());
	}

	TEST_CASE("Redoing a delete fails when the path holds other assets now, or the open scene")
	{
		AssetHarness harness("AssetOpsRedoChanged");
		const AssetHandle a = ToHandle(harness.Run("material.create", { { "path", "Props/A.stmat" } })["asset"]);
		harness.Run("asset.deleteFolder", { { "path", "Props" } });
		harness.Run("edit.undo");
		REQUIRE(harness.Assets().GetMetadata(a)->Path == "Props/A.stmat");

		// A material made in the restored folder: the step would trash it too.
		const AssetHandle b = ToHandle(harness.Run("material.create", { { "path", "Props/B.stmat" } })["asset"]);
		const EditorCommandResult redo = harness.Fail("edit.redo", nlohmann::json::object());
		CHECK(redo.Error.find("no longer holds what this step deleted") != std::string::npos);
		CHECK(harness.Assets().GetMetadata(a)->Path == "Props/A.stmat");
		CHECK(harness.Assets().GetMetadata(b)->Path == "Props/B.stmat");
		CHECK(harness.TrashEntries().empty());
		CHECK(harness.Run("edit.history")["redo"] == ""); // The step is gone from the history

		// The open scene saved into a restored folder: never trashed by a redo, like asset.deleteFolder refuses it.
		harness.Run("asset.deleteFolder", { { "path", "Props" } });
		harness.Run("edit.undo");
		harness.Run("scene.saveAs", { { "path", "Props/Level.stscene" } });
		CHECK_FALSE(harness.Fail("edit.redo", nlohmann::json::object()).Error.empty());
		CHECK(FileSystem::IsRegularFile(harness.AssetPath("Props/Level.stscene")));
		harness.Run("scene.save");
		CHECK(harness.Assets().GetMetadata(a)->Path == "Props/A.stmat");

		// A single asset: redone only while its path still holds it.
		const AssetHandle red = ToHandle(harness.Run("material.create", { { "path", "Materials/Red.stmat" } })["asset"]);
		harness.Run("asset.delete", { { "asset", UUIDToJson(red) } });
		harness.Run("edit.undo");
		harness.Run("edit.redo");
		CHECK_FALSE(harness.Assets().IsHandleValid(red));
	}

	TEST_CASE("asset.findReferences of a sub-asset lists the users of that sub-asset only")
	{
		AssetHarness harness("AssetOpsSubAssetReferences");
		const std::filesystem::path project = Tests::CopyFeatureProject(Tests::CreateTemporaryDirectory("AssetOpsSubAssetReferencesFeature") / "Project");
		harness.Run("project.open", { { "path", FileSystem::ToUTF8(project.parent_path()) } });
		EditorAssetManager& assets = harness.Assets();
		const AssetHandle model = assets.FindAssetByPath("Models/Platform.gltf");
		REQUIRE(model.IsValid());
		std::vector<AssetHandle> meshes;
		for (AssetHandle subAsset : assets.GetImportInfo(model).SubAssets)
		{
			if (assets.GetAssetType(subAsset) == AssetType::Mesh)
				meshes.push_back(subAsset);
		}
		REQUIRE(meshes.size() >= 2);

		// A scene that uses the second mesh only.
		harness.Run("scene.new");
		harness.Run("entity.create", { { "name", "OnlySecond" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(meshes[1]) } } } } } });
		harness.Run("scene.saveAs", { { "path", "Scenes/SecondOnly.stscene" } });
		const std::string scenePath = "Scenes/SecondOnly.stscene";
		auto lists = [&](AssetHandle asset)
		{
			const nlohmann::json found = harness.Run("asset.findReferences", { { "asset", UUIDToJson(asset) } });
			for (const nlohmann::json& referencer : found["referencedBy"])
			{
				if (referencer["path"] == scenePath)
					return true;
			}
			return false;
		};
		CHECK(lists(meshes[1]));
		CHECK_FALSE(lists(meshes[0]));
		// The model as a whole is used by the scene through its mesh.
		CHECK(lists(model));
	}

	TEST_CASE("An imported texture comes back from the trash with its import, without importing it again")
	{
		AssetHarness harness("AssetOpsTrashImported");
		const std::filesystem::path external = harness.Directory.parent_path() / "Brick.png";
		REQUIRE(FileSystem::WriteBytes(external, Tests::CreateSolidPNG(16, 16, 200, 100, 50)));
		const AssetHandle texture = ToHandle(Tests::RunCommandToCompletion(harness.Context, harness.Commands, "asset.import", { { "file", FileSystem::ToUTF8(external) }, { "directory", "Textures" } })["asset"]);
		const std::filesystem::path cache = harness.Directory / ".strata" / "Cache" / (texture.ToString() + ".bin");
		REQUIRE(FileSystem::IsRegularFile(cache));

		harness.Run("asset.delete", { { "asset", UUIDToJson(texture) } });
		CHECK_FALSE(FileSystem::Exists(cache)); // Its cached import went with it
		harness.Run("edit.undo");
		CHECK(FileSystem::IsRegularFile(cache));
		const nlohmann::json info = harness.Run("asset.info", { { "asset", UUIDToJson(texture) } });
		CHECK(info["imported"] == true);
		CHECK(info["importing"] == false);
		CHECK(info["importError"] == "");
		CHECK(AssetManager::GetActive()->LoadAssetSync(texture) != nullptr);
	}

	TEST_CASE("The trash folder of a project is cleaned of old entries when it opens")
	{
		AssetHarness harness("AssetOpsTrashPurge");
		harness.Run("material.create", { { "path", "Materials/Old.stmat" } });
		harness.Run("asset.delete", { { "asset", "Materials/Old.stmat" } });
		REQUIRE(harness.TrashEntries().size() == 1);
		const std::filesystem::path entry = harness.TrashEntries()[0];
		CHECK(FileSystem::IsRegularFile(entry / "Trashed.json"));
		// An entry from 40 days ago, and the one just made: only the old one goes when the project opens again.
		const std::filesystem::path old = harness.Trash() / "20200101-000000";
		REQUIRE(FileSystem::CreateDirectories(old / "Assets"));
		REQUIRE(FileSystem::WriteText(old / "Trashed.json", "{}"));
		std::error_code error;
		std::filesystem::last_write_time(old / "Trashed.json", std::filesystem::file_time_type::clock::now() - std::chrono::hours(40 * 24), error);
		REQUIRE_FALSE(error);
		harness.Context.CloseProject();
		harness.Run("project.open", { { "path", FileSystem::ToUTF8(harness.Directory) } });
		CHECK(Tests::WaitUntil([&]() { return !FileSystem::Exists(old); }, std::chrono::seconds(20)));
		CHECK(FileSystem::IsDirectory(entry));
	}
}
