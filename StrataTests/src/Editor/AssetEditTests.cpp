#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/AssetEdit.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Renderer/Material.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// An editor with a project holding the material "Materials/Red.stmat".
	struct MaterialHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		AssetHandle Red = UUID::Null();
		std::filesystem::path RedFile;

		MaterialHarness()
		{
			const std::filesystem::path directory = Tests::CreateTemporaryDirectory("EditorMaterial") / "Game";
			Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Game" } });
			Red = *UUIDFromJson(Run("material.create", { { "path", "Materials/Red.stmat" },
				{ "properties", { { "BaseColor", { 1, 0, 0, 1 } }, { "Roughness", 0.5 } } } })["asset"]);
			RedFile = Context.GetAssetManager()->GetAbsolutePath(Red);
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			const EditorCommandResult result = Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
			INFO(name, ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		EditorCommandResult Fail(std::string_view name, const nlohmann::json& parameters)
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			CHECK_FALSE(result.Success);
			return result;
		}

		std::vector<uint8_t> ReadFile() const
		{
			const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(RedFile);
			REQUIRE(bytes);
			return *bytes;
		}

		// The loaded material, as the renderer gets it.
		MaterialProperties Loaded() const
		{
			Ref<Material> material = AssetManager::GetAsset<Material>(Red);
			REQUIRE(material);
			return material->GetProperties();
		}

		size_t HistorySize() { return Context.GetUndoStack().GetHistory().size(); }
	};

	const PropertyInfo& FindProperty(std::string_view name)
	{
		const std::vector<PropertyInfo>& properties = Material::GetPropertyInfos();
		const auto it = std::find_if(properties.begin(), properties.end(), [name](const PropertyInfo& property) { return property.Name == name; });
		REQUIRE(it != properties.end());
		return *it;
	}

}

