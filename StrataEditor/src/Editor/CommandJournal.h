#pragma once

#include <Strata/Core/UUID.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strata
{

	class UndoStack;
	enum class EditorCommandError : uint8_t;
	struct EditorCommandResult;

	// What a command does, for the activity journal and replays (editor.activity.export repeats Edit commands only).
	enum class EditorCommandKind : uint8_t
	{
		Edit = 0, // Changes the edited scene, its assets or the project's files: what replaying a session repeats
		Query,    // Reads, waits or renders: changes nothing (captures included)
		Session   // Changes the editor's session, not the project: which project is open, play mode, simulated input,
		          // the selection, the view, the editor's own files
	};

	const char* EditorCommandKindToString(EditorCommandKind kind);

	// What the IDs a command's results show depend on, for replays (CommandJournal::BuildReplayScript binds IDs that results
	// gave for later steps).
	enum class EditorCommandResults : uint8_t
	{
		Content = 0, // The project's content (entities, assets, scenes): a replay of the session gets them from its own run
		Session      // The editor's session (the selection, the journal, what is loaded, running or shown), which a replay does
		             // not repeat: the IDs they show are never bound
	};

	// Who ran a command (the activity journal, CommandJournal).
	enum class CommandSource : uint8_t
	{
		UI = 0,        // The person, through the editor's interface
		Automation,    // A tool or AI agent through the automation server (EditorAutomation)
		CommandScript, // A --commands script (EditorCommandScript)
		Internal       // The editor itself, or a command another command ran
	};

	const char* CommandSourceToString(CommandSource source);
	std::optional<CommandSource> CommandSourceFromString(std::string_view text);

	// Who runs a command: EditorCommandRegistry::Execute and EditorCommandRunner::Run take it and the journal records it.
	struct CommandOrigin
	{
		CommandSource Source = CommandSource::Internal;
		std::string Client; // Automation: the client's name from its handshake (empty when it gave none)
	};

	enum class CommandStatus : uint8_t
	{
		Pending = 0, // A command that finishes over the next frames, still running
		Succeeded,
		Failed
	};

	const char* CommandStatusToString(CommandStatus status);

	// One command the editor ran.
	struct CommandJournalEntry
	{
		uint64_t Id = 0;
		uint64_t Parent = 0; // The entry of the command that ran this one (source Internal); 0 for none
		int64_t Time = 0;    // When it started: milliseconds since 1970-01-01 UTC
		CommandSource Source = CommandSource::Internal;
		std::string Client;
		std::string Command;
		EditorCommandKind Kind = EditorCommandKind::Edit;
		EditorCommandResults Results = EditorCommandResults::Content;
		bool Playing = false;     // It ran while the scene played
		bool RunningCopy = false; // It changed the running copy of the playing scene (EditorCommandResult::RunningCopy)
		// The parameters as given, unless they were larger than CommandJournal::c_MaxParameterBytes: then shortened (long
		// strings and arrays cut) and ParametersTruncated, so the entry cannot be run again exactly.
		nlohmann::json Parameters;
		bool ParametersTruncated = false;
		CommandStatus Status = CommandStatus::Pending;
		EditorCommandError ErrorKind = {};
		std::string Error;
		std::string Warning;   // The result's "warning" (e.g. a change to the running copy while playing)
		// A summary of the result as compact JSON text (Describe reads it): the result's scalar members, strings shortened, and
		// its containers described by their size.
		std::string ResultSummary;
		// Every ID (16 hexadecimal digits) the result holds, by JSON pointer (at most c_MaxResultIds): what a replay binds
		// to refer to entities and assets the command made. Empty for commands whose results follow the session.
		std::vector<std::pair<std::string, UUID>> ResultIds;
		double DurationMs = 0.0; // From the start to the end, frames in between for a deferred command
		uint32_t Frames = 0;     // Frames a deferred command was polled
		// The entities the undo step it recorded touches (at most c_MaxEntities; EntityCount has them all), the step's name
		// and its serial number (UndoStack::FindSerial), or empty and 0 when it recorded none.
		std::vector<UUID> Entities;
		size_t EntityCount = 0;
		std::string UndoStep;
		uint64_t UndoSerial = 0;
		uint64_t EditsOutsideCommandsBefore = 0; // CommandJournal::GetEditsOutsideCommands when it began
		size_t Bytes = 0; // About how much memory the entry holds
		std::chrono::steady_clock::time_point Started;
	};

	// The latest command an automation client ran, once it finished (CommandJournal::GetLatestAutomationActivity): what the
	// viewport's agent chip names and the entities it outlines for a moment.
	struct AutomationActivity
	{
		uint64_t Id = 0; // Its journal entry; 0 before the first
		std::string Command;
		std::string Client; // The name the client gave, empty when it gave none
		bool Succeeded = false;
		std::chrono::steady_clock::time_point Finished;
		std::vector<UUID> Entities; // What its undo step touched (at most CommandJournal::c_MaxEntities), when it succeeded
	};

	// A command script made from the journal (CommandJournal::BuildReplayScript).
	struct ReplayScript
	{
		nlohmann::json Steps = nlohmann::json::array(); // As EditorCommandScript reads them
		size_t Edits = 0;               // Commands of kind Edit replayed
		size_t Lookups = 0;             // Queries included because later steps use IDs their results gave
		size_t Bindings = 0;            // Values bound for later steps
		size_t SkippedWhilePlaying = 0; // Edits of the running copy, which play.stop discarded
		uint64_t EditsOutsideCommands = 0; // Edits no command made (CommandJournal::GetEditsOutsideCommands): not in it
		// Undo steps that commands a script does not repeat recorded (scene.recover: what it applied came from a recovery file
		// the copy does not have): not in it either
		size_t UnrepeatableEdits = 0;
		uint64_t Since = 0;
		uint64_t Latest = 0;            // The last entry looked at
	};

	// The activity journal: every command the editor ran, by whom, with what, how it went, how long it took and which
	// entities and undo step it touched, in a ring of at most c_MaxEntries and c_MaxBytes (the oldest go first).
	// EditorCommandRegistry::Execute begins an entry for each command and completes it when the command finishes at once;
	// a deferred command's entry is completed when its result becomes final (PollCommandResult) or it is cancelled
	// (EditorCommandRunner), so each command is recorded exactly once with its real end. Recording costs a few
	// microseconds: parameters are copied (a bounded copy above c_MaxParameterBytes), results summarized with a bounded walk.
	// editor.activity reads it, editor.activity.export turns it into a command script, the Activity panel shows it.
	// Main thread only.
	class CommandJournal
	{
	public:
		static constexpr size_t c_MaxEntries = 2000;
		static constexpr size_t c_MaxBytes = 8 * 1024 * 1024;
		// Parameters up to this size are kept whole (a replay needs them); larger ones are shortened.
		static constexpr size_t c_MaxParameterBytes = 256 * 1024;
		static constexpr size_t c_MaxEntities = 256;
		static constexpr size_t c_MaxResultIds = 1024;

		// Starts an entry and returns its id (0 while the journal is disabled). A command begun while another one runs is
		// that one's child, from source Internal, whatever origin it was given.
		uint64_t Begin(const CommandOrigin& origin, std::string_view command, EditorCommandKind kind, EditorCommandResults results,
			const nlohmann::json& parameters, bool playing);
		// Notes the undo step a command recorded: when `undo` recorded or merged a step since `recordCountBefore`
		// (UndoStack::GetRecordCount), its latest step is the command's.
		void NoteUndo(uint64_t id, const UndoStack& undo, uint64_t recordCountBefore);
		// Counts what `undo` recorded since the journal last looked as edits outside commands: continuous UI edits (inspector
		// fields, gizmo drags, the material editor) record their steps directly, so the journal does not hold them and a
		// replay cannot repeat them. Called before a top-level command's handler or poll runs (a command run by another one
		// belongs to it); NoteUndo marks what commands record as seen.
		void NoteEditsOutsideCommands(const UndoStack& undo);
		// Edits made outside commands since the journal began: undo steps (one however many merges made it, e.g. a drag), and
		// merges into a step recorded before (one for those between two commands).
		uint64_t GetEditsOutsideCommands() const { return m_EditsOutsideCommands; }
		const AutomationActivity& GetLatestAutomationActivity() const { return m_LatestAutomation; }
		// A deferred command was polled once more.
		void NotePoll(uint64_t id);
		// Ends the entry (only the first call counts) and, for a command begun with Begin, ends it as the running command.
		void Complete(uint64_t id, const EditorCommandResult& result);
		// A deferred command's handler returned: it is no longer the running command (its entry stays pending).
		void Suspend(uint64_t id);
		// A deferred command's poll runs (PollCommandResult): it is the running command again until Suspend or Complete, so
		// commands it runs are its children and work it causes is known to be its (GetOutermostRunningId).
		void Resume(uint64_t id);

		const std::deque<CommandJournalEntry>& GetEntries() const { return m_Entries; }
		const CommandJournalEntry* Find(uint64_t id) const;
		// The innermost command whose handler runs now (0 when none), e.g. editor.activity's own entry.
		uint64_t GetRunningId() const { return m_Running.empty() ? 0 : m_Running.back(); }
		// The outermost command whose handler or poll runs now (0 when none): whom the work going on is for (its source and
		// client; a command run by another one is that one's child).
		uint64_t GetOutermostRunningId() const { return m_Running.empty() ? 0 : m_Running.front(); }
		// Changes with every entry begun or completed (the Activity panel rebuilds its rows then).
		uint64_t GetVersion() const { return m_Version; }
		uint64_t GetLatestId() const { return m_NextId - 1; }
		size_t GetBytes() const { return m_Bytes; }
		// Off, nothing is recorded (the journal's own cost measured against none).
		void SetEnabled(bool enabled) { m_Enabled = enabled; }
		bool IsEnabled() const { return m_Enabled; }
		void Clear();

		// An entry as editor.activity reports it.
		static nlohmann::json Describe(const CommandJournalEntry& entry);

		// A command script that repeats the successful edits (EditorCommandKind::Edit, top level; made while playing only when
		// they outlast play mode, i.e. did not change the running copy) after entry `since`, for a project as it was then: IDs
		// that results of the session gave (entities and assets the session created, which get new IDs every run) are bound
		// where they appeared ("bind") and referenced where they are used ({"$ref"}), including results of queries of the
		// project's content that later edits took IDs from, which join the script; IDs the session used before any such result
		// gave them stay as they are (what the project already had), as do those of results that follow the session (the
		// selection, editor.status: EditorCommandResults::Session). Fails when the journal no longer holds entries after
		// `since`, or when the parameters of a command it needs were too large to keep. Edits outside commands and the undo
		// steps of commands it does not repeat cannot be in it: the script counts them (EditsOutsideCommands,
		// UnrepeatableEdits).
		std::optional<ReplayScript> BuildReplayScript(uint64_t since, std::string* outError = nullptr) const;
	private:
		CommandJournalEntry* FindMutable(uint64_t id);
		void Trim();
	private:
		std::deque<CommandJournalEntry> m_Entries; // Oldest first, ids increasing
		std::vector<uint64_t> m_Running;           // The commands whose handlers run, innermost last
		uint64_t m_NextId = 1;
		uint64_t m_SeenUndoRecords = 0;            // UndoStack::GetRecordCount when the journal last looked
		uint64_t m_SeenUndoSteps = 0;              // UndoStack::GetStepCount then
		uint64_t m_EditsOutsideCommands = 0;
		AutomationActivity m_LatestAutomation;
		uint64_t m_Version = 0;
		size_t m_Bytes = 0;
		bool m_Enabled = true;
	};

}
