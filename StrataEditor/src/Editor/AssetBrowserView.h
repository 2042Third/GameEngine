#pragma once

#include <Strata/Asset/AssetTypes.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace Strata
{

	class EditorAssetManager;

	enum class AssetBrowserItemKind : uint8_t
	{
		Folder = 0,
		Asset,
		SubAsset // An asset imported with its file (a model's meshes, materials and textures), shown below it when expanded
	};

	// One entry of the Content Browser's grid or list.
	struct AssetBrowserItem
	{
		AssetBrowserItemKind Kind = AssetBrowserItemKind::Asset;
		std::string Name;  // A folder's or file's name, or a sub-asset's
		std::string Path;  // Relative to the asset directory (sub-assets: their file's)
		AssetHandle Handle = UUID::Null();
		AssetType Type = AssetType::None;
		uint32_t Depth = 0; // 1 for sub-assets
		bool HasSubAssets = false;
		bool Expanded = false;
		bool Importing = false;
		std::string ImportError;
		size_t WarningCount = 0;
	};

	// One row of the Content Browser's folder tree: the asset directory ("Assets", path "") and its folders, as far as they
	// are expanded.
	struct AssetBrowserFolderRow
	{
		std::string Path; // "" for the asset directory
		std::string Name;
		uint32_t Depth = 0;
		bool HasChildren = false;
		bool Expanded = false;
	};

	// The Content Browser's rows without UI, like HierarchyView for the Hierarchy: the folder tree and the items of the
	// current folder (folders first, then assets, each by name; an expanded model's sub-assets after it), or with a search
	// the assets below the current folder whose names match, filtered by type. Rebuilt by Update only when the asset
	// manager's change version (EditorAssetManager::GetChangeVersion), the folder, the search, the type filter or an
	// expansion changed, so an unchanged frame copies nothing.
	class AssetBrowserView
	{
	public:
		// Rebuilds when something it shows changed (or the manager is another); returns true when it did. Main thread.
		bool Update(const EditorAssetManager* assets);
		// Rebuilds at the next Update whatever changed (another project's manager may reuse an address).
		void Invalidate() { m_Dirty = true; }

		// The folder whose items show ('/' separators, "" for the asset directory). A folder that no longer exists falls back
		// to its nearest parent at the next Update.
		void SetFolder(std::string folder);
		const std::string& GetFolder() const { return m_Folder; }
		// Case-insensitive words a name must contain (all of them); empty shows the folder's items.
		void SetSearch(std::string search);
		const std::string& GetSearch() const { return m_Search; }
		// The asset types shown; empty shows every type (folders show either way, but not while searching).
		void SetTypeFilter(std::set<AssetType> types);
		const std::set<AssetType>& GetTypeFilter() const { return m_Types; }

		void SetFolderExpanded(const std::string& folder, bool expanded);
		bool IsFolderExpanded(const std::string& folder) const;
		// Expands every folder above `folder` (and the asset directory), so its row shows.
		void ExpandTo(const std::string& folder);
		void SetAssetExpanded(AssetHandle asset, bool expanded);

		const std::vector<AssetBrowserItem>& GetItems() const { return m_Items; }
		const std::vector<AssetBrowserFolderRow>& GetFolderRows() const { return m_FolderRows; }
		// The index of an item showing an asset or sub-asset; nothing when it does not show.
		std::optional<size_t> FindItem(AssetHandle asset) const;
		// How often Update rebuilt the rows (tests count it: an idle editor rebuilds nothing).
		uint64_t GetRebuildCount() const { return m_RebuildCount; }

		// The folder a relative path is in ("Models/Crate.gltf" -> "Models", "Models" -> "").
		static std::string GetParentFolder(std::string_view path);
		// "Models/Crate.gltf" -> "Crate.gltf".
		static std::string GetFileName(std::string_view path);
	private:
		void Rebuild(const EditorAssetManager& assets);
	private:
		std::string m_Folder;
		std::string m_Search;
		std::set<AssetType> m_Types;
		std::unordered_set<std::string> m_ExpandedFolders = { "" };
		std::unordered_set<AssetHandle> m_ExpandedAssets;

		std::vector<AssetBrowserItem> m_Items;
		std::vector<AssetBrowserFolderRow> m_FolderRows;

		const EditorAssetManager* m_Assets = nullptr;
		uint64_t m_Version = 0;
		bool m_Dirty = true;
		uint64_t m_RebuildCount = 0;
	};

}
