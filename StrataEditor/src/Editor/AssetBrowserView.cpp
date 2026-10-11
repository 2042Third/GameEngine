#include "Editor/AssetBrowserView.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/StringUtils.h>

#include <algorithm>
#include <map>

namespace Strata
{

	namespace
	{

		// The search's words, lowercase.
		std::vector<std::string> SplitWords(const std::string& search)
		{
			std::vector<std::string> words;
			std::string word;
			for (const char character : StringUtils::ToLower(search))
			{
				if (character == ' ' || character == '\t')
				{
					if (!word.empty())
						words.push_back(std::move(word));
					word.clear();
				}
				else
				{
					word += character;
				}
			}
			if (!word.empty())
				words.push_back(std::move(word));
			return words;
		}

		bool MatchesWords(const std::string& name, const std::vector<std::string>& words)
		{
			const std::string lower = StringUtils::ToLower(name);
			return std::all_of(words.begin(), words.end(), [&lower](const std::string& word) { return lower.find(word) != std::string::npos; });
		}

		// Folders first, then by name ignoring case, then by name.
		bool ComesBefore(const AssetBrowserItem& a, const AssetBrowserItem& b)
		{
			if ((a.Kind == AssetBrowserItemKind::Folder) != (b.Kind == AssetBrowserItemKind::Folder))
				return a.Kind == AssetBrowserItemKind::Folder;
			const std::string lowerA = StringUtils::ToLower(a.Name);
			const std::string lowerB = StringUtils::ToLower(b.Name);
			return lowerA != lowerB ? lowerA < lowerB : a.Name < b.Name;
		}

	}

	std::string AssetBrowserView::GetParentFolder(std::string_view path)
	{
		const size_t slash = path.rfind('/');
		return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
	}

	std::string AssetBrowserView::GetFileName(std::string_view path)
	{
		const size_t slash = path.rfind('/');
		return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
	}

	void AssetBrowserView::SetFolder(std::string folder)
	{
		if (folder == m_Folder)
			return;
		m_Folder = std::move(folder);
		m_Dirty = true;
	}

	void AssetBrowserView::SetSearch(std::string search)
	{
		if (search == m_Search)
			return;
		m_Search = std::move(search);
		m_Dirty = true;
	}

	void AssetBrowserView::SetTypeFilter(std::set<AssetType> types)
	{
		if (types == m_Types)
			return;
		m_Types = std::move(types);
		m_Dirty = true;
	}

	void AssetBrowserView::SetFolderExpanded(const std::string& folder, bool expanded)
	{
		const bool changed = expanded ? m_ExpandedFolders.insert(folder).second : m_ExpandedFolders.erase(folder) > 0;
		m_Dirty = m_Dirty || changed;
	}

	bool AssetBrowserView::IsFolderExpanded(const std::string& folder) const
	{
		return m_ExpandedFolders.contains(folder);
	}

	void AssetBrowserView::ExpandTo(const std::string& folder)
	{
		std::string parent = GetParentFolder(folder);
		while (true)
		{
			SetFolderExpanded(parent, true);
			if (parent.empty())
				break;
			parent = GetParentFolder(parent);
		}
	}

	void AssetBrowserView::SetAssetExpanded(AssetHandle asset, bool expanded)
	{
		const bool changed = expanded ? m_ExpandedAssets.insert(asset).second : m_ExpandedAssets.erase(asset) > 0;
		m_Dirty = m_Dirty || changed;
	}

	std::optional<size_t> AssetBrowserView::FindItem(AssetHandle asset) const
	{
		for (size_t index = 0; index < m_Items.size(); index++)
		{
			if (m_Items[index].Kind != AssetBrowserItemKind::Folder && m_Items[index].Handle == asset)
				return index;
		}
		return std::nullopt;
	}

	bool AssetBrowserView::Update(const EditorAssetManager* assets)
	{
		if (!assets)
		{
			const bool hadRows = !m_Items.empty() || !m_FolderRows.empty();
			m_Items.clear();
			m_FolderRows.clear();
			m_Assets = nullptr;
			m_Dirty = true;
			return hadRows;
		}
		const uint64_t version = assets->GetChangeVersion();
		if (!m_Dirty && assets == m_Assets && version == m_Version)
			return false;
		m_Assets = assets;
		m_Version = version;
		m_Dirty = false;
		Rebuild(*assets);
		m_RebuildCount++;
		return true;
	}

