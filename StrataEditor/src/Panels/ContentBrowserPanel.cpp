#include "Panels/ContentBrowserPanel.h"

#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Panels/SceneHierarchyPanel.h"
#include "UI/FileDialogs.h"
#include "UI/PropertyWidgets.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Reflection/PropertyJson.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>

namespace Strata
{

	namespace
	{

		constexpr double c_RefreshInterval = 0.5; // Seconds between listings of the folder

		std::string JoinPath(const std::string& directory, const std::string& name)
		{
			return directory.empty() ? name : directory + "/" + name;
		}

		std::string GetParentDirectory(const std::string& path)
		{
			const size_t slash = path.rfind('/');
			return slash == std::string::npos ? std::string() : path.substr(0, slash);
		}

		std::string JoinLines(const std::vector<std::string>& lines)
		{
			std::string text;
			for (const std::string& line : lines)
			{
				if (!text.empty())
					text += '\n';
				text += line;
			}
			return text;
		}

		const char* GetTypeTag(AssetType type)
		{
			switch (type)
			{
				case AssetType::Scene:     return "[Scene]";
				case AssetType::Prefab:    return "[Prefab]";
				case AssetType::Model:     return "[Model]";
				case AssetType::Mesh:      return "[Mesh]";
				case AssetType::Material:  return "[Material]";
				case AssetType::Texture:   return "[Texture]";
				case AssetType::AudioClip: return "[Audio]";
				case AssetType::Font:      return "[Font]";
				default:                   return "[Asset]";
			}
		}

	}

	void ContentBrowserPanel::ImportFiles(EditorContext& context, const EditorCommandRegistry& commands, const std::vector<std::filesystem::path>& files)
	{
		for (const std::filesystem::path& file : files)
		{
			if (FileSystem::IsDirectory(file))
			{
				ST_WARN("Importing folders is not supported; drop the files instead ({})", FileSystem::ToUTF8(file));
				continue;
			}
			RunEditorCommand(context, commands, "asset.import", { { "file", FileSystem::ToUTF8(file) }, { "directory", m_CurrentDirectory } });
		}
		m_RefreshRequested = true;
	}

	void ContentBrowserPanel::OpenScene(EditorContext& context, const EditorCommandRegistry& commands, AssetHandle scene)
	{
		if (m_OpenScene)
			m_OpenScene(scene);
		else
			RunEditorCommand(context, commands, "scene.open", { { "scene", UUIDToJson(scene) } });
	}

	void ContentBrowserPanel::Refresh(EditorContext& context)
	{
		m_Folders.clear();
		m_Items.clear();
		EditorAssetManager* assets = context.GetAssetManager();
		const std::filesystem::path root = context.GetProject()->GetAssetDirectory();
		if (!m_CurrentDirectory.empty() && !FileSystem::IsDirectory(root / FileSystem::FromUTF8(m_CurrentDirectory)))
			m_CurrentDirectory.clear(); // Deleted or renamed outside the editor

		const std::string filter = StringUtils::ToLower(m_Filter);
		std::error_code error;
		for (std::filesystem::directory_iterator it(root / FileSystem::FromUTF8(m_CurrentDirectory), error), end; !error && it != end; it.increment(error))
		{
			const std::string name = FileSystem::ToUTF8(it->path().filename());
			std::error_code typeError;
			// Hidden folders are never assets.
			if (name.empty() || name[0] == '.' || !it->is_directory(typeError))
				continue;
			if (filter.empty() || StringUtils::ToLower(name).find(filter) != std::string::npos)
				m_Folders.push_back(name);
		}
		std::sort(m_Folders.begin(), m_Folders.end());

		for (AssetMetadata& metadata : assets->GetAllMetadata())
		{
			if (metadata.IsBuiltin() || metadata.IsSubAsset() || GetParentDirectory(metadata.Path) != m_CurrentDirectory)
				continue;
			if (!filter.empty() && StringUtils::ToLower(metadata.Path).find(filter) == std::string::npos)
				continue;
			Item item;
			item.FileName = FileSystem::ToUTF8(FileSystem::FromUTF8(metadata.Path).filename());
			item.Label = fmt::format("{}  {}", GetTypeTag(metadata.Type), item.FileName);
			const AssetImportInfo importInfo = assets->GetImportInfo(metadata.Handle);
			item.ImportError = importInfo.Error;
			item.ImportWarnings = JoinLines(importInfo.Warnings);
			item.Metadata = std::move(metadata);
			m_Items.push_back(std::move(item));
		}
		std::sort(m_Items.begin(), m_Items.end(), [](const Item& a, const Item& b) { return a.Metadata.Path < b.Metadata.Path; });

		m_LastRefreshTime = ImGui::GetTime();
		m_RefreshRequested = false;
	}