TEST_SUITE("Editor.Material")
{
	TEST_CASE("material.set {save: false} changes the loaded material without writing its file")
	{
		MaterialHarness harness;
		REQUIRE(AssetManager::LoadAssetSync<Material>(harness.Red));
		const std::vector<uint8_t> saved = harness.ReadFile();
		const size_t history = harness.HistorySize();

		const nlohmann::json preview = harness.Run("material.set", { { "material", "Materials/Red.stmat" }, { "properties", { { "BaseColor", { 0, 0, 1, 1 } } } },
			{ "save", false } });
		CHECK(preview["saved"] == false);
		CHECK(preview["values"]["BaseColor"] == nlohmann::json { 0.0, 0.0, 1.0, 1.0 });
		CHECK(harness.Loaded().BaseColor == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
		CHECK(harness.ReadFile() == saved);
		CHECK(harness.HistorySize() == history);
		CHECK(harness.Run("material.get", { { "material", "Materials/Red.stmat" } })["saved"] == false);

		// A saving edit saves the previewed values with its own; the step's state before is the file as it was.
		harness.Run("material.set", { { "material", "Materials/Red.stmat" }, { "properties", { { "Roughness", 0.25 } } } });
		CHECK(harness.HistorySize() == history + 1);
		CHECK(harness.Run("material.get", { { "material", "Materials/Red.stmat" } })["saved"] == true);
		const Ref<Material> written = Material::Deserialize(harness.ReadFile());
		REQUIRE(written);
		CHECK(written->GetProperties().BaseColor == glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
		CHECK(written->GetProperties().Roughness == 0.25f);

		harness.Run("edit.undo");
		CHECK(harness.ReadFile() == saved);
		CHECK(harness.Loaded().BaseColor == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
		CHECK(harness.Loaded().Roughness == 0.5f);

		// Material edits are not scene changes: the scene stays saved.
		CHECK_FALSE(harness.Context.IsSceneModified());
	}

	TEST_CASE("Undo and redo of a material edit restore the file and the loaded values")
	{
		MaterialHarness harness;
		const std::vector<uint8_t> before = harness.ReadFile();
		REQUIRE(AssetManager::LoadAssetSync<Material>(harness.Red));
		const uint64_t version = AssetManager::GetActive()->GetContentVersion();

		harness.Run("material.set", { { "material", UUIDToJson(harness.Red) }, { "properties", { { "Metallic", 1.0 }, { "AlphaMode", "Blend" } } } });
		const std::vector<uint8_t> after = harness.ReadFile();
		CHECK(after != before);
		// Shown in the same frame: the loaded object was replaced, which caches of loaded assets hear about.
		CHECK(harness.Loaded().Metallic == 1.0f);
		CHECK(harness.Loaded().AlphaMode == MaterialAlphaMode::Blend);
		CHECK(AssetManager::GetActive()->GetContentVersion() != version);
		CHECK(harness.Context.GetUndoStack().GetUndoName() == "Edit Material Red");

		harness.Run("edit.undo");
		CHECK(harness.ReadFile() == before);
		CHECK(harness.Loaded().Metallic == 0.0f);
		CHECK(harness.Loaded().AlphaMode == MaterialAlphaMode::Opaque);

		harness.Run("edit.redo");
		CHECK(harness.ReadFile() == after);
		CHECK(harness.Loaded().Metallic == 1.0f);

		// An unloaded material only gets its file: it reads it when it is next used.
		harness.Run("edit.undo");
		AssetManager::GetActive()->UnloadAsset(harness.Red);
		harness.Run("edit.redo");
		CHECK(harness.ReadFile() == after);
		CHECK(AssetManager::GetAssetState(harness.Red) == AssetState::Unloaded);
		CHECK(AssetManager::LoadAssetSync<Material>(harness.Red)->GetProperties().Metallic == 1.0f);
	}

	TEST_CASE("A 60-step slider drag is one undo step")
	{
		MaterialHarness harness;
		const std::vector<uint8_t> before = harness.ReadFile();
		const PropertyInfo& roughness = FindProperty("Roughness");
		const size_t history = harness.HistorySize();
		std::string error;

		// The asset inspector's drag: previews while the slider moves, then saves once when it is released.
		for (int step = 1; step < 60; step++)
		{
			REQUIRE_MESSAGE(AssetEdit::SetMaterialProperty(harness.Context, harness.Red, roughness, PropertyValue(step / 60.0f), true, &error), error);
			CHECK(harness.Loaded().Roughness == step / 60.0f);
		}
		CHECK(harness.ReadFile() == before);
		CHECK(harness.HistorySize() == history);
		REQUIRE(AssetEdit::SetMaterialProperty(harness.Context, harness.Red, roughness, PropertyValue(1.0f), false, &error));
		harness.Context.GetUndoStack().BreakMerge();
		CHECK(harness.HistorySize() == history + 1);
		CHECK(harness.Context.GetUndoStack().GetUndoName() == "Edit Red Roughness");

		// A drag that saves every step merges by property, until the merge is broken (the mouse is released).
		const std::vector<uint8_t> afterFirstDrag = harness.ReadFile();
		for (int step = 0; step < 60; step++)
			REQUIRE(AssetEdit::SetMaterialProperty(harness.Context, harness.Red, roughness, PropertyValue(step / 120.0f), false, &error));
		harness.Context.GetUndoStack().BreakMerge();
		CHECK(harness.HistorySize() == history + 2);
		CHECK(harness.Loaded().Roughness == 59.0f / 120.0f);
		// Another property is a step of its own.
		REQUIRE(AssetEdit::SetMaterialProperty(harness.Context, harness.Red, FindProperty("Metallic"), PropertyValue(0.5f), false, &error));
		CHECK(harness.HistorySize() == history + 3);

		harness.Run("edit.undo");
		harness.Run("edit.undo");
		CHECK(harness.ReadFile() == afterFirstDrag);
		harness.Run("edit.undo");
		CHECK(harness.ReadFile() == before);
		CHECK(harness.Loaded().Roughness == 0.5f);
		CHECK_FALSE(harness.Context.IsSceneModified());
	}

	TEST_CASE("Material edits validate like material.create and change nothing when they fail")
	{
		MaterialHarness harness;
		const std::vector<uint8_t> before = harness.ReadFile();
		const size_t history = harness.HistorySize();

		const EditorCommandResult unknown = harness.Fail("material.set", { { "material", "Materials/Red.stmat" }, { "properties", { { "Shininess", 1 } } } });
		CHECK(unknown.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(unknown.Error.find("'Shininess'") != std::string::npos);
		for (const PropertyInfo& property : Material::GetPropertyInfos())
			CHECK_MESSAGE(unknown.Error.find(property.Name) != std::string::npos, property.Name);
		CHECK(harness.Fail("material.set", { { "material", "Materials/Red.stmat" }, { "properties", { { "AlphaMode", "Glowing" } } } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("material.set", { { "material", "Materials/Red.stmat" }, { "properties", { { "Roughness", 1 } } }, { "save", "no" } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("material.create", { { "path", "Materials/Bad.stmat" }, { "properties", { { "Shininess", 1 } } } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		// Built-in materials exist only in memory.
		const EditorCommandResult builtin = harness.Fail("material.set", { { "material", "Builtin/DefaultMaterial" }, { "properties", { { "Roughness", 1 } } } });
		CHECK(builtin.ErrorKind == EditorCommandError::Failed);
		CHECK(builtin.Error.find("built in") != std::string::npos);
		CHECK(harness.Fail("material.set", { { "material", "Builtin/DefaultMaterial" }, { "properties", { { "Roughness", 1 } } }, { "save", false } }).Error
			.find("built in") != std::string::npos);

		CHECK(harness.ReadFile() == before);
		CHECK(harness.Loaded().Roughness == 0.5f);
		CHECK(harness.HistorySize() == history);

		// Setting what the material already holds saves nothing and records no step.
		harness.Run("material.set", { { "material", "Materials/Red.stmat" }, { "properties", { { "Roughness", 0.5 } } } });
		CHECK(harness.HistorySize() == history);
	}

	TEST_CASE("Material edits keep their own steps among scene edits and never mark the scene unsaved")
	{
		MaterialHarness harness;
		harness.Run("entity.create", { { "name", "Cube" }, { "components", { { "MeshRenderer", { { "Mesh", UUIDToJson(BuiltinAssets::CubeMesh) },
			{ "Material", UUIDToJson(harness.Red) } } } } } });
		harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
		CHECK_FALSE(harness.Context.IsSceneModified());

		harness.Run("material.set", { { "material", "Materials/Red.stmat" }, { "properties", { { "Roughness", 0.9 } } } });
		CHECK_FALSE(harness.Context.IsSceneModified());
		harness.Run("entity.create", { { "name", "Light" } });
		CHECK(harness.Context.IsSceneModified());
		harness.Run("edit.undo");
		CHECK_FALSE(harness.Context.IsSceneModified());
		harness.Run("edit.undo");
		CHECK(harness.Loaded().Roughness == 0.5f);
		CHECK_FALSE(harness.Context.IsSceneModified());

		// Closing the project ends its history; steps of a closed project's manager would do nothing.
		AssetDocumentAction orphan(*harness.Context.GetAssetManager(), harness.Red, "Orphan", harness.ReadFile(), harness.ReadFile());
		harness.Context.CloseProject();
		std::string error;
		CHECK_FALSE(orphan.Execute(&error));
		CHECK(error.find("closed") != std::string::npos);
	}
}
