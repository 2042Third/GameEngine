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

		// The most lines of references the dialog shows at once; more scroll.
		constexpr size_t c_VisibleReferenceLines = 12;

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
		DescribeReferencers();
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
		DescribeReferencers();
		m_OpenRequested = true;
	}

	void DeleteAssetDialog::DescribeReferencers()
	{
		m_ReferenceLines.clear();
		m_ReferenceLines.reserve(m_Referencers.size());
		for (const AssetReferencer& referencer : m_Referencers)
		{
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
			m_ReferenceLines.push_back(std::move(line));
		}
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
			ImGui::PopTextWrapPos();
			// Every one of them, so that what breaks is known before "Delete Anyway": a list that scrolls beyond a dozen lines.
			const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
			const float listHeight = lineHeight * static_cast<float>(std::min(m_ReferenceLines.size(), c_VisibleReferenceLines)) + ImGui::GetStyle().WindowPadding.y * 2.0f;
			if (ImGui::BeginChild("DeleteAsset.References", ImVec2(wrap, listHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar))
			{
				ImGuiListClipper clipper;
				clipper.Begin(static_cast<int>(m_ReferenceLines.size()), lineHeight);
				while (clipper.Step())
				{
					for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; line++)
						ImGui::BulletText("%s", m_ReferenceLines[static_cast<size_t>(line)].c_str());
				}
			}
			ImGui::EndChild();
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
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
		// Escape cancels too: ImGui leaves modals open on it.
		if (UI::DialogButton("DeleteAsset.Cancel", "Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			ImGui::CloseCurrentPopup();
			m_Open = false;
		}
		UI::EndModal();
	}

}
