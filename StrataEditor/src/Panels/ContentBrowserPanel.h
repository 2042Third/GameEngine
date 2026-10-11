#pragma once

#include "Editor/AssetBrowserView.h"
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

	class EditorAssetManager;
	class EditorCommandRegistry;
	class EditorCommandRunner;
	class EditorContext;

	// The project's asset directory in two panes: a folder tree and the current folder's items as a grid of tiles (type icons,
	// or thumbnails once they are rendered) or a list, both clipped to what is in view. Above them: breadcrumbs of the folder,
	// a search (below the folder), type filter chips, the grid's tile size, New and Import. The rows come from
	// AssetBrowserView, rebuilt only when the asset manager's change version (or the folder, search or filter) changed, so an
	// idle frame lists nothing.
	//
	// Assets are drag sources (the inspector's asset fields, the viewport, the hierarchy), and so are an expanded model's
	// sub-assets; folders (tree rows, tiles, breadcrumbs) take assets and folders dropped on them (asset.move,
	// asset.moveFolder) and entities dragged from the Hierarchy (a linked prefab, prefab.create). Every change runs a command;
	// Delete asks first (DeleteAssetDialog). Files and folders dropped onto the editor window are imported into the current
	// folder. Double-click opens: a folder, a scene (asking about unsaved changes), any other asset in the Asset tab.
	// Inspecting an asset elsewhere (selection.set {asset}, the Streaming panel) reveals it: its folder opens in the tree and
	// the grid scrolls to it.
	class ContentBrowserPanel : public EditorPanel
	{
	public:
		void OnUpdate(EditorPanelContext& context) override;
		void OnImGuiRender(EditorPanelContext& context) override;

		// Opening a scene replaces the edited one: the editor decides how (e.g. after asking about unsaved changes).
		void SetOpenSceneHandler(std::function<void(AssetHandle)> handler) { m_OpenScene = std::move(handler); }

		// Imports external files and folders into the current folder (asset.import through the runner: copied and imported in
		// the background).
		void ImportFiles(EditorContext& context, const EditorCommandRegistry& commands, EditorCommandRunner& runner, const std::vector<std::filesystem::path>& paths);
		const std::string& GetCurrentDirectory() const { return m_View.GetFolder(); }

		// Shows an asset of the project: opens its folder (expanding the tree to it), selects it (a sub-asset: inside its
		// expanded model) and scrolls to it. False, changing nothing, for assets without a file in the project (built-in and
		// memory assets).
		bool RevealAsset(EditorContext& context, AssetHandle asset);
		AssetHandle GetRevealedAsset() const { return m_Revealed; }

		// Shows a folder of the asset directory ('/' separators, empty for the root) from the next frame on.
		void SetCurrentDirectory(std::string directory);

		enum class ViewMode : uint8_t
		{
			Grid = 0,
			List
		};
		void SetViewMode(ViewMode mode) { m_Mode = mode; }
		ViewMode GetViewMode() const { return m_Mode; }

		// Where an item was drawn in the last frame (screen coordinates), when it was in view: for tests.
		struct ItemRect
		{
			AssetHandle Asset;
			ImVec2 Min;
			ImVec2 Max;
		};
		std::optional<ItemRect> FindItemRect(AssetHandle asset) const;
		// Where a folder's tile, list row or tree row was drawn in the last frame, when it was in view.
		std::optional<ItemRect> FindFolderRect(const std::string& folder) const;
		const AssetBrowserView& GetView() const { return m_View; }

		// The grid's tile edge in font sizes, and its range.
		static constexpr float c_MinTileScale = 4.0f;
		static constexpr float c_MaxTileScale = 12.0f;
		static constexpr float c_DefaultTileScale = 6.5f;
		// Payload of a folder dragged from the browser: its path ('/' separators), without a terminator.
		static constexpr const char* c_FolderPayload = "STRATA_ASSET_FOLDER";
	private:
		// What the UI shows of an item, made when the view rebuilds (not every frame).
		struct ItemLabels
		{
			std::string Icon;
			std::string Title;    // The name a tile shows (without the file's extension)
			std::string ListName; // The list's name column: icon and file name
			std::string TypeName;
		};

		void DrawToolbar(EditorPanelContext& context, EditorAssetManager& assets);
		void DrawBreadcrumbs(EditorPanelContext& context);
		void DrawFilters();
		void DrawFolderTree(EditorPanelContext& context);
		void DrawItems(EditorPanelContext& context);
		void DrawGrid(EditorPanelContext& context);
		void DrawList(EditorPanelContext& context);
		void DrawEmptyState();
		// The interactions of an item just drawn (its selectable is the last item): selection, double-click, drag and drop,
		// context menu, tooltip.
		void HandleItem(EditorPanelContext& context, size_t index);
		void DrawItemMenu(EditorPanelContext& context, const AssetBrowserItem& item);
		void DrawFolderMenu(EditorPanelContext& context, const std::string& folder);
		void DrawBackgroundMenu(EditorPanelContext& context);
		// Takes what is dropped on a folder: assets and folders move into it, entities become a linked prefab in it.
		void AcceptDrops(EditorPanelContext& context, const std::string& folder);
		// The same inside a drop target begun already.
		void AcceptPayloads(EditorPanelContext& context, const std::string& folder);
		void DrawRenamePopup(EditorPanelContext& context);
		void HandleShortcuts(EditorPanelContext& context);
		void Open(EditorPanelContext& context, const AssetBrowserItem& item);
		void Select(EditorContext& context, const AssetBrowserItem& item);
		void BeginRename(const AssetBrowserItem& item);
		void CreateFolder(EditorPanelContext& context);
		void RebuildLabels();
		// The item's tile title, cut with an ellipsis to the width (cached per width).
		const std::string& GetTileTitle(size_t index, float width);
	private:
		AssetBrowserView m_View;
		const EditorAssetManager* m_ViewAssets = nullptr; // The manager the view was built for
		std::vector<ItemLabels> m_Labels;                // Per item of the view
		std::vector<std::string> m_FolderLabels;         // Per folder row of the view
		std::vector<std::string> m_TileTitles;           // Per item, cut to m_TileTitleWidth
		float m_TileTitleWidth = 0.0f;

		ViewMode m_Mode = ViewMode::Grid;
		float m_TileScale = c_DefaultTileScale;
		std::string m_Search;
		std::function<void(AssetHandle)> m_OpenScene;

		// The selection: an asset, or a folder of the current folder's items.
		AssetHandle m_Selected = UUID::Null();
		std::string m_SelectedFolder;
		AssetHandle m_Revealed = UUID::Null(); // Highlighted until another item is clicked
		bool m_ScrollToRevealed = false;
		bool m_ScrollTreeToFolder = false;
		uint64_t m_SeenInspectionVersion = 0;
		AssetHandle m_InspectedFromHere = UUID::Null(); // Inspected by a click in the browser: no reveal needed

		std::vector<ItemRect> m_ItemRects;   // Drawn in the last frame
		std::vector<std::pair<std::string, ItemRect>> m_FolderRects;

		// Rename (a popup): an asset (asset.move) or a folder (asset.moveFolder).
		bool m_OpenRename = false;
		AssetHandle m_RenameAsset = UUID::Null();
		std::string m_RenameFolder;
		std::string m_RenameText;
		DeleteAssetDialog m_DeleteDialog;
	};

}
