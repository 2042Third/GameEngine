#pragma once

#include "Editor/CommandJournal.h"
#include "UI/EditorPanelRegistry.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Strata
{

	// The activity journal (CommandJournal): every command the person, agents, command scripts and the editor ran, newest at
	// the bottom, each with its summary (CommandJournal::Summarize: "Rename 'Crate' to 'Box'") and an arrow that shows its raw
	// parameters and result below it, filtered by text, source and whether it changed anything, with a JSON details pane for
	// the selected one and
	// its actions: Copy as command script, Re-run (a command that replaces the edited scene asks about its unsaved changes
	// first, as the menus do), and Undo to here (undoes every step after the entry's own). The rows are built again only when
	// the journal or the filters change, and drawn clipped.
	class ActivityPanel : public EditorPanel
	{
	public:
		void OnImGuiRender(EditorPanelContext& context) override;

		// Selects an entry (0: none) and scrolls it into view at the next draw, e.g. for the toasts' Show in Activity.
		void Select(uint64_t id);
		uint64_t GetSelected() const { return m_Selected; }
		// The ids of the entries the list shows, oldest first (after the filters), as of the last draw.
		const std::vector<uint64_t>& GetShownEntries() const { return m_Rows; }
		// Shows (or hides) an entry's raw parameters and result in lines below its row, as its arrow does.
		void SetExpanded(uint64_t id, bool expanded);
		bool IsExpanded(uint64_t id) const { return m_Expanded.contains(id); }

		// A command script of one step that runs the entry's command again with its parameters; nullopt when the journal
		// shortened them (the step would not be the same). The "force" of a command the person ran is left out: it was their
		// answer to the question about unsaved changes then (CommandUtils::CheckUnsavedChanges), not part of the command.
		static std::optional<std::string> MakeCommandScript(const CommandJournalEntry& entry);
	private:
		// What a button of the details pane asked for, with what it needs copied from the entry.
		struct PendingAction
		{
			enum class Type : uint8_t
			{
				Rerun = 0,
				UndoToHere
			};

			Type Kind = Type::Rerun;
			std::string Command;       // Rerun: the command and its parameters
			nlohmann::json Parameters;
			size_t UndoTarget = 0;     // UndoToHere: the undo position to go back to
		};

		void DrawFilters();
		void RebuildRows(const CommandJournal& journal);
		void DrawList(const CommandJournal& journal);
		// Draws the entry's details and buttons; what a button asks for runs at the end, once the entry is not used any more
		// (the commands it runs may drop it from a full journal).
		void DrawDetails(EditorPanelContext& context, const CommandJournalEntry& entry);
		void RunAction(EditorPanelContext& context, PendingAction action);
		// The undo position "Undo to here" goes back to (the entry's step is the latest applied), when it can.
		static std::optional<size_t> GetUndoTarget(EditorContext& context, const CommandJournalEntry& entry);
		const std::string& GetParameterPreview(const CommandJournalEntry& entry);
		// The entry's result as compact JSON (its error for a failure), cut to a line's worth: made once per status.
		const std::string& GetResultPreview(const CommandJournalEntry& entry);
	private:
		// A line of the list: an entry's row, or one of the two below an expanded entry.
		enum class LineKind : uint8_t
		{
			Entry = 0,
			Parameters,
			Result
		};
		struct Line
		{
			uint64_t Entry = 0;
			LineKind Kind = LineKind::Entry;
		};

		std::vector<uint64_t> m_Rows;        // Entries passing the filters, oldest first
		std::vector<Line> m_Lines;           // What the list draws: m_Rows with the detail lines of expanded entries
		uint64_t m_RowsVersion = UINT64_MAX; // The journal version m_Rows was built from
		std::string m_RowsKey;               // The filters (and the expansions' version) m_Rows was built with
		std::unordered_set<uint64_t> m_Expanded; // Entries whose parameters and result show below them
		uint64_t m_ExpandedVersion = 0;
		uint64_t m_ScrollToExpanded = 0; // An entry just expanded, whose detail lines scroll into view at the next draw
		// Compact parameters of entries, by id, made once (parameters never change); pruned with the journal's ring.
		std::unordered_map<uint64_t, std::string> m_ParameterPreviews;
		// Compact results of expanded entries, by id, with the status they were made for.
		std::unordered_map<uint64_t, std::pair<CommandStatus, std::string>> m_ResultPreviews;
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
