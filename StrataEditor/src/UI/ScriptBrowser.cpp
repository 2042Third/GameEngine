#include "UI/ScriptBrowser.h"

#include "Editor/EditorContext.h"
#include "UI/EditorShell.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Project/Project.h>

#include <imgui.h>

#include <algorithm>
#include <cctype>

namespace Strata
{

	namespace
	{

		// The Classes column's share of the table's width.
		constexpr float c_ClassesColumnWeight = 0.35f;

		constexpr const char* c_NewScriptLabel = "New Script...";
		constexpr const char* c_OpenInIDELabel = "Open in IDE";

		const char* GetFileIcon(std::string_view path)
		{
			return path.ends_with("CMakeLists.txt") || path.ends_with(".cmake") ? Icons::FileCog : Icons::FileCode;
		}

		// The words of a filter, lowered.
		std::vector<std::string> SplitWords(std::string_view filter)
		{
			std::vector<std::string> words;
			size_t position = 0;
			while (position < filter.size())
			{
				while (position < filter.size() && std::isspace(static_cast<unsigned char>(filter[position])))
					position++;
				size_t end = position;
				while (end < filter.size() && !std::isspace(static_cast<unsigned char>(filter[end])))
					end++;
				if (end > position)
					words.push_back(StringUtils::ToLower(filter.substr(position, end - position)));
				position = end;
			}
			return words;
		}

