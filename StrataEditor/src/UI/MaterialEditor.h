#pragma once

#include <Strata/Asset/AssetTypes.h>

#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	class EditorContext;

	// The properties of a material of the project, for editing (the asset inspector, and the inspector's inline section
	// for the material of a mesh renderer). A value being dragged previews in the loaded material
	// (AssetEdit::SetMaterialProperty with preview), so the viewport follows it, and is saved once the drag ends: one undo
	// step and one write of the file per drag; clicks (checkboxes, combos, texture pickers) save at once. Built-in materials
	// (only in memory) and those a model's import makes are not edited. Main thread only.
	class MaterialEditor
	{
	public:
		// Draws the material's properties (or why they cannot be edited). `probeKey` prefixes the ItemProbe keys of the
		// fields ("<probeKey>.<Property>").
		void Draw(EditorContext& context, AssetHandle material, std::string_view probeKey);
		// Saves a preview whose drag never finished (the panel was hidden or another material shown during it).
		void CommitPending(EditorContext& context);
	private:
		struct PendingPreview
		{
			AssetHandle Material;
			std::string Property;
		};
		std::optional<PendingPreview> m_Pending;
	};

}
