#include "Panels/ActivityPanel.h"

#include "Editor/CommandJournal.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/UndoStack.h"
#include "UI/EditorActions.h"
#include "UI/EditorFonts.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/TextFormat.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/StringUtils.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <chrono>

namespace Strata
{

	namespace
	{

		// Parameters longer than this show cut in the list (the details pane has them whole).
		constexpr size_t c_ParameterPreviewBytes = 160;
		// Widths in font sizes.
		constexpr float c_SourceColumnInFontSizes = 1.6f;
		constexpr float c_CommandColumnInFontSizes = 11.0f;
		constexpr float c_ResultColumnInFontSizes = 10.0f;
		constexpr float c_DurationColumnInFontSizes = 4.5f;
		constexpr float c_SourceFilterInFontSizes = 9.0f;
		constexpr float c_SearchInFontSizes = 14.0f;
		// The details pane beside the list takes this share of the width, but not less than this many font sizes; narrower
		// panels show it below the list.
		constexpr float c_DetailsShare = 0.4f;
		constexpr float c_MinDetailsInFontSizes = 18.0f;
		constexpr float c_SideBySideInFontSizes = 40.0f;

		constexpr const char* c_SourceFilterNames[] = { "Every source", "UI", "Automation", "Command scripts", "Editor" };

		// The text cut to at most `bytes` (never inside a UTF-8 sequence), with an ellipsis when it was longer.
		std::string Shorten(std::string text, size_t bytes)
		{
			if (text.size() <= bytes)
				return text;
			size_t end = bytes;
			while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
				end--;
			text.resize(end);
			return text + "...";
		}

		const char* GetSourceIcon(CommandSource source)
		{
			switch (source)
			{
				case CommandSource::UI:            return Icons::MousePointer;
				case CommandSource::Automation:    return Icons::Bot;
				case CommandSource::CommandScript: return Icons::ScrollText;
				case CommandSource::Internal:      return Icons::Cog;
			}
			return Icons::Cog;
		}

		std::string DescribeSource(const CommandJournalEntry& entry)
		{
			switch (entry.Source)
			{
				case CommandSource::UI:            return "The person, in the editor's interface";
				case CommandSource::Automation:    return entry.Client.empty() ? "An automation client that gave no name" : fmt::format("Automation: {}", entry.Client);
				case CommandSource::CommandScript: return "A command script (--commands)";
				case CommandSource::Internal:      return entry.Parent != 0 ? fmt::format("Run by #{}", entry.Parent) : "The editor itself";
			}
			return {};
		}

		// What the Result column says, in which color.
		std::pair<std::string, ImVec4> DescribeOutcome(const CommandJournalEntry& entry, const UI::ThemeColors& colors)
		{
			if (entry.Status == CommandStatus::Pending)
				return { "running", colors.TextSecondary };
			if (entry.Status == CommandStatus::Failed)
			{
				// The first line of the error is enough for the list.
				const std::string_view error = entry.Error;
				return { std::string(error.substr(0, error.find('\n'))), colors.Error };
			}
			if (!entry.Warning.empty())
				return { entry.Warning, colors.Warning };
			if (!entry.UndoStep.empty())
				return { entry.UndoStep, colors.Text };
			return { "done", colors.TextSecondary };
		}

	}

	void ActivityPanel::Select(uint64_t id)
	{
		m_Selected = id;
		m_ScrollToSelected = id != 0;
		m_FollowNewest = id == 0;
	}

	std::optional<std::string> ActivityPanel::MakeCommandScript(const CommandJournalEntry& entry)
	{
		if (entry.ParametersTruncated)
			return std::nullopt;
		nlohmann::json step = { { "command", entry.Command } };
		if (entry.Parameters.is_object() && !entry.Parameters.empty())
			step["parameters"] = entry.Parameters;
		return JsonUtils::Dump(nlohmann::json::array({ std::move(step) }), 1, '\t') + "\n";
	}

	const std::string& ActivityPanel::GetParameterPreview(const CommandJournalEntry& entry)
	{
		auto [it, added] = m_ParameterPreviews.try_emplace(entry.Id);
		if (added)
		{
			const bool empty = entry.Parameters.is_null() || (entry.Parameters.is_object() && entry.Parameters.empty());
			it->second = empty ? std::string() : Shorten(JsonUtils::Dump(entry.Parameters), c_ParameterPreviewBytes);
		}
		return it->second;
	}