		float CalcToolbarButtonWidth(const char* icon, const char* label)
		{
			const ImGuiStyle& style = ImGui::GetStyle();
			return style.FramePadding.x * 2.0f + ImGui::CalcTextSize(icon).x + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label).x;
		}

	}

	void ScriptBrowser::DrawActions(EditorPanelContext& context)
	{
		const bool project = context.Context.HasProject();
		if (UI::ToolbarButton("ScriptBrowser.NewScript", Icons::FilePlus, c_NewScriptLabel, "Write a new script class into the project's Scripts folder",
			UI::ButtonStyle { .Enabled = project && context.Shell != nullptr }))
		{
			context.Shell->ShowNewScriptDialog();
			RequestRefresh();
		}
		ImGui::SameLine();
		if (UI::ToolbarButton("ScriptBrowser.OpenInIDE", Icons::ExternalLink, c_OpenInIDELabel,
			"Open the Scripts folder in your code editor (Scripts > External Editor... sets which one)", UI::ButtonStyle { .Enabled = project }))
		{
			m_LastOpenError.clear();
			if (!context.Context.OpenScriptSource({}, 0, 0, nullptr, &m_LastOpenError))
				ST_ERROR("Scripts: {}", m_LastOpenError);
		}
	}

	float ScriptBrowser::CalcActionsWidth()
	{
		return CalcToolbarButtonWidth(Icons::FilePlus, c_NewScriptLabel) + ImGui::GetStyle().ItemSpacing.x
			+ CalcToolbarButtonWidth(Icons::ExternalLink, c_OpenInIDELabel);
	}

	void ScriptBrowser::Draw(EditorPanelContext& context, std::string_view filter)
	{
		m_Rects.clear();
		const Ref<Project>& project = context.Context.GetProject();
		if (!project)
		{
			if (!m_List.GetDirectory().empty())
				m_List.Clear();
			ImGui::TextDisabled("Open or create a project to see its scripts");
			return;
		}

		// Listed when shown, then every c_RefreshSeconds while shown: files change in the code editor, outside the editor.
		const std::filesystem::path directory = project->GetScriptSourceDirectory();
		const double now = ImGui::GetTime();
		if (directory != m_List.GetDirectory() || m_RefreshRequested || (!m_List.IsListing() && now - m_LastRefreshTime >= c_RefreshSeconds))
		{
			m_List.Refresh(directory);
			m_LastRefreshTime = now;
			m_RefreshRequested = false;
		}
		m_List.Update();
		if (m_RowsVersion != m_List.GetVersion() || m_RowsFilter != filter)
			RebuildRows(filter);

		const UI::ThemeColors& colors = UI::GetThemeColors();
		if (!m_LastOpenError.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, colors.Warning);
			ImGui::TextWrapped("%s", m_LastOpenError.c_str());
			ImGui::PopStyleColor();
		}

		const ScriptSourceListing& listing = m_List.GetListing();
		if (!m_List.HasListed())
		{
			ImGui::TextDisabled("Listing the scripts...");
			return;
		}
		if (listing.Files.empty())
		{
			UI::CenteredText("ScriptBrowser.Empty", "No scripts yet. New Script... writes a script class into the project's Scripts folder; "
				"attach it to an entity with Add Component > Script.", colors.TextSecondary);
			return;
		}
		if (m_Rows.empty())
		{
			ImGui::TextDisabled("No file or class matches the search");
			return;
		}

		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV
			| ImGuiTableFlags_NoSavedSettings;
		const float footer = listing.Truncated ? ImGui::GetTextLineHeightWithSpacing() : 0.0f;
		const float height = std::max(ImGui::GetContentRegionAvail().y - footer, ImGui::GetFrameHeight() * 3.0f);
		if (ImGui::BeginTable("ScriptFiles", 2, flags, ImVec2(0.0f, height)))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 1.0f - c_ClassesColumnWeight);
			ImGui::TableSetupColumn("Classes", ImGuiTableColumnFlags_WidthStretch, c_ClassesColumnWeight);
			ImGui::TableHeadersRow();

			std::optional<size_t> open;
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(m_Rows.size()));
			while (clipper.Step())
			{
				for (int line = clipper.DisplayStart; line < clipper.DisplayEnd; line++)
				{
					const size_t row = static_cast<size_t>(line);
					const size_t index = m_Rows[row];
					const ScriptSourceFile& file = listing.Files[index];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::PushID(file.Path.c_str());
					ImGui::PushStyleColor(ImGuiCol_Text, colors.TextSecondary);
					ImGui::TextUnformatted(GetFileIcon(file.Path));
					ImGui::PopStyleColor();
					ImGui::SameLine();
					// An unlabeled row with the path drawn after it: a path may hold "##", which a label would cut.
					const bool selected = file.Path == m_Selected;
					UI::PushSelectionColors(selected);
					if (ImGui::Selectable("##File", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
						m_Selected = file.Path;
					UI::PopSelectionColors();
					m_Rects.emplace_back(index, FileRect { ImGui::GetItemRectMin(), ImGui::GetItemRectMax() });
					// The first rows only: the probe keeps a fixed set of keys.
					if (row < c_ProbedRows)
						UI::ItemProbe::Record(UI::NumberedProbeKey("ScriptBrowser.Row.", row), true, 0, file.Path);
					if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
						open = index;
					if (UI::IsTooltipDue())
					{
						const std::string classes = file.Classes.empty() ? std::string("Registers no script class") : "Script classes: " + m_ClassTexts[index];
						ImGui::SetTooltip("%s\n%s\nDouble-click to open it in the external editor", FileSystem::ToUTF8(directory / FileSystem::FromUTF8(file.Path)).c_str(),
							classes.c_str());
					}
					if (ImGui::BeginPopupContextItem())
					{
						m_Selected = file.Path;
						DrawFileMenu(context, file);
						ImGui::EndPopup();
					}
					ImGui::SameLine(0.0f, 0.0f);
					ImGui::TextUnformatted(file.Path.c_str());
					ImGui::PopID();
					ImGui::TableSetColumnIndex(1);
					ImGui::TextUnformatted(m_ClassTexts[index].c_str());
				}
			}
			ImGui::EndTable();
			if (open)
				OpenFile(context, listing.Files[*open].Path);
		}
		if (listing.Truncated)
			ImGui::TextDisabled("Showing the first %zu files of the Scripts folder", listing.Files.size());
	}

	void ScriptBrowser::DrawFileMenu(EditorPanelContext& context, const ScriptSourceFile& file)
	{
		const std::filesystem::path path = m_List.GetDirectory() / FileSystem::FromUTF8(file.Path);
		if (ImGui::MenuItem("Open"))
			OpenFile(context, file.Path);
		if (ImGui::MenuItem("Show in Folder"))
		{
			if (!Platform::OpenWithDefaultApplication(FileSystem::ToUTF8(path.parent_path())))
				ST_ERROR("Scripts: cannot show the folder '{}'", FileSystem::ToUTF8(path.parent_path()));
		}
		if (ImGui::MenuItem("Copy Path"))
			ImGui::SetClipboardText(FileSystem::ToUTF8(path).c_str());
	}

	bool ScriptBrowser::OpenFile(EditorPanelContext& context, std::string_view path)
	{
		m_LastOpenError.clear();
		m_Selected = std::string(path);
		if (!context.Context.OpenScriptSource(m_List.GetDirectory() / FileSystem::FromUTF8(path), 1, 1, nullptr, &m_LastOpenError))
		{
			ST_ERROR("Scripts: {}", m_LastOpenError);
			return false;
		}
		return true;
	}

	std::optional<ScriptBrowser::FileRect> ScriptBrowser::FindFileRect(std::string_view path) const
	{
		const std::vector<ScriptSourceFile>& files = m_List.GetListing().Files;
		for (const auto& [index, rect] : m_Rects)
		{
			if (index < files.size() && files[index].Path == path)
				return rect;
		}
		return std::nullopt;
	}

	void ScriptBrowser::RebuildRows(std::string_view filter)
	{
		const ScriptSourceListing& listing = m_List.GetListing();
		const std::vector<std::string> words = SplitWords(filter);
		m_ClassTexts.clear();
		m_Rows.clear();
		for (size_t index = 0; index < listing.Files.size(); index++)
		{
			const ScriptSourceFile& file = listing.Files[index];
			std::string classes;
			for (const std::string& name : file.Classes)
				classes += (classes.empty() ? "" : ", ") + name;
			// Every word in the path or in a class name.
			const std::string searched = words.empty() ? std::string() : StringUtils::ToLower(file.Path) + "\n" + StringUtils::ToLower(classes);
			const bool matches = std::all_of(words.begin(), words.end(), [&searched](const std::string& word) { return searched.find(word) != std::string::npos; });
			if (matches)
				m_Rows.push_back(index);
			m_ClassTexts.push_back(std::move(classes));
		}
		m_RowsVersion = m_List.GetVersion();
		m_RowsFilter = std::string(filter);
	}

}
