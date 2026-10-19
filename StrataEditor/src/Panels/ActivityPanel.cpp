#include "Panels/ActivityPanel.h"

#include "Editor/CommandJournal.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/UndoStack.h"
#include "UI/EditorActions.h"
#include "UI/EditorFonts.h"
#include "UI/EditorShell.h"
#include "UI/Icons.h"
#include "UI/ItemProbe.h"
#include "UI/TextFormat.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/StringUtils.h>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <chrono>

namespace Strata
{

	namespace
	{

		// Parameters longer than this show cut in the list (the details pane has them whole).
		constexpr size_t c_ParameterPreviewBytes = 160;
		// Results longer than this show cut in an expanded entry's detail row (the details pane has them whole).
		constexpr size_t c_ResultPreviewBytes = 240;
		// Widths in font sizes; the summary (the names of undo steps, such as "Rename 'Wall' to 'East Wall'", the first line of
		// errors) takes the rest.
		constexpr float c_SourceColumnInFontSizes = 1.6f;
		constexpr float c_CommandColumnInFontSizes = 12.0f;
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

		// The color of an entry's summary (CommandJournal::Summarize): errors, warnings and discarded changes stand out, undo
		// steps read as text, the rest is secondary.
		const ImVec4& GetSummaryColor(const CommandJournalEntry& entry, const UI::ThemeColors& colors)
		{
			if (entry.Status == CommandStatus::Pending)
				return colors.TextSecondary;
			if (entry.Status == CommandStatus::Failed)
				return colors.Error;
			if (!entry.Warning.empty() || entry.ResultSummary.find("\"discardedChanges\":true") != std::string::npos)
				return colors.Warning;
			return entry.UndoStep.empty() ? colors.TextSecondary : colors.Text;
		}

		// Commands that replace the edited scene take "force" (CommandUtils::ForceSchema), which discards its unsaved changes.
		bool ReplacesScene(const EditorCommand& command)
		{
			const auto properties = command.Parameters.find("properties");
			return properties != command.Parameters.end() && properties->is_object() && properties->contains("force");
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
		nlohmann::json parameters = entry.Parameters;
		if (entry.Source == CommandSource::UI && parameters.is_object())
			parameters.erase("force");
		nlohmann::json step = { { "command", entry.Command } };
		if (parameters.is_object() && !parameters.empty())
			step["parameters"] = std::move(parameters);
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
		const std::string key = fmt::format("{}|{}|{}|{}", m_SourceFilter, m_EditsOnly, m_ExpandedVersion, filter);
		if (journal.GetVersion() == m_RowsVersion && key == m_RowsKey)
			return;
		m_RowsVersion = journal.GetVersion();
		m_RowsKey = key;

		const std::deque<CommandJournalEntry>& entries = journal.GetEntries();
		// Previews and expansions of entries the ring dropped go with them.
		const uint64_t oldest = entries.empty() ? UINT64_MAX : entries.front().Id;
		std::erase_if(m_ParameterPreviews, [oldest](const auto& preview) { return preview.first < oldest; });
		std::erase_if(m_Expanded, [oldest](uint64_t id) { return id < oldest; });

		m_Rows.clear();
		m_Lines.clear();
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
			m_Lines.push_back({ entry.Id, LineKind::Entry });
			if (m_Expanded.contains(entry.Id))
			{
				m_Lines.push_back({ entry.Id, LineKind::Parameters });
				m_Lines.push_back({ entry.Id, LineKind::Result });
			}
		}
	}

	const std::string& ActivityPanel::GetResultPreview(const CommandJournalEntry& entry)
	{
		// A running command's result is still to come; a finished one's never changes.
		auto [it, added] = m_ResultPreviews.try_emplace(entry.Id);
		if (added || it->second.first != entry.Status)
		{
			std::string text = entry.Status == CommandStatus::Pending ? std::string("(running)")
				: entry.Status == CommandStatus::Failed ? fmt::format("{} error: {}", EditorCommandErrorToString(entry.ErrorKind), entry.Error)
				: entry.ResultSummary;
			it->second = { entry.Status, Shorten(std::move(text), c_ResultPreviewBytes) };
		}
		return it->second.second;
	}

