#include "UI/DeleteAssetDialog.h"

#include "Editor/EditorContext.h"
#include "UI/EditorActions.h"
#include "UI/Icons.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Reflection/PropertyJson.h>

#include <imgui.h>

#include <algorithm>

namespace Strata
{

	namespace
	{

		// The references the dialog lists before it sums up the rest.
		constexpr size_t c_ListedReferencers = 8;

		std::string GetFileName(const std::string& path)
		{
			const size_t slash = path.rfind('/');
			return slash == std::string::npos ? path : path.substr(slash + 1);
		}

	}

	void DeleteAssetDialog::OpenForAsset(EditorContext& context, AssetHandle asset)
	{
		const EditorAssetManager* assets = context.GetAssetManager();
		const std::optional<AssetMetadata> metadata = assets ? assets->GetMetadata(asset) : std::nullopt;
		if (!metadata)
			return;
		m_Folder = false;
		m_Asset = asset;
		m_Path = metadata->Path;
		m_AssetCount = 1;
		const std::vector<AssetHandle> targets = AssetReferences::WithSubAssets(context, { asset });
		m_Referencers = AssetReferences::Find(context, targets, { targets.begin(), targets.end() });
		m_OpenRequested = true;
	}

	void DeleteAssetDialog::OpenForFolder(EditorContext& context, const std::string& folder)
	{
		const EditorAssetManager* assets = context.GetAssetManager();
		if (!assets || folder.empty())
			return;
		m_Folder = true;
		m_Asset = UUID::Null();
		m_Path = folder;
		const std::vector<AssetHandle> contained = assets->GetAssetsBelowFolder(folder);
		m_AssetCount = contained.size();
		const std::vector<AssetHandle> targets = AssetReferences::WithSubAssets(context, contained);
		m_Referencers = AssetReferences::Find(context, targets, { targets.begin(), targets.end() });
		m_OpenRequested = true;
	}

	void DeleteAssetDialog::Draw(EditorContext& context, const EditorCommandRegistry& commands)
	{
		if (m_OpenRequested)
		{
			UI::OpenModal(c_PopupName);
			m_OpenRequested = false;
		}
		bool open = true;
		m_Open = UI::BeginModal(c_PopupName, m_Folder ? "Delete Folder" : "Delete Asset", &open);
		if (!m_Open)
			return;
		if (!open)
		{
			ImGui::CloseCurrentPopup();
			UI::EndModal();
			m_Open = false;
			return;
		}

		const UI::ThemeColors& colors = UI::GetThemeColors();
		const float wrap = ImGui::GetFontSize() * 30.0f;
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
		if (m_Folder)
			ImGui::TextUnformatted(fmt::format("Delete the folder '{}' with its {} {}?", m_Path, m_AssetCount, m_AssetCount == 1 ? "asset" : "assets").c_str());
		else
			ImGui::TextUnformatted(fmt::format("Delete '{}'?", GetFileName(m_Path)).c_str());
		ImGui::PushStyleColor(ImGuiCol_Text, colors.TextSecondary);
		ImGui::TextUnformatted("It moves to the project's trash (.strata/Trash): Edit > Undo brings it back with its references.");
		ImGui::PopStyleColor();

		if (!m_Referencers.empty())
		{
			ImGui::Spacing();
			ImGui::PushStyleColor(ImGuiCol_Text, colors.Warning);
			ImGui::TextUnformatted(fmt::format("{}  Used by {} {}: deleting leaves {} references missing.", Icons::TriangleAlert, m_Referencers.size(),
				m_Referencers.size() == 1 ? "asset or scene" : "assets or scenes", m_Referencers.size() == 1 ? "its" : "their").c_str());
			ImGui::PopStyleColor();
			const size_t listed = std::min(m_Referencers.size(), c_ListedReferencers);
			for (size_t index = 0; index < listed; index++)
			{
				const AssetReferencer& referencer = m_Referencers[index];
				std::string line = referencer.Path.empty() ? std::string("The open scene (not saved yet)") : referencer.Path;
				if (!referencer.Name.empty())
					line += fmt::format(" ({})", referencer.Name);
				if (referencer.OpenScene && !referencer.Entities.empty())
				{
					std::string names;
					for (size_t entity = 0; entity < std::min<size_t>(referencer.Entities.size(), 3); entity++)
						names += (names.empty() ? "" : ", ") + referencer.Entities[entity].Name;
					if (referencer.Entities.size() > 3)
						names += fmt::format(" and {} more", referencer.Entities.size() - 3);
					line += fmt::format(" - open: {}", names);
				}
				ImGui::BulletText("%s", line.c_str());
			}
			if (m_Referencers.size() > listed)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, colors.TextSecondary);
				ImGui::TextUnformatted(fmt::format("and {} more (asset.findReferences lists them)", m_Referencers.size() - listed).c_str());
				ImGui::PopStyleColor();
			}
		}
		ImGui::PopTextWrapPos();
		ImGui::Spacing();

		const bool referenced = !m_Referencers.empty();
		if (UI::DialogButton("DeleteAsset.Confirm", referenced ? "Delete Anyway" : "Delete", true))
		{
			nlohmann::json parameters = m_Folder ? nlohmann::json { { "path", m_Path } } : nlohmann::json { { "asset", UUIDToJson(m_Asset) } };
			if (referenced)
				parameters["force"] = true;
			RunEditorCommand(context, commands, m_Folder ? "asset.deleteFolder" : "asset.delete", parameters);
			ImGui::CloseCurrentPopup();
			m_Open = false;
		}
		ImGui::SameLine();
		if (UI::DialogButton("DeleteAsset.Cancel", "Cancel"))
		{
			ImGui::CloseCurrentPopup();
			m_Open = false;
		}
		UI::EndModal();
	}

}
