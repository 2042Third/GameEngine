#pragma once

#include "UI/DeleteAssetDialog.h"
#include "UI/EditorPanelRegistry.h"

#include <Strata/Asset/Asset.h>

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	class EditorCommandRegistry;
	class EditorContext;

	// Folders and assets of the project's asset directory. Assets are drag sources for the inspector (asset fields)
	// and the hierarchy (prefabs and models); files dropped onto the editor window are imported into the current
	// folder. The listing is refreshed periodically and after every action, not every frame.
	class ContentBrowserPanel : public EditorPanel
	{
	public:
		void OnImGuiRender(EditorPanelContext& context) override;

		// Opening a scene replaces the edited one: the editor decides how (e.g. after asking about unsaved changes).
		void SetOpenSceneHandler(std::function<void(AssetHandle)> handler) { m_OpenScene = std::move(handler); }

		// Imports external files into the current folder.
		void ImportFiles(EditorContext& context, const EditorCommandRegistry& commands, const std::vector<std::filesystem::path>& files);
		const std::string& GetCurrentDirectory() const { return m_CurrentDirectory; }

		// Shows an asset of the project: opens its folder, highlights it (a sub-asset: the file it comes from) and scrolls to it.
		// False, changing nothing, for assets without a file in the project (built-in and memory assets).
		bool RevealAsset(EditorContext& context, AssetHandle asset);
		AssetHandle GetRevealedAsset() const { return m_Revealed; }

		// Shows a folder of the asset directory ('/' separators, empty for the root) from the next frame on.
		void SetCurrentDirectory(std::string directory);

		// Where an asset's row was drawn in the last frame (screen coordinates), when it was in view: for tests.
		struct ItemRect
		{
			AssetHandle Asset;
			ImVec2 Min;
			ImVec2 Max;
		};
		std::optional<ItemRect> FindItemRect(AssetHandle asset) const;
	private:
		struct Item
		{
			AssetMetadata Metadata;
			std::string Label;
			std::string FileName;
			std::string ImportError;
			std::string ImportWarnings;
		};

		void Refresh(EditorContext& context);
		void DrawToolbar(EditorContext& context, const EditorCommandRegistry& commands);
		void DrawRenamePopup(EditorContext& context, const EditorCommandRegistry& commands);
		void OpenScene(EditorContext& context, const EditorCommandRegistry& commands, AssetHandle scene);
	private:
		std::string m_CurrentDirectory; // Relative to the asset directory, '/' separators, empty for the root
		std::string m_Filter;
		std::function<void(AssetHandle)> m_OpenScene;

		std::vector<std::string> m_Folders;
		std::vector<Item> m_Items;
		std::vector<ItemRect> m_ItemRects; // Drawn in the last frame
		double m_LastRefreshTime = -1.0;
		bool m_RefreshRequested = true;

		AssetHandle m_Revealed = UUID::Null(); // Highlighted until another item is clicked
		bool m_ScrollToRevealed = false;
		AssetHandle m_RenameAsset = UUID::Null();
		std::string m_RenameText;
		std::string m_NewFolderName;
		DeleteAssetDialog m_DeleteDialog;
		bool m_DeleteDialogWasOpen = false;
	};

}
