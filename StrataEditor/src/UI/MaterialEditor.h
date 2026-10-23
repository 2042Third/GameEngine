#pragma once

#include "Editor/AssetEdit.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Renderer/MaterialData.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class EditorContext;
	struct EditorPanelContext;
	struct MaterialProperties;

	// The properties of a material of the project, for editing (the asset inspector, and the inspector's inline section
	// for the material of a mesh renderer). A value being dragged previews in the loaded material
	// (AssetEdit::SetMaterialProperty with preview), so the viewport follows it, and is saved once the drag ends: one undo
	// step and one write of the file per drag; clicks (checkboxes, combos, texture pickers) save at once. Built-in materials
	// (only in memory) and those a model's import makes are not edited. A texture map whose import usage does not fit its
	// slot (AssetEdit::FindTextureUsageMismatches: a Color texture as the normal map...) gets a warning row under it with a
	// button that imports the texture as the slot reads it (asset.setImportSettings through the runner: a background
	// reimport). Main thread only.
	class MaterialEditor
	{
	public:
		// Draws the material's properties (or why they cannot be edited). `probeKey` prefixes the ItemProbe keys of the
		// fields ("<probeKey>.<Property>"; a warning's button "<probeKey>.<Property>.Fix").
		void Draw(EditorPanelContext& panelContext, AssetHandle material, std::string_view probeKey);
		// Saves a preview whose drag never finished (the panel was hidden or another material shown during it).
		void CommitPending(EditorContext& context);
	private:
		struct PendingPreview
		{
			AssetHandle Material;
			std::string Property;
		};
		// The warnings of the material shown, found again only when the material, its maps or the project's assets changed.
		const std::vector<TextureUsageMismatch>& GetMismatches(EditorContext& context, AssetHandle material, const MaterialProperties& properties);
	private:
		std::optional<PendingPreview> m_Pending;
		struct MismatchCache
		{
			AssetHandle Material = UUID::Null();
			std::array<AssetHandle, MaterialGPU::c_TextureMapCount> Maps = {};
			uint64_t AssetsVersion = UINT64_MAX;
			std::vector<TextureUsageMismatch> Mismatches;
		};
		MismatchCache m_Mismatches;
	};

}