	void AssetBrowserView::Rebuild(const EditorAssetManager& assets)
	{
		const std::vector<std::string> folders = assets.GetFolders();
		// A folder that went (deleted, moved outside the editor) shows its nearest parent that exists.
		while (!m_Folder.empty() && !std::binary_search(folders.begin(), folders.end(), m_Folder))
			m_Folder = GetParentFolder(m_Folder);

		// The tree: the asset directory, then each folder below an expanded one, depth first.
		std::map<std::string, std::vector<std::string>> children; // Parent -> child folders, sorted
		for (const std::string& folder : folders)
			children[GetParentFolder(folder)].push_back(folder);
		m_FolderRows.clear();
		std::vector<std::pair<std::string, uint32_t>> pending = { { std::string(), 0 } };
		while (!pending.empty())
		{
			auto [path, depth] = std::move(pending.back());
			pending.pop_back();
			const auto found = children.find(path);
			const bool hasChildren = found != children.end() && !found->second.empty();
			const bool expanded = m_ExpandedFolders.contains(path);
			m_FolderRows.push_back({ path, path.empty() ? std::string("Assets") : GetFileName(path), depth, hasChildren, expanded });
			if (hasChildren && expanded)
			{
				for (auto child = found->second.rbegin(); child != found->second.rend(); ++child)
					pending.emplace_back(*child, depth + 1);
			}
		}

		// The items.
		m_Items.clear();
		const std::vector<std::string> words = SplitWords(m_Search);
		const bool searching = !words.empty();
		if (!searching)
		{
			if (const auto found = children.find(m_Folder); found != children.end())
			{
				for (const std::string& folder : found->second)
				{
					AssetBrowserItem item;
					item.Kind = AssetBrowserItemKind::Folder;
					item.Name = GetFileName(folder);
					item.Path = folder;
					m_Items.push_back(std::move(item));
				}
			}
		}
		std::vector<AssetBrowserItem> files;
		for (AssetListing& listing : assets.ListAssets(m_Folder, searching))
		{
			if (!m_Types.empty() && !m_Types.contains(listing.Type))
				continue;
			AssetBrowserItem item;
			item.Kind = AssetBrowserItemKind::Asset;
			item.Name = GetFileName(listing.Path);
			if (searching && !MatchesWords(item.Name, words))
				continue;
			item.Path = std::move(listing.Path);
			item.Handle = listing.Handle;
			item.Type = listing.Type;
			item.HasSubAssets = !listing.SubAssets.empty();
			item.Expanded = item.HasSubAssets && m_ExpandedAssets.contains(listing.Handle);
			item.Importing = listing.Importing;
			item.ImportError = std::move(listing.Error);
			item.WarningCount = listing.WarningCount;
			files.push_back(std::move(item));
			if (!files.back().Expanded)
				continue;
			// An expanded model's meshes, materials and textures, as drag sources of their own.
			std::vector<AssetBrowserItem> subAssets;
			for (AssetHandle subAsset : listing.SubAssets)
			{
				const std::optional<AssetMetadata> metadata = assets.GetMetadata(subAsset);
				if (!metadata)
					continue;
				AssetBrowserItem entry;
				entry.Kind = AssetBrowserItemKind::SubAsset;
				entry.Name = metadata->Name;
				entry.Path = metadata->Path;
				entry.Handle = subAsset;
				entry.Type = metadata->Type;
				entry.Depth = 1;
				subAssets.push_back(std::move(entry));
			}
			std::stable_sort(subAssets.begin(), subAssets.end(), [](const AssetBrowserItem& a, const AssetBrowserItem& b) { return a.Type < b.Type; });
			files.back().HasSubAssets = !subAssets.empty();
			for (AssetBrowserItem& entry : subAssets)
				files.push_back(std::move(entry));
		}
		// Files by name; the sub-assets stay right after their file.
		std::vector<std::pair<size_t, size_t>> groups; // [first, end) of each file with its sub-assets
		for (size_t index = 0; index < files.size(); index++)
		{
			if (files[index].Kind == AssetBrowserItemKind::Asset)
				groups.emplace_back(index, index + 1);
			else if (!groups.empty())
				groups.back().second = index + 1;
		}
		std::stable_sort(groups.begin(), groups.end(), [&files](const auto& a, const auto& b) { return ComesBefore(files[a.first], files[b.first]); });
		std::sort(m_Items.begin(), m_Items.end(), ComesBefore);
		for (const auto& [first, end] : groups)
		{
			for (size_t index = first; index < end; index++)
				m_Items.push_back(std::move(files[index]));
		}
	}

}