	void ContentBrowserPanel::DrawToolbar(EditorContext& context, const EditorCommandRegistry& commands)
	{
		ImGui::BeginDisabled(m_CurrentDirectory.empty());
		if (ImGui::Button("Up"))
		{
			m_CurrentDirectory = GetParentDirectory(m_CurrentDirectory);
			m_RefreshRequested = true;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextUnformatted(m_CurrentDirectory.empty() ? "Assets" : ("Assets/" + m_CurrentDirectory).c_str());

		ImGui::SameLine();
		if (ImGui::Button("Import..."))
		{
			if (std::optional<std::filesystem::path> file = FileDialogs::OpenFile({}))
				ImportFiles(context, commands, { *file });
		}
		ImGui::SameLine();
		if (ImGui::Button("New..."))
			ImGui::OpenPopup("NewAsset");
		if (ImGui::BeginPopup("NewAsset"))
		{
			if (ImGui::BeginMenu("Folder"))
			{
				ImGui::InputTextWithHint("##Folder", "Name", &m_NewFolderName);
				if (ImGui::Button("Create") && !m_NewFolderName.empty())
				{
					const std::filesystem::path assetDirectory = context.GetProject()->GetAssetDirectory();
					const std::filesystem::path directory = (assetDirectory / FileSystem::FromUTF8(JoinPath(m_CurrentDirectory, m_NewFolderName))).lexically_normal();
					if (!FileSystem::IsInside(directory, assetDirectory) || !FileSystem::CreateDirectories(directory))
						ST_ERROR("Could not create the folder '{}'", m_NewFolderName);
					m_NewFolderName.clear();
					m_RefreshRequested = true;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndMenu();
			}
			if (ImGui::MenuItem("Material"))
			{
				const std::filesystem::path path = FileSystem::GetUniquePath(context.GetProject()->GetAssetDirectory() / FileSystem::FromUTF8(JoinPath(m_CurrentDirectory, "Material.stmat")));
				RunEditorCommand(context, commands, "material.create", { { "path", JoinPath(m_CurrentDirectory, FileSystem::ToUTF8(path.filename())) } });
				m_RefreshRequested = true;
			}
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x, 60.0f));
		if (ImGui::InputTextWithHint("##Filter", "Search", &m_Filter))
			m_RefreshRequested = true;
	}

	void ContentBrowserPanel::DrawRenamePopup(EditorContext& context, const EditorCommandRegistry& commands)
	{
		if (!ImGui::BeginPopup("RenameAsset"))
			return;
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		const bool submitted = ImGui::InputText("##Name", &m_RenameText, ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if ((ImGui::Button("Rename") || submitted) && !m_RenameText.empty())
		{
			RunEditorCommand(context, commands, "asset.move", { { "asset", UUIDToJson(m_RenameAsset) }, { "path", JoinPath(m_CurrentDirectory, m_RenameText) } });
			m_RefreshRequested = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void ContentBrowserPanel::OnImGuiRender(EditorPanelContext& panelContext)
	{
		EditorContext& context = panelContext.Context;
		const EditorCommandRegistry& commands = panelContext.Commands;
		if (!context.GetAssetManager())
		{
			ImGui::TextDisabled("Open or create a project to manage its assets (File menu)");
			m_RefreshRequested = true;
			return;
		}

		DrawToolbar(context, commands);
		if (m_RefreshRequested || ImGui::GetTime() - m_LastRefreshTime > c_RefreshInterval)
			Refresh(context);
		ImGui::Separator();

		if (ImGui::BeginChild("Items"))
		{
			std::string enterFolder;
			for (const std::string& folder : m_Folders)
			{
				ImGui::Selectable(fmt::format("[Folder]  {}", folder).c_str(), false, ImGuiSelectableFlags_AllowDoubleClick);
				if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
					enterFolder = folder;
			}

			AssetHandle openScene = UUID::Null();
			for (const Item& item : m_Items)
			{
				const AssetMetadata& metadata = item.Metadata;
				ImGui::PushID(metadata.Handle.ToString().c_str());
				const bool failed = !item.ImportError.empty();
				if (failed)
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.35f, 1.0f));
				ImGui::Selectable(item.Label.c_str(), metadata.Handle == context.GetSceneHandle(), ImGuiSelectableFlags_AllowDoubleClick);
				if (failed)
					ImGui::PopStyleColor();
				if (ImGui::IsItemHovered())
				{
					if (failed)
						ImGui::SetTooltip("Import failed: %s", item.ImportError.c_str());
					else if (!item.ImportWarnings.empty())
						ImGui::SetTooltip("%s", item.ImportWarnings.c_str());
					if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && metadata.Type == AssetType::Scene)
						openScene = metadata.Handle;
				}
				if (ImGui::BeginDragDropSource())
				{
					ImGui::SetDragDropPayload(DragDrop::c_Asset, &metadata.Handle, sizeof(AssetHandle));
					ImGui::TextUnformatted(item.FileName.c_str());
					ImGui::EndDragDropSource();
				}

				bool openRename = false;
				if (ImGui::BeginPopupContextItem())
				{
					if ((metadata.Type == AssetType::Prefab || metadata.Type == AssetType::Model) && ImGui::MenuItem("Add to Scene"))
						RunEditorCommand(context, commands, "prefab.instantiate", { { "prefab", UUIDToJson(metadata.Handle) } });
					if (metadata.Type == AssetType::Scene && ImGui::MenuItem("Open"))
						openScene = metadata.Handle;
					if (ImGui::MenuItem("Rename"))
						openRename = true;
					if (ImGui::MenuItem("Reimport"))
						RunEditorCommand(context, commands, "asset.reimport", { { "asset", UUIDToJson(metadata.Handle) } });
					ImGui::Separator();
					if (ImGui::MenuItem("Delete"))
					{
						RunEditorCommand(context, commands, "asset.delete", { { "asset", UUIDToJson(metadata.Handle) } });
						m_RefreshRequested = true;
					}
					ImGui::EndPopup();
				}
				ImGui::PopID();
				if (openRename)
				{
					m_RenameAsset = metadata.Handle;
					m_RenameText = item.FileName;
					ImGui::OpenPopup("RenameAsset");
				}
			}
			if (m_Folders.empty() && m_Items.empty())
				ImGui::TextDisabled(m_Filter.empty() ? "Empty folder: drop files onto the editor window to import them" : "Nothing matches");
			DrawRenamePopup(context, commands);

			// Applied after the listing was drawn.
			if (!enterFolder.empty())
			{
				m_CurrentDirectory = JoinPath(m_CurrentDirectory, enterFolder);
				m_RefreshRequested = true;
			}
			if (openScene.IsValid())
				OpenScene(context, commands, openScene);
		}
		ImGui::EndChild();
	}

}