	void ActivityPanel::DrawFilters()
	{
		UI::SearchField("Activity.Search", m_Filter, "Filter", ImGui::GetFontSize() * c_SearchInFontSizes,
			"Shows entries whose command, client, parameters or error contain the text");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * c_SourceFilterInFontSizes);
		ImGui::Combo("##Source", &m_SourceFilter, c_SourceFilterNames, static_cast<int>(std::size(c_SourceFilterNames)));
		UI::ItemProbe::Record("Activity.Source");
		ImGui::SameLine();
		ImGui::Checkbox("Changes only", &m_EditsOnly);
		UI::ItemProbe::Record("Activity.ChangesOnly");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Only commands that change the scene, its assets or the project (what a replay repeats)");
	}

	void ActivityPanel::RebuildRows(const CommandJournal& journal)
	{
		const std::string filter = StringUtils::ToLower(m_Filter);
		const std::string key = fmt::format("{}|{}|{}", m_SourceFilter, m_EditsOnly, filter);
		if (journal.GetVersion() == m_RowsVersion && key == m_RowsKey)
			return;
		m_RowsVersion = journal.GetVersion();
		m_RowsKey = key;

		const std::deque<CommandJournalEntry>& entries = journal.GetEntries();
		// Previews of entries the ring dropped go with them.
		const uint64_t oldest = entries.empty() ? UINT64_MAX : entries.front().Id;
		std::erase_if(m_ParameterPreviews, [oldest](const auto& preview) { return preview.first < oldest; });

		m_Rows.clear();
		for (const CommandJournalEntry& entry : entries)
		{
			if (m_SourceFilter > 0 && static_cast<int>(entry.Source) != m_SourceFilter - 1)
				continue;
			if (m_EditsOnly && entry.Kind != EditorCommandKind::Edit)
				continue;
			if (!filter.empty())
			{
				const bool found = StringUtils::ToLower(entry.Command).find(filter) != std::string::npos
					|| StringUtils::ToLower(entry.Client).find(filter) != std::string::npos
					|| StringUtils::ToLower(GetParameterPreview(entry)).find(filter) != std::string::npos
					|| StringUtils::ToLower(entry.Error).find(filter) != std::string::npos;
				if (!found)
					continue;
			}
			m_Rows.push_back(entry.Id);
		}
	}

	void ActivityPanel::DrawList(const CommandJournal& journal)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable
			| ImGuiTableFlags_SizingFixedFit;
		if (!ImGui::BeginTable("Entries", 5, flags))
			return;
		const float fontSize = ImGui::GetFontSize();
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("##Source", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, fontSize * c_SourceColumnInFontSizes);
		ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, fontSize * c_CommandColumnInFontSizes);
		ImGui::TableSetupColumn("Parameters", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, fontSize * c_ResultColumnInFontSizes);
		ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, fontSize * c_DurationColumnInFontSizes);
		ImGui::TableHeadersRow();

		const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(m_Rows.size()));
		std::optional<int> selectedRow;
		if (m_ScrollToSelected)
		{
			if (const auto it = std::find(m_Rows.begin(), m_Rows.end(), m_Selected); it != m_Rows.end())
			{
				selectedRow = static_cast<int>(it - m_Rows.begin());
				clipper.IncludeItemByIndex(*selectedRow);
			}
		}
		while (clipper.Step())
		{
			for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
			{
				const CommandJournalEntry* entry = journal.Find(m_Rows[static_cast<size_t>(row)]);
				if (!entry)
					continue;
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::PushID(static_cast<int>(entry->Id));
				const bool selected = entry->Id == m_Selected;
				UI::PushSelectionColors(selected);
				if (ImGui::Selectable(GetSourceIcon(entry->Source), selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
				{
					m_Selected = selected ? 0 : entry->Id;
					m_FollowNewest = false;
				}
				UI::PopSelectionColors();
				UI::ItemProbe::Record(fmt::format("Activity.Row.{}", entry->Id), true, 0, entry->Command);
				if (ImGui::IsItemHovered() && UI::IsTooltipDue())
					ImGui::SetTooltip("#%llu %s", static_cast<unsigned long long>(entry->Id), DescribeSource(*entry).c_str());
				if (selectedRow && *selectedRow == row)
				{
					ImGui::SetScrollHereY(0.5f);
					m_ScrollToSelected = false;
				}
				ImGui::PopID();

				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(entry->Command.c_str());
				ImGui::TableSetColumnIndex(2);
				UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
				ImGui::TextColored(colors.TextSecondary, "%s", GetParameterPreview(*entry).c_str());
				ImGui::PopFont();
				ImGui::TableSetColumnIndex(3);
				const auto [outcome, color] = DescribeOutcome(*entry, colors);
				ImGui::TextColored(color, "%s", outcome.c_str());
				ImGui::TableSetColumnIndex(4);
				if (entry->Status != CommandStatus::Pending)
					ImGui::Text("%.1f", entry->DurationMs);
			}
		}
		// New entries scroll into view while the list shows its end.
		if (m_FollowNewest && atBottom && !m_ScrollToSelected)
			ImGui::SetScrollHereY(1.0f);
		m_FollowNewest = atBottom || m_FollowNewest;
		ImGui::EndTable();
	}

	std::optional<size_t> ActivityPanel::GetUndoTarget(EditorContext& context, const CommandJournalEntry& entry)
	{
		if (entry.UndoSerial == 0 || context.IsPlaying())
			return std::nullopt;
		const UndoStack& undo = context.GetUndoStack();
		const std::optional<size_t> index = undo.FindSerial(entry.UndoSerial);
		// Its step must be applied and have steps after it.
		if (!index || *index + 1 >= undo.GetPosition())
			return std::nullopt;
		return *index + 1;
	}

	void ActivityPanel::DrawDetails(EditorPanelContext& panelContext, const CommandJournalEntry& entry)
	{
		EditorContext& context = panelContext.Context;
		const UI::ThemeColors& colors = UI::GetThemeColors();
		UI::Heading(entry.Command);
		const double age = std::chrono::duration<double>(std::chrono::steady_clock::now() - entry.Started).count();
		ImGui::TextColored(colors.TextSecondary, "#%llu  %s  %s", static_cast<unsigned long long>(entry.Id), DescribeSource(entry).c_str(),
			UI::DescribeSecondsAgo(age).c_str());

		// Copy as command script: the entry as a --commands step.
		const std::optional<std::string> script = MakeCommandScript(entry);
		UI::ButtonStyle copyStyle;
		copyStyle.Enabled = script.has_value();
		if (UI::ToolbarButton("Activity.CopyScript", Icons::Copy, "Copy as script",
				script ? "Copy a command script that runs this command again (StrataEditor --commands)"
					   : "The journal shortened this command's parameters: it cannot be written as a script", copyStyle))
			ImGui::SetClipboardText(script->c_str());

		// Re-run, as the person: through the runner, so that commands taking frames work too.
		ImGui::SameLine();
		UI::ButtonStyle rerunStyle;
		rerunStyle.Enabled = !entry.ParametersTruncated && entry.Status != CommandStatus::Pending;
		if (UI::ToolbarButton("Activity.Rerun", Icons::RotateCcw, "Re-run",
				rerunStyle.Enabled ? "Run this command again with the same parameters" : "Only finished commands whose parameters the journal kept whole", rerunStyle))
		{
			const std::string command = entry.Command;
			panelContext.Runner.Run(context, panelContext.Commands, command, entry.Parameters, [command](const EditorCommandResult& result)
			{
				if (!result.Success)
					ST_ERROR("{}: {}", command, result.Error);
				else if (result.Value.is_object() && result.Value.contains("warning") && result.Value["warning"].is_string())
					ST_WARN("{}: {}", command, result.Value["warning"].get<std::string>());
			}, CommandOrigin { CommandSource::UI });
		}

		// Undo to here: every step after this entry's own, through edit.undo, so the journal shows the undos too.
		ImGui::SameLine();
		const std::optional<size_t> target = GetUndoTarget(context, entry);
		UI::ButtonStyle undoStyle;
		undoStyle.Enabled = target.has_value();
		const std::string undoTooltip = target ? fmt::format("Undo the {} steps after this one, back to '{}'", context.GetUndoStack().GetPosition() - *target, entry.UndoStep)
			: (entry.UndoSerial == 0 ? std::string("This command recorded no undo step")
				: std::string("Its step is the latest, undone, gone from the history, or the scene is playing"));
		if (UI::ToolbarButton("Activity.UndoToHere", Icons::Undo2, "Undo to here", undoTooltip.c_str(), undoStyle))
		{
			while (target && context.GetUndoStack().GetPosition() > *target)
			{
				if (RunEditorCommand(context, panelContext.Commands, "edit.undo").is_null())
					break;
			}
		}

		// The entry as editor.activity reports it, selectable for copying.
		if (m_DetailsId != entry.Id || m_DetailsVersion != context.GetJournal().GetVersion())
		{
			m_Details = JsonUtils::Dump(CommandJournal::Describe(entry), 2, ' ');
			m_DetailsId = entry.Id;
			m_DetailsVersion = context.GetJournal().GetVersion();
		}
		UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
		ImGui::InputTextMultiline("##Details", &m_Details, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
		ImGui::PopFont();
		UI::ItemProbe::Record("Activity.Details", true, 0, m_Details);
	}

	void ActivityPanel::OnImGuiRender(EditorPanelContext& panelContext)
	{
		const CommandJournal& journal = panelContext.Context.GetJournal();
		DrawFilters();
		RebuildRows(journal);
		ImGui::SameLine();
		ImGui::TextColored(UI::GetThemeColors().TextSecondary, "%zu of %zu", m_Rows.size(), journal.GetEntries().size());

		const CommandJournalEntry* selected = m_Selected != 0 ? journal.Find(m_Selected) : nullptr;
		if (!selected)
			m_Selected = 0;
		const ImVec2 available = ImGui::GetContentRegionAvail();
		const float fontSize = ImGui::GetFontSize();
		const bool sideBySide = available.x >= fontSize * c_SideBySideInFontSizes;
		const float detailsWidth = std::max(available.x * c_DetailsShare, fontSize * c_MinDetailsInFontSizes);
		const ImVec2 listSize = !selected ? ImVec2(0.0f, 0.0f)
			: (sideBySide ? ImVec2(available.x - detailsWidth - ImGui::GetStyle().ItemSpacing.x, 0.0f) : ImVec2(0.0f, available.y * (1.0f - c_DetailsShare)));
		if (ImGui::BeginChild("List", listSize))
			DrawList(journal);
		ImGui::EndChild();
		if (selected)
		{
			if (sideBySide)
				ImGui::SameLine();
			if (ImGui::BeginChild("Details", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders))
				DrawDetails(panelContext, *selected);
			ImGui::EndChild();
		}
	}

}
