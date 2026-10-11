#pragma once

#include "UI/EditorPanelRegistry.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class CommandJournal;
	struct CommandJournalEntry;

	// The activity journal (CommandJournal): every command the person, agents, command scripts and the editor ran, newest at
	// the bottom, filtered by text, source and whether it changed anything, with a JSON details pane for the selected one and
	// its actions: Copy as command script, Re-run, and Undo to here (undoes every step after the entry's own). The rows are
	// built again only when the journal or the filters change, and drawn clipped.
	class ActivityPanel : public EditorPanel
	{
	public:
		void OnImGuiRender(EditorPanelContext& context) override;

		// Selects an entry (0: none) and scrolls it into view at the next draw, e.g. for the toasts' Show in Activity.
		void Select(uint64_t id);
		uint64_t GetSelected() const { return m_Selected; }
		// The ids of the entries the list shows, oldest first (after the filters), as of the last draw.
		const std::vector<uint64_t>& GetShownEntries() const { return m_Rows; }

		// A command script of one step that runs the entry's command again with its parameters; nullopt when the journal
		// shortened them (the step would not be the same).
		static std::optional<std::string> MakeCommandScript(const CommandJournalEntry& entry);
	private:
		void DrawFilters();
		void RebuildRows(const CommandJournal& journal);
		void DrawList(const CommandJournal& journal);
		void DrawDetails(EditorPanelContext& context, const CommandJournalEntry& entry);
		// The undo position "Undo to here" goes back to (the entry's step is the latest applied), when it can.
		static std::optional<size_t> GetUndoTarget(EditorContext& context, const CommandJournalEntry& entry);
		const std::string& GetParameterPreview(const CommandJournalEntry& entry);
	private:
		std::vector<uint64_t> m_Rows;        // Entries passing the filters, oldest first
		uint64_t m_RowsVersion = UINT64_MAX; // The journal version m_Rows was built from
		std::string m_RowsKey;               // The filters m_Rows was built with
		// Compact parameters of entries, by id, made once (parameters never change); pruned with the journal's ring.
		std::unordered_map<uint64_t, std::string> m_ParameterPreviews;
		uint64_t m_Selected = 0;
		bool m_ScrollToSelected = false;
		std::string m_Details;        // The selected entry described as JSON
		uint64_t m_DetailsId = 0;
		uint64_t m_DetailsVersion = UINT64_MAX;
		std::string m_Filter;
		int m_SourceFilter = 0; // 0: every source, else CommandSource + 1
		bool m_EditsOnly = false;
		bool m_FollowNewest = true;
	};

}
