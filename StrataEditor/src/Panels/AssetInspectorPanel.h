#pragma once

#include "UI/AnimationAssetViews.h"
#include "UI/EditorPanelRegistry.h"
#include "UI/MaterialEditor.h"

#include <Strata/Asset/Asset.h>
#include <Strata/Core/Base.h>

#include <nlohmann/json.hpp>

#include <string>

namespace Strata
{

	class AssetImporter;
	class EditorAssetManager;

	// The asset the editor inspects (EditorContext::GetInspectedAsset: clicked in the Content Browser, or selection.set
	// {asset}), in a panel tabbed with the Inspector that comes to the front when an asset is inspected:
	// - a header with the asset's type, name, path and handle (with a copy button), and its actions (open a scene, add a
	//   prefab or model to the scene, re-import);
	// - a material's properties, edited live (MaterialEditor: a drag previews and is one undo step);
	// - a skeleton's joints, an animation clip's channels, and an animation controller's parameters, states and transitions
	//   as editable tables (AnimationAssetViews: one undo step per edit);
	// - the import settings of an imported asset (texture, model, audio clip; fonts have none) as a draft: Apply runs
	//   asset.setImportSettings through the command runner (the import runs in the background and the panel shows it),
	//   Revert discards the draft;
	// - a texture's image, and the asset's state and stored size.
	class AssetInspectorPanel : public EditorPanel
	{
	public:
		void OnImGuiRender(EditorPanelContext& context) override;
		void OnHidden(EditorPanelContext& context) override;
	private:
		void DrawHeader(const AssetMetadata& metadata);
		void DrawActions(EditorPanelContext& context, const AssetMetadata& metadata, const AssetImporter* importer);
		void DrawImportSettings(EditorPanelContext& context, EditorAssetManager& assets, const AssetMetadata& metadata, const AssetImporter& importer);
		void DrawTexturePreview(AssetHandle handle);
		void DrawDetails(EditorAssetManager& assets, const AssetMetadata& metadata);
	private:
		MaterialEditor m_MaterialEditor;
		AnimationAssetViews m_AnimationViews;

		// The import settings being edited: as the asset has them (Saved) and as edited (Edited).
		struct ImportDraft
		{
			AssetHandle Asset = UUID::Null();
			nlohmann::json Saved;
			nlohmann::json Edited;
		};
		ImportDraft m_Draft;
		// The last Apply, shared with the command's completion (the runner completes it in a later frame).
		struct ApplyState
		{
			bool Running = false;
			std::string Error;
		};
		Ref<ApplyState> m_Apply = CreateRef<ApplyState>();
	};

}
