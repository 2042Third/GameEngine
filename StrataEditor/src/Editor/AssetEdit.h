#pragma once

#include "Editor/EditorCommands.h"
#include "Editor/UndoStack.h"

#include <Strata/Animation/AnimationController.h>
#include <Strata/Asset/AssetTypes.h>
#include <Strata/Asset/TextureImporter.h>
#include <Strata/Reflection/Property.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	class AssetManagerBase;
	class EditorAssetManager;
	class EditorContext;
	struct MaterialProperties;

	// A texture in a material slot whose import usage does not fit the slot (AssetEdit::FindTextureUsageMismatches).
	struct TextureUsageMismatch
	{
		std::string Property;  // The slot: a material's map property, "NormalMap", "BaseColorMap", "ClearcoatMap"...
		AssetHandle Texture = UUID::Null();
		TextureUsage Usage = TextureUsage::Color;    // As the texture is imported
		TextureUsage Expected = TextureUsage::Color; // What the slot reads it as
	};

	// Edits of engine-native asset documents (".stmat", ".stprefab", ".stscene", ".stanimator": JSON the engine writes itself)
	// with undo.
	// An edit writes the document's file and publishes the new document as the loaded asset at once
	// (AssetManagerBase::ReplaceLoadedAsset), so everything that uses the asset shows the change in the same frame; a preview
	// only publishes it, for live edits while a value is dragged.
	namespace AssetEdit
	{

		// The document as saved: the bytes of the asset's file. Fails for assets that are not documents of the project.
		bool ReadDocument(const EditorAssetManager& assets, AssetHandle handle, std::vector<uint8_t>& outDocument, std::string* outError = nullptr);
		// Writes the document (validated by the type's loader) to the asset's file and publishes it as the loaded asset when
		// the asset is loaded or loading (an unloaded one reads the file when it is next requested).
		bool ApplyDocument(EditorAssetManager& assets, AssetHandle handle, std::span<const uint8_t> document, std::string* outError = nullptr);
		// Publishes the document as the loaded asset without writing the file: a live preview, until the next saved edit,
		// undo or reload of the file replaces it.
		bool PreviewDocument(EditorAssetManager& assets, AssetHandle handle, std::span<const uint8_t> document, std::string* outError = nullptr);

		// "AlphaCutoff, AlphaMode, BaseColor, ...": the properties of materials, for messages.
		std::string ListMaterialProperties();
		// A material document: `base` (a material's Material::Serialize) with the given property values applied, validated by
		// the loader and written in the canonical form (tabs, a final newline). Unknown properties fail with the list of known
		// ones and invalid values with the loader's message; both are InvalidParameters errors.
		std::optional<std::string> BuildMaterialDocument(const nlohmann::json& base, const nlohmann::json* properties, std::string* outError = nullptr,
			EditorCommandError* outErrorKind = nullptr);

		// How SetMaterialProperties applies a change.
		struct MaterialEdit
		{
			// Only the loaded material changes: no file is written and no undo step recorded (live edits while a value is
			// dragged). Otherwise the file is written, with what earlier previews changed, and one undo step is recorded whose
			// state before is the file as it was.
			bool Preview = false;
			// A saved edit merges into the previous undo step with the same key until UndoStack::BreakMerge (a drag that saves
			// every frame is one step). Empty: never merges, as each command is its own step.
			std::string MergeKey;
		};

		// Sets property values ({"BaseColor": [1, 0, 0, 1], ...}, in the forms material.set accepts) of a material of the open
		// project. The material is loaded if it is not (blocking: the edited asset must be at hand). Fails, changing nothing,
		// without a project, for built-in materials (they exist only in memory), unknown properties and invalid values;
		// `outErrorKind` tells requests that cannot be accepted (InvalidParameters) from edits that could not be carried out.
		bool SetMaterialProperties(EditorContext& context, AssetHandle material, const nlohmann::json& properties, const MaterialEdit& edit,
			std::string* outError = nullptr, EditorCommandError* outErrorKind = nullptr);
		// One property, as the asset inspector edits it: previews while `preview`, else saves with the merge key
		// "Material/<handle>/<property>", so a drag that saves every frame is one undo step.
		bool SetMaterialProperty(EditorContext& context, AssetHandle material, const PropertyInfo& property, const PropertyValue& value, bool preview,
			std::string* outError = nullptr);
		// Whether the loaded material shows what its file holds (false while a preview is not saved). Loads nothing: an
		// unloaded material is what its file holds.
		bool IsMaterialSaved(EditorAssetManager& assets, AssetHandle material);

		// The material's texture maps whose import usage does not fit their slot: the Normal Map slot reads normal maps
		// (NormalMap), Metal/Rough and Occlusion read linear data (a Color texture would be sRGB-decoded there), Base Color and
		// Emissive read color (a NormalMap or Data texture there is a mistake too). The usage comes from the texture's import
		// settings; textures a model's import made are left out (the importer chose their usage from the slot), as are
		// unknown ones. Empty without a project.
		std::vector<TextureUsageMismatch> FindTextureUsageMismatches(EditorContext& context, const MaterialProperties& properties);
		// What material.get, material.set and material.create report: [{property, texture, usage, expected, message, fix:
		// {command: "asset.setImportSettings", parameters: {asset, settings: {Usage}}}}].
		nlohmann::json DescribeTextureUsageMismatches(EditorContext& context, const std::vector<TextureUsageMismatch>& mismatches);
		// "aerial_rocks_02_nor_gl_1k is imported as Color: its normals will be wrong".
		std::string DescribeTextureUsageMismatch(EditorContext& context, const TextureUsageMismatch& mismatch);

		// An animation controller document (".stanimator") with `data` as its content, validated (AnimationController::Create)
		// and written in the canonical form (tabs, a final newline). Invalid content fails with the reason.
		std::optional<std::string> BuildControllerDocument(const AnimationControllerData& data, std::string* outError = nullptr);
		// Replaces the content of an animation controller of the open project: the file is written, the loaded controller
		// replaced (animators and previews follow) and one undo step named `name` recorded; consecutive edits with the same
		// non-empty merge key are one step. Fails, changing nothing, without a project, for assets that are not controllers of
		// the project (InvalidParameters) and for invalid content (InvalidParameters, with the reason).
		bool SetControllerData(EditorContext& context, AssetHandle controller, const AnimationControllerData& data, const std::string& name,
			const std::string& mergeKey = {}, std::string* outError = nullptr, EditorCommandError* outErrorKind = nullptr);

	}

	// Undo step of an edit of an asset document: the document's bytes before and after. Undo and redo write them back and
	// publish them (AssetEdit::ApplyDocument). The step does not change the scene, so it never marks it unsaved. It refers
	// to the project's asset manager weakly: once the project is closed (which clears the history anyway) it does nothing.
	class AssetDocumentAction final : public EditorAction
	{
	public:
		AssetDocumentAction(EditorAssetManager& assets, AssetHandle handle, std::string name, std::vector<uint8_t> before, std::vector<uint8_t> after,
			std::string mergeKey = {});

		const std::string& GetName() const override { return m_Name; }
		bool Execute(std::string* outError) override;
		bool Undo(std::string* outError) override;
		// Consecutive edits with the same non-empty key of the same asset: the step keeps its state before and takes the
		// next one's state after.
		bool MergeWith(const EditorAction& next) override;
		bool IsNoOp() const override { return m_Before == m_After; }
		bool ChangesScene() const override { return false; }

		AssetHandle GetAsset() const { return m_Handle; }
		const std::vector<uint8_t>& GetBefore() const { return m_Before; }
		const std::vector<uint8_t>& GetAfter() const { return m_After; }
	private:
		bool Apply(const std::vector<uint8_t>& document, std::string* outError);
	private:
		std::weak_ptr<AssetManagerBase> m_Assets;
		AssetHandle m_Handle;
		std::string m_Name;
		std::vector<uint8_t> m_Before;
		std::vector<uint8_t> m_After;
		std::string m_MergeKey;
	};

}