	void ActivityPanel::DrawList(const CommandJournal& journal)
	{
		const UI::ThemeColors& colors = UI::GetThemeColors();
		constexpr ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable
			| ImGuiTableFlags_SizingFixedFit;
		const float fontSize = ImGui::GetFontSize();
		if (!ImGui::BeginTable("Entries", 4, flags))
			return;
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("##Source", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, fontSize * c_SourceColumnInFontSizes);
		ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, fontSize * c_CommandColumnInFontSizes);
		ImGui::TableSetupColumn("Summary", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, fontSize * c_DurationColumnInFontSizes);
		ImGui::TableHeadersRow();

		const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(m_Lines.size()));
		std::optional<int> selectedLine;
		if (m_ScrollToSelected)
		{
			const auto it = std::find_if(m_Lines.begin(), m_Lines.end(), [this](const Line& line) { return line.Entry == m_Selected; });
			if (it != m_Lines.end())
			{
				selectedLine = static_cast<int>(it - m_Lines.begin());
				clipper.IncludeItemByIndex(*selectedLine);
			}
		}
		// A row just expanded brings its detail lines into view (the last one is enough).
		if (m_ScrollToExpanded != 0)
		{
			const auto it = std::find_if(m_Lines.begin(), m_Lines.end(),
				[this](const Line& line) { return line.Entry == m_ScrollToExpanded && line.Kind == LineKind::Result; });
			if (it != m_Lines.end())
				clipper.IncludeItemByIndex(static_cast<int>(it - m_Lines.begin()));
			else
				m_ScrollToExpanded = 0;
		}
		std::optional<uint64_t> toggled;
		while (clipper.Step())
		{
			for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; index++)
			{
				const Line& line = m_Lines[static_cast<size_t>(index)];
				const CommandJournalEntry* entry = journal.Find(line.Entry);
				if (!entry)
					continue;
				ImGui::TableNextRow();
				if (line.Kind != LineKind::Entry)
				{
					// An expanded entry's raw parameters and result, cut to a line (the details pane has them whole).
					const bool parameters = line.Kind == LineKind::Parameters;
					ImGui::TableSetColumnIndex(1);
					ImGui::Indent(fontSize);
					ImGui::TextColored(colors.TextSecondary, "%s", parameters ? "parameters" : "result");
					ImGui::Unindent(fontSize);
					ImGui::TableSetColumnIndex(2);
					const std::string& text = parameters ? GetParameterPreview(*entry) : GetResultPreview(*entry);
					UI::PushFont(UI::EditorFont::Mono, UI::TextSize::Caption);
					ImGui::TextColored(colors.TextSecondary, "%s", text.empty() ? "{}" : text.c_str());
					ImGui::PopFont();
					UI::ItemProbe::Record(fmt::format("Activity.{}.{}", parameters ? "Parameters" : "ResultJson", entry->Id), true, 0, text);
					if (!parameters && entry->Id == m_ScrollToExpanded)
					{
						ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
						m_ScrollToExpanded = 0;
					}
					continue;
				}

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
				if (selectedLine && *selectedLine == index)
				{
					ImGui::SetScrollHereY(0.5f);
					m_ScrollToSelected = false;
				}

				// The command, after the arrow that shows its parameters and result below it.
				ImGui::TableSetColumnIndex(1);
				const bool expanded = m_Expanded.contains(entry->Id);
				const std::string expandId = fmt::format("Activity.Expand.{}", entry->Id);
				if (UI::IconButton(expandId.c_str(), expanded ? Icons::ChevronDown : Icons::ChevronRight,
						expanded ? "Hide the parameters and the result" : "Show the parameters and the result"))
					toggled = entry->Id;
				ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
				ImGui::TextUnformatted(entry->Command.c_str());
				ImGui::PopID();

				ImGui::TableSetColumnIndex(2);
				const std::string summary = CommandJournal::Summarize(*entry);
				const ImVec4& color = GetSummaryColor(*entry, colors);
				ImGui::TextColored(color, "%s", summary.c_str());
				UI::ItemProbe::Record(fmt::format("Activity.Summary.{}", entry->Id), true, UI::ToColorU32(color), summary);
				ImGui::TableSetColumnIndex(3);
				if (entry->Status != CommandStatus::Pending)
					ImGui::Text("%.1f", entry->DurationMs);
			}
		}
		// New entries scroll into view while the list shows its end.
		if (m_FollowNewest && atBottom && !m_ScrollToSelected)
			ImGui::SetScrollHereY(1.0f);
		m_FollowNewest = atBottom || m_FollowNewest;
		ImGui::EndTable();
		if (toggled)
			SetExpanded(*toggled, !m_Expanded.contains(*toggled));
	}

	void ActivityPanel::SetExpanded(uint64_t id, bool expanded)
	{
		if (expanded ? m_Expanded.insert(id).second : m_Expanded.erase(id) > 0)
			m_ExpandedVersion++;
		m_ScrollToExpanded = expanded ? id : 0;
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

		// Re-run and Undo to here run commands, which the journal records: a full ring drops its oldest entry for each, and that
		// may be this one. So they run once the entry is no longer used (RunAction), with what they need copied.
		std::optional<PendingAction> action;

		// Re-run, as the person: through the runner, so that commands taking frames work too.
		ImGui::SameLine();
		UI::ButtonStyle rerunStyle;
		rerunStyle.Enabled = !entry.ParametersTruncated && entry.Status != CommandStatus::Pending;
		if (UI::ToolbarButton("Activity.Rerun", Icons::RotateCcw, "Re-run",
				rerunStyle.Enabled ? "Run this command again with the same parameters" : "Only finished commands whose parameters the journal kept whole", rerunStyle))
			action = PendingAction { PendingAction::Type::Rerun, entry.Command, entry.Parameters, 0 };

		// Undo to here: every step after this entry's own, through edit.undo, so the journal shows the undos too.
		ImGui::SameLine();
		const std::optional<size_t> target = GetUndoTarget(context, entry);
		UI::ButtonStyle undoStyle;
		undoStyle.Enabled = target.has_value();
		const std::string undoTooltip = target ? fmt::format("Undo the {} steps after this one, back to '{}'", context.GetUndoStack().GetPosition() - *target, entry.UndoStep)
			: (entry.UndoSerial == 0 ? std::string("This command recorded no undo step")
				: std::string("Its step is the latest, undone, gone from the history, or the scene is playing"));
		if (UI::ToolbarButton("Activity.UndoToHere", Icons::Undo2, "Undo to here", undoTooltip.c_str(), undoStyle) && target)
			action = PendingAction { PendingAction::Type::UndoToHere, {}, {}, *target };

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

		// `entry` is not used from here on.
		if (action)
			RunAction(panelContext, std::move(*action));
	}

	void ActivityPanel::RunAction(EditorPanelContext& panelContext, PendingAction action)
	{
		EditorContext& context = panelContext.Context;
		if (action.Kind == PendingAction::Type::UndoToHere)
		{
			while (context.GetUndoStack().GetPosition() > action.UndoTarget)
			{
				if (RunEditorCommand(context, panelContext.Commands, "edit.undo").is_null())
					break;
			}
			return;
		}

		// A command that replaces the edited scene asks about its unsaved changes now, as the menus do: the force it was recorded
		// with answered for other changes. The answer may come in a later frame, so the run keeps what outlives this one.
		const EditorCommand* command = panelContext.Commands.Find(action.Command);
		const bool replacesScene = command && ReplacesScene(*command);
		if (replacesScene && action.Parameters.is_object())
			action.Parameters.erase("force");
		auto rerun = [editor = &context, commands = &panelContext.Commands, runner = &panelContext.Runner, name = action.Command,
			parameters = action.Parameters](bool force)
		{
			nlohmann::json given = parameters;
			if (force)
				given["force"] = true;
			runner->Run(*editor, *commands, name, given, [name](const EditorCommandResult& result)
			{
				if (!result.Success)
					ST_ERROR("{}: {}", name, result.Error);
				else if (result.Value.is_object() && result.Value.contains("warning") && result.Value["warning"].is_string())
					ST_WARN("{}: {}", name, result.Value["warning"].get<std::string>());
			}, CommandOrigin { CommandSource::UI });
		};
		if (replacesScene && panelContext.Shell)
			panelContext.Shell->RequestDiscardChanges([rerun]() { rerun(true); });
		else
			rerun(false);
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
