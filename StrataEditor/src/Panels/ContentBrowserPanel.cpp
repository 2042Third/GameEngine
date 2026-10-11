#include "Panels/ContentBrowserPanel.h"

#include "Editor/CommandUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ThumbnailCache.h"
#include "UI/EditorActions.h"
#include "UI/EditorFonts.h"
#include "UI/EditorShell.h"
#include "UI/FileDialogs.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/PropertyWidgets.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Asset/AssetImporter.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Entity.h>
#include <Strata/Scene/Scene.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Strata
{

	namespace
	{

		// The type filter's chips: a label and the asset types it shows.
		struct TypeChip
		{
			const char* Id;
			const char* Label;
			AssetType Type;
		};
		constexpr TypeChip c_TypeChips[] = {
			{ "ContentBrowser.Filter.Scene", "Scenes", AssetType::Scene },
			{ "ContentBrowser.Filter.Prefab", "Prefabs", AssetType::Prefab },
			{ "ContentBrowser.Filter.Model", "Models", AssetType::Model },
			{ "ContentBrowser.Filter.Material", "Materials", AssetType::Material },
			{ "ContentBrowser.Filter.Texture", "Textures", AssetType::Texture },
			{ "ContentBrowser.Filter.AudioClip", "Audio", AssetType::AudioClip },
			{ "ContentBrowser.Filter.Font", "Fonts", AssetType::Font },
		};

		// The tree pane's width when the panel opens, in font sizes.
		constexpr float c_TreeWidthInFontSizes = 13.0f;

		std::string JoinPath(const std::string& folder, const std::string& name)
		{
			return folder.empty() ? name : folder + "/" + name;
		}

		std::string GetStem(const std::string& fileName)
		{
			const size_t dot = fileName.rfind('.');
			return dot == std::string::npos || dot == 0 ? fileName : fileName.substr(0, dot);
		}

		// The text cut at its end with an ellipsis to fit the width in the current font.
		std::string ElideEnd(const std::string& text, float maxWidth)
		{
			if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth)
				return text;
			constexpr const char* c_Ellipsis = "...";
			const float ellipsisWidth = ImGui::CalcTextSize(c_Ellipsis).x;
			// Whole UTF-8 characters only: a cut inside one would show a broken glyph.
			size_t low = 0;
			size_t high = text.size();
			while (low < high)
			{
				size_t middle = (low + high + 1) / 2;
				while (middle > 0 && middle < text.size() && (static_cast<unsigned char>(text[middle]) & 0xC0) == 0x80)
					middle--;
				if (middle <= low)
					break;
				if (ImGui::CalcTextSize(text.c_str(), text.c_str() + middle).x + ellipsisWidth <= maxWidth)
					low = middle;
				else
					high = middle - 1;
			}
			return text.substr(0, low) + c_Ellipsis;
		}

		void SetAssetPayload(AssetHandle handle, const std::string& label)
		{
			ImGui::SetDragDropPayload(DragDrop::c_Asset, &handle, sizeof(AssetHandle));
			ImGui::TextUnformatted(label.c_str());
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// State
	////////////////////////////////////////////////////////////////////////////////

	void ContentBrowserPanel::OnUpdate(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		// An asset inspected elsewhere (selection.set {asset}, the Streaming panel, the Asset tab) is revealed here; one this
		// panel inspected is in view already.
		if (context.GetInspectionVersion() != m_SeenInspectionVersion)
		{
			m_SeenInspectionVersion = context.GetInspectionVersion();
			const AssetHandle inspected = context.GetInspectedAsset();
			if (inspected.IsValid() && inspected != m_InspectedFromHere)
				RevealAsset(context, inspected);
			m_InspectedFromHere = UUID::Null();
		}
	}

	bool ContentBrowserPanel::RevealAsset(EditorContext& context, AssetHandle asset)
	{
		const EditorAssetManager* assets = context.GetAssetManager();
		const std::optional<AssetMetadata> metadata = assets ? assets->GetMetadata(asset) : std::nullopt;
		if (!metadata || metadata->IsBuiltin() || metadata->Path.empty())
			return false;
		// A sub-asset shows inside its model.
		if (metadata->IsSubAsset())
			m_View.SetAssetExpanded(metadata->Parent, true);
		const std::string folder = AssetBrowserView::GetParentFolder(metadata->Path);
		m_View.SetFolder(folder);
		m_View.ExpandTo(folder);
		m_View.SetSearch({});
		m_Search.clear();
		m_Selected = asset;
		m_SelectedFolder.clear();
		m_Revealed = asset;
		m_ScrollToRevealed = true;
		m_ScrollTreeToFolder = true;
		return true;
	}

	void ContentBrowserPanel::SetCurrentDirectory(std::string directory)
	{
		m_View.SetFolder(std::move(directory));
	}

	std::optional<ContentBrowserPanel::ItemRect> ContentBrowserPanel::FindItemRect(AssetHandle asset) const
	{
		for (const ItemRect& item : m_ItemRects)
		{
			if (item.Asset == asset)
				return item;
		}
		return std::nullopt;
	}

	std::optional<ContentBrowserPanel::ItemRect> ContentBrowserPanel::FindFolderRect(const std::string& folder) const
	{
		for (const auto& [path, rect] : m_FolderRects)
		{
			if (path == folder)
				return rect;
		}
		return std::nullopt;
	}

	void ContentBrowserPanel::RebuildLabels()
	{
		m_Labels.clear();
		m_Labels.reserve(m_View.GetItems().size());
		for (const AssetBrowserItem& item : m_View.GetItems())
		{
			ItemLabels labels;
			if (item.Kind == AssetBrowserItemKind::Folder)
			{
				labels.Icon = Icons::Folder;
				labels.Title = item.Name;
				labels.TypeName = "Folder";
			}
			else
			{
				labels.Icon = UI::GetAssetTypeIcon(item.Type);
				labels.Title = item.Kind == AssetBrowserItemKind::SubAsset ? item.Name : GetStem(item.Name);
				labels.TypeName = UI::GetAssetTypeDisplayName(item.Type);
			}
			labels.ListName = fmt::format("{}  {}", labels.Icon, item.Name);
			m_Labels.push_back(std::move(labels));
		}
		m_FolderLabels.clear();
		for (const AssetBrowserFolderRow& row : m_View.GetFolderRows())
		{
			const bool current = row.Path == m_View.GetFolder();
			m_FolderLabels.push_back(fmt::format("{}  {}", row.Path.empty() ? Icons::House : (current ? Icons::FolderOpen : Icons::Folder), row.Name));
		}
		m_TileTitles.clear();
		m_TileTitleWidth = 0.0f;
	}

	const std::string& ContentBrowserPanel::GetTileTitle(size_t index, float width)
	{
		if (width != m_TileTitleWidth || m_TileTitles.size() != m_Labels.size())
		{
			m_TileTitleWidth = width;
			m_TileTitles.assign(m_Labels.size(), std::string());
		}
		if (m_TileTitles[index].empty())
			m_TileTitles[index] = ElideEnd(m_Labels[index].Title, width);
		return m_TileTitles[index];
	}

	////////////////////////////////////////////////////////////////////////////////
	// Actions
	////////////////////////////////////////////////////////////////////////////////

	void ContentBrowserPanel::ImportFiles(EditorContext& context, const EditorCommandRegistry& commands, EditorCommandRunner& runner,
		const std::vector<std::filesystem::path>& paths)
	{
		// One asset.import for files and folders alike: it copies on an I/O job and imports in the background, and the assets
		// show as importing meanwhile. Files no importer reads are left out (asset.import would refuse the whole request), and
		// said so.
		nlohmann::json files = nlohmann::json::array();
		nlohmann::json directories = nlohmann::json::array();
		std::string skipped;
		for (const std::filesystem::path& path : paths)
		{
			if (FileSystem::IsDirectory(path))
				directories.push_back(FileSystem::ToUTF8(path));
			else if (FileSystem::IsRegularFile(path) && AssetImporterRegistry::FindByExtension(StringUtils::ToLower(FileSystem::ToUTF8(path.extension()))))
				files.push_back(FileSystem::ToUTF8(path));
			else
				skipped += (skipped.empty() ? "" : ", ") + FileSystem::ToUTF8(path.filename());
		}
		if (!skipped.empty())
			ST_WARN("Not imported (no asset type Strata imports): {}", skipped);
		if (files.empty() && directories.empty())
			return;
		runner.Run(context, commands, "asset.import", { { "files", std::move(files) }, { "directories", std::move(directories) }, { "directory", m_View.GetFolder() } },
			[](const EditorCommandResult& result)
			{
				// Failures of the items are logged by the import, once each (their list is in the error's data); the others
				// (the project closed, the target folder is gone) here.
				if (!result.Success && !(result.ErrorData.is_object() && result.ErrorData.contains("items")))
					ST_ERROR("Import failed: {}", result.Error);
			});
	}

	void ContentBrowserPanel::Select(EditorContext& context, const AssetBrowserItem& item)
	{
		m_Revealed = UUID::Null();
		if (item.Kind == AssetBrowserItemKind::Folder)
		{
			m_Selected = UUID::Null();
			m_SelectedFolder = item.Path;
			return;
		}
		m_Selected = item.Handle;
		m_SelectedFolder.clear();
		// The Asset tab shows it; this browser keeps the keyboard focus.
		m_InspectedFromHere = item.Handle;
		context.SetInspectedAsset(item.Handle);
	}

	void ContentBrowserPanel::Open(EditorPanelContext& panelContext, const AssetBrowserItem& item)
	{
		EditorContext& context = panelContext.Context;
		if (item.Kind == AssetBrowserItemKind::Folder)
		{
			m_View.SetFolder(item.Path);
			m_View.ExpandTo(item.Path);
			m_SelectedFolder.clear();
			return;
		}
		if (item.Type == AssetType::Scene)
		{
			if (m_OpenScene)
				m_OpenScene(item.Handle);
			else
				RunEditorCommand(context, panelContext.Commands, "scene.open", { { "scene", UUIDToJson(item.Handle) } });
			return;
		}
		// Anything else in the Asset tab (inspecting it again brings the tab forward).
		m_InspectedFromHere = item.Handle;
		context.SetInspectedAsset(item.Handle);
	}

	void ContentBrowserPanel::BeginRename(const AssetBrowserItem& item)
	{
		if (item.Kind == AssetBrowserItemKind::SubAsset)
			return;
		m_RenameAsset = item.Kind == AssetBrowserItemKind::Folder ? UUID::Null() : item.Handle;
		m_RenameFolder = item.Kind == AssetBrowserItemKind::Folder ? item.Path : std::string();
		m_RenameText = item.Name;
		m_OpenRename = true;
	}

	void ContentBrowserPanel::CreateFolder(EditorPanelContext& panelContext)
	{
		EditorAssetManager* assets = panelContext.Context.GetAssetManager();
		if (!assets)
			return;
		// "New Folder", "New Folder 2", ...: the first name that is free, then renamed in place.
		std::string name = "New Folder";
		for (uint32_t number = 2; assets->HasFolder(JoinPath(m_View.GetFolder(), name)) || FileSystem::Exists(assets->GetAssetDirectory() /
			FileSystem::FromUTF8(JoinPath(m_View.GetFolder(), name))); number++)
			name = fmt::format("New Folder {}", number);
		const std::string path = JoinPath(m_View.GetFolder(), name);
		if (RunEditorCommand(panelContext.Context, panelContext.Commands, "asset.createFolder", { { "path", path } }).is_null())
			return;
		AssetBrowserItem folder;
		folder.Kind = AssetBrowserItemKind::Folder;
		folder.Name = name;
		folder.Path = path;
		m_SelectedFolder = path;
		m_Selected = UUID::Null();
		BeginRename(folder);
	}

	void ContentBrowserPanel::AcceptDrops(EditorPanelContext& panelContext, const std::string& folder)
	{
		if (!ImGui::BeginDragDropTarget())
			return;
		AcceptPayloads(panelContext, folder);
		ImGui::EndDragDropTarget();
	}

	void ContentBrowserPanel::AcceptPayloads(EditorPanelContext& panelContext, const std::string& folder)
	{
		EditorContext& context = panelContext.Context;
		const EditorCommandRegistry& commands = panelContext.Commands;
		EditorAssetManager* assets = context.GetAssetManager();
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Asset); payload && assets)
		{
			AssetHandle dropped;
			std::memcpy(&dropped, payload->Data, sizeof(AssetHandle));
			const std::optional<AssetMetadata> metadata = assets->GetMetadata(dropped);
			// Sub-assets go with their file; one already there stays.
			if (metadata && !metadata->IsSubAsset() && !metadata->IsBuiltin() && AssetBrowserView::GetParentFolder(metadata->Path) != folder)
			{
				RunEditorCommand(context, commands, "asset.move", { { "asset", UUIDToJson(dropped) },
					{ "path", JoinPath(folder, AssetBrowserView::GetFileName(metadata->Path)) } });
			}
		}
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(c_FolderPayload))
		{
			const std::string moved(static_cast<const char*>(payload->Data), static_cast<size_t>(payload->DataSize));
			const std::string destination = JoinPath(folder, AssetBrowserView::GetFileName(moved));
			if (moved != folder && AssetBrowserView::GetParentFolder(moved) != folder)
				RunEditorCommand(context, commands, "asset.moveFolder", { { "path", moved }, { "to", destination } });
		}
		// Entities dragged from the Hierarchy become a prefab in the folder, named after the first, which they are an instance of.
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(DragDrop::c_Entities); payload && assets && !context.IsPlaying())
		{
			std::vector<UUID> dropped(static_cast<size_t>(payload->DataSize) / sizeof(UUID));
			if (!dropped.empty())
				std::memcpy(dropped.data(), payload->Data, dropped.size() * sizeof(UUID));
			const Ref<Scene> scene = context.GetActiveScene();
			const Entity first = dropped.empty() || !scene ? Entity() : scene->GetEntityByUUID(dropped.front());
			if (first)
			{
				nlohmann::json entities = nlohmann::json::array();
				for (UUID id : dropped)
					entities.push_back(UUIDToJson(id));
				const std::string path = CommandUtils::MakeUniqueAssetPath(*assets, folder, first.GetName(), ".stprefab", "Prefab");
				const nlohmann::json created = RunEditorCommand(context, commands, "prefab.create", { { "entities", std::move(entities) }, { "path", path } });
				if (created.is_object())
				{
					if (const std::optional<UUID> prefab = UUIDFromJson(created.value("asset", nlohmann::json())))
						m_Selected = *prefab;
				}
			}
		}
	}

	////////////////////////////////////////////////////////////////////////////////
	// Drawing
	////////////////////////////////////////////////////////////////////////////////

	void ContentBrowserPanel::OnImGuiRender(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		m_ItemRects.clear();
		m_FolderRects.clear();
		EditorAssetManager* assets = context.GetAssetManager();
		if (!assets)
		{
			m_View.Update(nullptr);
			m_ViewAssets = nullptr;
			UI::CenteredText("ContentBrowser.NoProject", "Open or create a project to manage its assets (File menu)", UI::GetThemeColors().TextSecondary);
			return;
		}
		// Another project: its rows (a folder it lacks falls back to the asset directory).
		if (assets != m_ViewAssets)
		{
			m_View.Invalidate();
			m_ViewAssets = assets;
			m_Selected = UUID::Null();
			m_SelectedFolder.clear();
		}
		if (m_View.Update(assets))
			RebuildLabels();

		DrawToolbar(panelContext, *assets);

		const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings;
		if (ImGui::BeginTable("ContentBrowser.Panes", 2, flags, ImGui::GetContentRegionAvail()))
		{
			ImGui::TableSetupColumn("Folders", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * c_TreeWidthInFontSizes);
			ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			if (ImGui::BeginChild("ContentBrowser.Tree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None))
				DrawFolderTree(panelContext);
			ImGui::EndChild();
			ImGui::TableSetColumnIndex(1);
			if (ImGui::BeginChild("ContentBrowser.Items", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None))
				DrawItems(panelContext);
			ImGui::EndChild();
			ImGui::EndTable();
		}

		HandleShortcuts(panelContext);
		DrawRenamePopup(panelContext);
		m_DeleteDialog.Draw(context, panelContext.Commands);
	}

	void ContentBrowserPanel::DrawToolbar(EditorPanelContext& panelContext, EditorAssetManager& assets)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		EditorContext& context = panelContext.Context;

		if (UI::IconButton("ContentBrowser.Up", Icons::CornerLeftUp, "Up one folder (Backspace)", !m_View.GetFolder().empty()))
			m_View.SetFolder(AssetBrowserView::GetParentFolder(m_View.GetFolder()));
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		DrawBreadcrumbs(panelContext);

		// New and Import at the right end.
		const float newWidth = ImGui::GetFrameHeight() + ImGui::CalcTextSize("New").x + style.FramePadding.x * 3.0f;
		const float importWidth = ImGui::GetFrameHeight() + ImGui::CalcTextSize("Import").x + style.FramePadding.x * 3.0f;
		const float right = newWidth + importWidth + style.ItemSpacing.x;
		ImGui::SameLine();
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - right));
		if (UI::ToolbarButton("ContentBrowser.New", Icons::Plus, "New", "Make a folder or a material here"))
			ImGui::OpenPopup("ContentBrowser.NewMenu");
		if (ImGui::BeginPopup("ContentBrowser.NewMenu"))
		{
			if (ImGui::MenuItem((std::string(Icons::FolderPlus) + "  Folder").c_str()))
				CreateFolder(panelContext);
			if (ImGui::MenuItem((std::string(Icons::Palette) + "  Material").c_str()))
			{
				RunEditorCommand(context, panelContext.Commands, "material.create",
					{ { "path", CommandUtils::MakeUniqueAssetPath(assets, m_View.GetFolder(), "Material", ".stmat", "Material") } });
			}
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		if (UI::ToolbarButton("ContentBrowser.Import", Icons::Import, "Import", "Copy files or a folder into this folder and import them (or drop them onto the editor)"))
			ImGui::OpenPopup("ContentBrowser.ImportMenu");
		if (ImGui::BeginPopup("ContentBrowser.ImportMenu"))
		{
			if (ImGui::MenuItem((std::string(Icons::FileUp) + "  Files...").c_str()))
			{
				const std::vector<std::filesystem::path> files = FileDialogs::OpenFiles({});
				if (!files.empty())
					ImportFiles(context, panelContext.Commands, panelContext.Runner, files);
			}
			if (ImGui::MenuItem((std::string(Icons::FolderInput) + "  Folder...").c_str()))
			{
				if (const std::optional<std::filesystem::path> folder = FileDialogs::PickFolder())
					ImportFiles(context, panelContext.Commands, panelContext.Runner, { *folder });
			}
			ImGui::EndPopup();
		}

		// The second row: the search, the type chips, and the view's mode and size at the right end.
		if (UI::SearchField("ContentBrowser.Search", m_Search, "Search", ImGui::GetFontSize() * 12.0f, "Names below this folder that contain every word"))
			m_View.SetSearch(m_Search);
		ImGui::SameLine();
		DrawFilters();

		const float sliderWidth = ImGui::GetFontSize() * 5.0f;
		const float viewWidth = ImGui::GetFrameHeight() * 2.0f + style.ItemInnerSpacing.x + (m_Mode == ViewMode::Grid ? sliderWidth + style.ItemSpacing.x : 0.0f);
		ImGui::SameLine();
		ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - viewWidth));
		if (m_Mode == ViewMode::Grid)
		{
			ImGui::SetNextItemWidth(sliderWidth);
			ImGui::SliderFloat("##TileSize", &m_TileScale, c_MinTileScale, c_MaxTileScale, "");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Tile size");
			ImGui::SameLine();
		}
		UI::ButtonStyle grid;
		grid.Active = m_Mode == ViewMode::Grid;
		if (UI::ToolbarButton("ContentBrowser.Grid", Icons::LayoutGrid, nullptr, "Show tiles", grid))
			m_Mode = ViewMode::Grid;
		ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
		UI::ButtonStyle list;
		list.Active = m_Mode == ViewMode::List;
		if (UI::ToolbarButton("ContentBrowser.List", Icons::List, nullptr, "Show a list", list))
			m_Mode = ViewMode::List;
	}

	void ContentBrowserPanel::DrawBreadcrumbs(EditorPanelContext& panelContext)
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const std::string& folder = m_View.GetFolder();
		std::vector<std::pair<std::string, std::string>> crumbs = { { std::string(), std::string("Assets") } }; // Path, name
		for (size_t start = 0; start < folder.size();)
		{
			const size_t slash = folder.find('/', start);
			const size_t end = slash == std::string::npos ? folder.size() : slash;
			crumbs.emplace_back(folder.substr(0, end), folder.substr(start, end - start));
			start = end + 1;
		}
		for (size_t index = 0; index < crumbs.size(); index++)
		{
			if (index > 0)
			{
				ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
				ImGui::AlignTextToFramePadding();
				ImGui::TextColored(UI::GetThemeColors().TextDisabled, "%s", Icons::ChevronRight);
				ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
			}
			else
			{
				ImGui::AlignTextToFramePadding();
			}
			const std::string id = fmt::format("ContentBrowser.Crumb{}", index);
			if (index + 1 == crumbs.size())
			{
				UI::PushFont(UI::EditorFont::SemiBold, UI::TextSize::Body);
				ImGui::TextUnformatted(crumbs[index].second.c_str());
				ImGui::PopFont();
				UI::ItemProbe::Record(id);
			}
			else if (UI::LinkButton(id.c_str(), crumbs[index].second))
			{
				m_View.SetFolder(crumbs[index].first);
			}
			AcceptDrops(panelContext, crumbs[index].first);
		}
	}

	void ContentBrowserPanel::DrawFilters()
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		std::set<AssetType> types = m_View.GetTypeFilter();
		bool changed = false;
		for (size_t index = 0; index < std::size(c_TypeChips); index++)
		{
			const TypeChip& chip = c_TypeChips[index];
			bool on = types.contains(chip.Type);
			if (index > 0)
				ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
			if (UI::ToggleChip(chip.Id, UI::GetAssetTypeIcon(chip.Type), chip.Label, &on, "Show only these types (several can be on)"))
			{
				if (on)
					types.insert(chip.Type);
				else
					types.erase(chip.Type);
				changed = true;
			}
		}
		if (changed)
			m_View.SetTypeFilter(std::move(types));
	}

	void ContentBrowserPanel::DrawFolderTree(EditorPanelContext& panelContext)
	{
		const std::vector<AssetBrowserFolderRow>& rows = m_View.GetFolderRows();
		const float indent = ImGui::GetStyle().IndentSpacing;
		if (m_ScrollTreeToFolder)
		{
			// A revealed asset's folder row comes into view.
			const auto current = std::find_if(rows.begin(), rows.end(), [this](const AssetBrowserFolderRow& row) { return row.Path == m_View.GetFolder(); });
			if (current != rows.end())
			{
				const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
				const float top = static_cast<float>(current - rows.begin()) * rowHeight;
				if (top < ImGui::GetScrollY() || top + rowHeight > ImGui::GetScrollY() + ImGui::GetWindowHeight())
					ImGui::SetScrollY(std::max(top - (ImGui::GetWindowHeight() - rowHeight) * 0.5f, 0.0f));
			}
			m_ScrollTreeToFolder = false;
		}
		std::optional<std::pair<std::string, bool>> toggle;
		std::optional<std::string> open;
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(rows.size()));
		while (clipper.Step())
		{
			for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; index++)
			{
				const AssetBrowserFolderRow& row = rows[static_cast<size_t>(index)];
				ImGui::PushID(index);
				ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_NoTreePushOnOpen;
				if (!row.HasChildren)
					flags |= ImGuiTreeNodeFlags_Leaf;
				const bool current = row.Path == m_View.GetFolder();
				if (current)
					flags |= ImGuiTreeNodeFlags_Selected;
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent * static_cast<float>(row.Depth));
				ImGui::SetNextItemOpen(row.Expanded);
				UI::PushSelectionColors(current);
				const std::string& label = static_cast<size_t>(index) < m_FolderLabels.size() ? m_FolderLabels[static_cast<size_t>(index)] : row.Name;
				ImGui::TreeNodeEx("##Folder", flags, "%s", label.c_str());
				UI::PopSelectionColors();
				m_FolderRects.emplace_back(row.Path, ItemRect { UUID::Null(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax() });
				if (ImGui::IsItemToggledOpen())
					toggle = std::make_pair(row.Path, !row.Expanded);
				else if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
					open = row.Path;
				// Folders below the root drag like the grid's.
				if (!row.Path.empty() && ImGui::BeginDragDropSource())
				{
					ImGui::SetDragDropPayload(c_FolderPayload, row.Path.data(), row.Path.size());
					ImGui::TextUnformatted(label.c_str());
					ImGui::EndDragDropSource();
				}
				AcceptDrops(panelContext, row.Path);
				if (ImGui::BeginPopupContextItem("FolderMenu"))
				{
					DrawFolderMenu(panelContext, row.Path);
					ImGui::EndPopup();
				}
				ImGui::PopID();
			}
		}
		// Applied after the rows were drawn: they rebuild the rows.
		if (toggle)
			m_View.SetFolderExpanded(toggle->first, toggle->second);
		if (open)
			m_View.SetFolder(*open);
	}

	void ContentBrowserPanel::DrawItems(EditorPanelContext& panelContext)
	{
		if (m_View.GetItems().empty())
			DrawEmptyState();
		else if (m_Mode == ViewMode::Grid)
			DrawGrid(panelContext);
		else
			DrawList(panelContext);

		// The empty space of the folder: its menu, and drops into it.
		if (ImGui::BeginPopupContextWindow("ContentBrowser.Background", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
		{
			DrawBackgroundMenu(panelContext);
			ImGui::EndPopup();
		}
		// Drops between the items go into the folder shown (ImGui prefers the smallest target, so a folder's tile under the
		// mouse takes them first).
		ImGuiWindow* window = ImGui::GetCurrentWindow();
		if (ImGui::BeginDragDropTargetCustom(window->InnerRect, window->ID))
		{
			AcceptPayloads(panelContext, m_View.GetFolder());
			ImGui::EndDragDropTarget();
		}
		if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
		{
			m_Selected = UUID::Null();
			m_SelectedFolder.clear();
		}
	}

	void ContentBrowserPanel::DrawEmptyState()
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize()));
		const char* text = !m_View.GetSearch().empty() || !m_View.GetTypeFilter().empty() ? "Nothing matches the search or the filter"
			: "This folder is empty: drop files or folders onto the editor to import them here";
		UI::CenteredText("ContentBrowser.Empty", text, colors.TextSecondary);
	}

	void ContentBrowserPanel::DrawGrid(EditorPanelContext& panelContext)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const ImGuiStyle& style = ImGui::GetStyle();
		const std::vector<AssetBrowserItem>& items = m_View.GetItems();
		ThumbnailCache* thumbnails = panelContext.Context.GetThumbnails();
		const float titleHeight = ImGui::GetTextLineHeight();
		const float titleSpace = titleHeight + style.ItemInnerSpacing.y * 2.0f;
		// The size the slider picked, unless the browser is too short to show a whole tile with its name (a laptop's docked
		// panel): then tiles shrink to the height in view, down to c_MinFitTileScale. A tile's item takes the spacing too.
		const float picked = std::round(ImGui::GetFontSize() * std::clamp(m_TileScale, c_MinTileScale, c_MaxTileScale));
		const float smallest = std::round(ImGui::GetFontSize() * c_MinFitTileScale);
		const ImGuiWindow* pane = ImGui::GetCurrentWindow();
		const float paneHeight = pane->InnerRect.Max.y - (ImGui::GetCursorScreenPos().y + pane->Scroll.y);
		const float inView = std::floor(paneHeight - titleSpace - style.ItemSpacing.y);
		const float tile = std::max(smallest, std::min(picked, inView));
		const ImVec2 tileSize(tile, tile + titleSpace);
		const float spacing = style.ItemSpacing.x;
		const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / (tile + spacing)));
		const int rowCount = static_cast<int>((items.size() + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
		const float rowHeight = tileSize.y + style.ItemSpacing.y;

		if (m_ScrollToRevealed)
		{
			if (const std::optional<size_t> index = m_View.FindItem(m_Revealed))
			{
				const float row = static_cast<float>(*index / static_cast<size_t>(columns));
				ImGui::SetScrollY(std::max(row * rowHeight - (ImGui::GetWindowHeight() - rowHeight) * 0.5f, 0.0f));
			}
			m_ScrollToRevealed = false;
		}

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImGuiListClipper clipper;
		clipper.Begin(rowCount, rowHeight);
		while (clipper.Step())
		{
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
			{
				for (int column = 0; column < columns; column++)
				{
					const size_t index = static_cast<size_t>(row) * static_cast<size_t>(columns) + static_cast<size_t>(column);
					if (index >= items.size())
						break;
					if (column > 0)
						ImGui::SameLine(0.0f, spacing);
					const AssetBrowserItem& item = items[index];
					const bool selected = item.Kind == AssetBrowserItemKind::Folder ? item.Path == m_SelectedFolder
						: item.Handle == m_Selected || item.Handle == m_Revealed;
					ImGui::PushID(static_cast<int>(index));
					UI::PushSelectionColors(selected);
					ImGui::Selectable("##Tile", selected, ImGuiSelectableFlags_AllowDoubleClick, tileSize);
					UI::PopSelectionColors();
					const ImVec2 min = ImGui::GetItemRectMin();
					const ImVec2 max = ImGui::GetItemRectMax();
					const bool hovered = ImGui::IsItemHovered();
					HandleItem(panelContext, index);

					// The square: sub-assets on a tint, so they read as part of the model before them.
					const ImVec2 squareMin(min.x + style.ItemInnerSpacing.x, min.y + style.ItemInnerSpacing.y);
					const ImVec2 squareMax(max.x - style.ItemInnerSpacing.x, max.y - titleHeight - style.ItemInnerSpacing.y * 2.0f);
					drawList->AddRectFilled(squareMin, squareMax, UI::ToColorU32(item.Kind == AssetBrowserItemKind::SubAsset ? colors.AccentMuted : colors.Raised,
						item.Kind == AssetBrowserItemKind::SubAsset ? 0.55f : 1.0f), style.FrameRounding);
					// The asset's thumbnail once it is ready (asked for while the tile is on screen), else its type's icon.
					nvrhi::ITexture* thumbnail = item.Kind != AssetBrowserItemKind::Folder && thumbnails ? thumbnails->Request(item.Handle) : nullptr;
					if (thumbnail)
					{
						const nvrhi::TextureDesc& desc = thumbnail->getDesc();
						const float side = std::min(squareMax.x - squareMin.x, squareMax.y - squareMin.y);
						const float scale = side / static_cast<float>(std::max(desc.width, desc.height));
						const ImVec2 size(std::round(static_cast<float>(desc.width) * scale), std::round(static_cast<float>(desc.height) * scale));
						const ImVec2 imageMin(std::round((squareMin.x + squareMax.x - size.x) * 0.5f), std::round((squareMin.y + squareMax.y - size.y) * 0.5f));
						// Display colors, drawn as they are (ImGuiTextureDisplay::Direct).
						drawList->AddImage(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(thumbnail)), imageMin, ImVec2(imageMin.x + size.x, imageMin.y + size.y));
					}
					else
					{
						const ImVec4 iconColor = !item.ImportError.empty() ? colors.Error : item.Kind == AssetBrowserItemKind::Folder ? colors.Accent : colors.TextSecondary;
						UI::PushFont(UI::EditorFont::Regular, UI::TextSize::Display);
						const char* icon = m_Labels[index].Icon.c_str();
						const ImVec2 iconSize = ImGui::CalcTextSize(icon);
						drawList->AddText(ImVec2((squareMin.x + squareMax.x - iconSize.x) * 0.5f, (squareMin.y + squareMax.y - iconSize.y) * 0.5f),
							UI::ToColorU32(iconColor, item.Importing ? 0.45f : 1.0f), icon);
						ImGui::PopFont();
					}

					// Badges: importing, a failed import, warnings; the model's expander.
					const float badge = ImGui::GetFontSize();
					const ImVec2 badgePosition(squareMax.x - badge - style.ItemInnerSpacing.x * 0.5f, squareMin.y + style.ItemInnerSpacing.y * 0.5f);
					if (item.Importing)
						drawList->AddText(badgePosition, UI::ToColorU32(colors.Info), Icons::Hourglass);
					else if (!item.ImportError.empty())
						drawList->AddText(badgePosition, UI::ToColorU32(colors.Error), Icons::CircleAlert);
					else if (item.WarningCount > 0)
						drawList->AddText(badgePosition, UI::ToColorU32(colors.Warning), Icons::TriangleAlert);
					if (item.HasSubAssets)
					{
						// An expander drawn in the tile (no item of its own, which would break the row's layout): a click on it
						// shows or hides the model's sub-assets.
						const ImVec2 expanderMin(squareMax.x - ImGui::GetFrameHeight(), squareMax.y - ImGui::GetFrameHeight());
						const bool overExpander = hovered && ImGui::IsMouseHoveringRect(expanderMin, squareMax);
						drawList->AddRectFilled(expanderMin, squareMax, UI::ToColorU32(overExpander ? colors.AccentMuted : colors.Control), style.FrameRounding);
						const char* arrow = item.Expanded ? Icons::ChevronLeft : Icons::ChevronRight;
						const ImVec2 arrowSize = ImGui::CalcTextSize(arrow);
						drawList->AddText(ImVec2((expanderMin.x + squareMax.x - arrowSize.x) * 0.5f, (expanderMin.y + squareMax.y - arrowSize.y) * 0.5f),
							UI::ToColorU32(colors.Text), arrow);
						if (overExpander && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
							m_View.SetAssetExpanded(item.Handle, !item.Expanded);
					}

					// The title under the square, cut to its width.
					const std::string& title = GetTileTitle(index, tile - style.ItemInnerSpacing.x * 2.0f);
					const float titleWidth = ImGui::CalcTextSize(title.c_str()).x;
					drawList->AddText(ImVec2(min.x + (tile - titleWidth) * 0.5f, max.y - titleHeight - style.ItemInnerSpacing.y),
						UI::ToColorU32(!item.ImportError.empty() ? colors.Error : colors.Text), title.c_str());

					if (item.Kind == AssetBrowserItemKind::Folder)
						m_FolderRects.emplace_back(item.Path, ItemRect { UUID::Null(), min, max });
					else
						m_ItemRects.push_back({ item.Handle, min, max });
					ImGui::PopID();
				}
			}
		}
		clipper.End();
	}

	void ContentBrowserPanel::DrawList(EditorPanelContext& panelContext)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		const std::vector<AssetBrowserItem>& items = m_View.GetItems();
		const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
		if (m_ScrollToRevealed)
		{
			if (const std::optional<size_t> index = m_View.FindItem(m_Revealed))
				ImGui::SetScrollY(std::max(static_cast<float>(*index) * rowHeight - (ImGui::GetWindowHeight() - rowHeight) * 0.5f, 0.0f));
			m_ScrollToRevealed = false;
		}
		const float typeColumn = ImGui::GetFontSize() * 8.0f;
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(items.size()), rowHeight);
		while (clipper.Step())
		{
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
			{
				const size_t index = static_cast<size_t>(row);
				const AssetBrowserItem& item = items[index];
				const bool selected = item.Kind == AssetBrowserItemKind::Folder ? item.Path == m_SelectedFolder
					: item.Handle == m_Selected || item.Handle == m_Revealed;
				ImGui::PushID(row);
				const float rowRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetStyle().IndentSpacing * static_cast<float>(item.Depth));
				if (!item.ImportError.empty())
					ImGui::PushStyleColor(ImGuiCol_Text, colors.Error);
				UI::PushSelectionColors(selected);
				ImGui::Selectable(m_Labels[index].ListName.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap);
				UI::PopSelectionColors();
				if (!item.ImportError.empty())
					ImGui::PopStyleColor();
				const ImVec2 min = ImGui::GetItemRectMin();
				const ImVec2 max = ImGui::GetItemRectMax();
				HandleItem(panelContext, index);
				// The type at the right, and what the import is doing.
				ImGui::SameLine(std::max(rowRight - typeColumn, ImGui::GetCursorPosX()));
				const char* state = item.Importing ? "Importing..." : m_Labels[index].TypeName.c_str();
				ImGui::TextColored(item.Importing ? colors.Info : colors.TextSecondary, "%s", state);
				if (item.HasSubAssets)
				{
					ImGui::SameLine();
					if (UI::IconButton("Expand", item.Expanded ? Icons::ChevronDown : Icons::ChevronRight,
						item.Expanded ? "Hide the meshes, materials and textures" : "Show the meshes, materials and textures it brings"))
						m_View.SetAssetExpanded(item.Handle, !item.Expanded);
				}
				if (item.Kind == AssetBrowserItemKind::Folder)
					m_FolderRects.emplace_back(item.Path, ItemRect { UUID::Null(), min, max });
				else
					m_ItemRects.push_back({ item.Handle, min, max });
				ImGui::PopID();
			}
		}
		clipper.End();
	}

	void ContentBrowserPanel::HandleItem(EditorPanelContext& panelContext, size_t index)
	{
		EditorContext& context = panelContext.Context;
		const AssetBrowserItem& item = m_View.GetItems()[index];
		const ItemLabels& labels = m_Labels[index];
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
			Select(context, item);
		if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			Open(panelContext, item);

		if (ImGui::BeginDragDropSource())
		{
			if (item.Kind == AssetBrowserItemKind::Folder)
			{
				ImGui::SetDragDropPayload(c_FolderPayload, item.Path.data(), item.Path.size());
				ImGui::TextUnformatted(labels.ListName.c_str());
			}
			else
			{
				SetAssetPayload(item.Handle, labels.ListName);
			}
			ImGui::EndDragDropSource();
		}
		if (item.Kind == AssetBrowserItemKind::Folder)
			AcceptDrops(panelContext, item.Path);

		if (UI::IsTooltipDue())
		{
			std::string tooltip = item.Kind == AssetBrowserItemKind::Folder ? item.Path : fmt::format("{}\n{}", item.Path, labels.TypeName);
			if (item.Kind == AssetBrowserItemKind::SubAsset)
				tooltip = fmt::format("{} ({})\nimported with {}", item.Name, labels.TypeName, item.Path);
			if (item.Importing)
				tooltip += "\nImporting...";
			if (!item.ImportError.empty())
				tooltip += fmt::format("\nImport failed: {}", item.ImportError);
			else if (item.WarningCount > 0)
				tooltip += fmt::format("\n{} import {} (the Asset tab lists them)", item.WarningCount, item.WarningCount == 1 ? "warning" : "warnings");
			ImGui::SetTooltip("%s", tooltip.c_str());
		}

		if (ImGui::BeginPopupContextItem("ItemMenu"))
		{
			if (item.Kind == AssetBrowserItemKind::Folder)
				DrawFolderMenu(panelContext, item.Path);
			else
				DrawItemMenu(panelContext, item);
			ImGui::EndPopup();
		}
	}

	void ContentBrowserPanel::DrawItemMenu(EditorPanelContext& panelContext, const AssetBrowserItem& item)
	{
		EditorContext& context = panelContext.Context;
		const EditorCommandRegistry& commands = panelContext.Commands;
		const nlohmann::json asset = UUIDToJson(item.Handle);
		if (item.Type == AssetType::Scene && ImGui::MenuItem("Open"))
			Open(panelContext, item);
		if ((item.Type == AssetType::Prefab || item.Type == AssetType::Model) && ImGui::MenuItem("Add to Scene"))
			RunEditorCommand(context, commands, "prefab.instantiate", { { "prefab", asset } });
		if (ImGui::MenuItem("Show in Asset Tab"))
		{
			m_InspectedFromHere = item.Handle;
			context.SetInspectedAsset(item.Handle);
		}
		if (ImGui::MenuItem("Copy Path"))
			ImGui::SetClipboardText(item.Path.c_str());
		if (item.Kind == AssetBrowserItemKind::SubAsset)
			return;
		ImGui::Separator();
		if (ImGui::MenuItem("Rename", "F2"))
			BeginRename(item);
		// Imports run in the background: the command answers frames later.
		if (EditorAssetManager* assets = context.GetAssetManager(); assets && assets->GetImporter(item.Handle) && !assets->GetImporter(item.Handle)->StoresSourceDirectly()
			&& ImGui::MenuItem("Reimport"))
		{
			panelContext.Runner.Run(context, commands, "asset.reimport", { { "asset", asset } }, [](const EditorCommandResult& result)
			{
				if (!result.Success)
					ST_ERROR("Reimport: {}", result.Error);
			});
		}
		ImGui::Separator();
		if (ImGui::MenuItem("Delete...", "Delete"))
			m_DeleteDialog.OpenForAsset(context, item.Handle);
	}

	void ContentBrowserPanel::DrawFolderMenu(EditorPanelContext& panelContext, const std::string& folder)
	{
		if (ImGui::MenuItem("Open"))
		{
			m_View.SetFolder(folder);
			m_View.ExpandTo(folder);
		}
		if (folder.empty())
			return;
		ImGui::Separator();
		if (ImGui::MenuItem("Rename", "F2"))
		{
			AssetBrowserItem item;
			item.Kind = AssetBrowserItemKind::Folder;
			item.Name = AssetBrowserView::GetFileName(folder);
			item.Path = folder;
			BeginRename(item);
		}
		if (ImGui::MenuItem("Copy Path"))
			ImGui::SetClipboardText(folder.c_str());
		ImGui::Separator();
		if (ImGui::MenuItem("Delete Folder...", "Delete"))
			m_DeleteDialog.OpenForFolder(panelContext.Context, folder);
	}

	void ContentBrowserPanel::DrawBackgroundMenu(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		if (ImGui::MenuItem((std::string(Icons::FolderPlus) + "  New Folder").c_str()))
			CreateFolder(panelContext);
		if (ImGui::MenuItem((std::string(Icons::Palette) + "  New Material").c_str()))
		{
			if (EditorAssetManager* assets = context.GetAssetManager())
			{
				RunEditorCommand(context, panelContext.Commands, "material.create",
					{ { "path", CommandUtils::MakeUniqueAssetPath(*assets, m_View.GetFolder(), "Material", ".stmat", "Material") } });
			}
		}
		ImGui::Separator();
		if (ImGui::MenuItem((std::string(Icons::FileUp) + "  Import Files...").c_str()))
		{
			const std::vector<std::filesystem::path> files = FileDialogs::OpenFiles({});
			if (!files.empty())
				ImportFiles(context, panelContext.Commands, panelContext.Runner, files);
		}
		if (ImGui::MenuItem((std::string(Icons::FolderInput) + "  Import Folder...").c_str()))
		{
			if (const std::optional<std::filesystem::path> folder = FileDialogs::PickFolder())
				ImportFiles(context, panelContext.Commands, panelContext.Runner, { *folder });
		}
		if (ImGui::MenuItem((std::string(Icons::RefreshCw) + "  Rescan Folders").c_str()))
		{
			if (EditorAssetManager* assets = context.GetAssetManager())
				assets->RescanFolders();
		}
	}

	void ContentBrowserPanel::HandleShortcuts(EditorPanelContext& panelContext)
	{
		// Only while the browser has the keyboard focus and no text field takes the keys.
		if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) || ImGui::GetIO().WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
			return;
		const AssetBrowserItem* selected = nullptr;
		for (const AssetBrowserItem& item : m_View.GetItems())
		{
			if ((item.Kind == AssetBrowserItemKind::Folder && !m_SelectedFolder.empty() && item.Path == m_SelectedFolder)
				|| (item.Kind != AssetBrowserItemKind::Folder && m_Selected.IsValid() && item.Handle == m_Selected))
			{
				selected = &item;
				break;
			}
		}
		if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && !m_View.GetFolder().empty())
			m_View.SetFolder(AssetBrowserView::GetParentFolder(m_View.GetFolder()));
		if (!selected)
			return;
		if (ImGui::IsKeyPressed(ImGuiKey_Delete))
		{
			if (selected->Kind == AssetBrowserItemKind::Folder)
				m_DeleteDialog.OpenForFolder(panelContext.Context, selected->Path);
			else if (selected->Kind == AssetBrowserItemKind::Asset)
				m_DeleteDialog.OpenForAsset(panelContext.Context, selected->Handle);
		}
		else if (ImGui::IsKeyPressed(ImGuiKey_F2))
		{
			BeginRename(*selected);
		}
		else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
		{
			const AssetBrowserItem item = *selected;
			Open(panelContext, item);
		}
	}

	void ContentBrowserPanel::DrawRenamePopup(EditorPanelContext& panelContext)
	{
		if (m_OpenRename)
		{
			ImGui::OpenPopup("ContentBrowser.Rename");
			m_OpenRename = false;
		}
		if (!ImGui::BeginPopup("ContentBrowser.Rename"))
			return;
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
		const bool submitted = ImGui::InputText("##Name", &m_RenameText, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		ImGui::SameLine();
		if ((UI::DialogButton("ContentBrowser.RenameConfirm", "Rename", true, !m_RenameText.empty()) || submitted) && !m_RenameText.empty())
		{
			EditorContext& context = panelContext.Context;
			if (m_RenameAsset.IsValid())
			{
				if (const std::optional<AssetMetadata> metadata = context.GetAssetManager() ? context.GetAssetManager()->GetMetadata(m_RenameAsset) : std::nullopt)
				{
					RunEditorCommand(context, panelContext.Commands, "asset.move", { { "asset", UUIDToJson(m_RenameAsset) },
						{ "path", JoinPath(AssetBrowserView::GetParentFolder(metadata->Path), m_RenameText) } });
				}
			}
			else if (!m_RenameFolder.empty())
			{
				const std::string destination = JoinPath(AssetBrowserView::GetParentFolder(m_RenameFolder), m_RenameText);
				if (destination != m_RenameFolder && !RunEditorCommand(context, panelContext.Commands, "asset.moveFolder", { { "path", m_RenameFolder },
					{ "to", destination } }).is_null())
				{
					if (m_SelectedFolder == m_RenameFolder)
						m_SelectedFolder = destination;
				}
			}
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

}
