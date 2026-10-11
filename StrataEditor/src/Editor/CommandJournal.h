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
		bool Mutating = false; // The command changes the scene, its assets or the project (EditorCommandKind::Edit)
		bool Playing = false;  // It ran while the scene played: its scene changes went to the running copy
		// The parameters as given, unless they were larger than CommandJournal::c_MaxParameterBytes: then shortened (long
		// strings and arrays cut) and ParametersTruncated, so the entry cannot be run again exactly.
		nlohmann::json Parameters;
		bool ParametersTruncated = false;
		CommandStatus Status = CommandStatus::Pending;
		EditorCommandError ErrorKind = {};
		std::string Error;
		std::string Warning;   // The result's "warning" (e.g. a change to the running copy while playing)
		nlohmann::json Result; // A summary: the result's scalar members, containers described by their size
		// Every ID (16 hexadecimal digits) the result holds, by JSON pointer (at most c_MaxResultIds): what a replay binds
		// to refer to entities and assets the command made.
		std::vector<std::pair<std::string, UUID>> ResultIds;
		double DurationMs = 0.0; // From the start to the end, frames in between for a deferred command
		uint32_t Frames = 0;     // Frames a deferred command was polled
		// The entities the undo step it recorded touches (at most c_MaxEntities; EntityCount has them all), the step's name
		// and its serial number (UndoStack::FindSerial), or empty and 0 when it recorded none.
		std::vector<UUID> Entities;
		size_t EntityCount = 0;
		std::string UndoStep;
		uint64_t UndoSerial = 0;
		size_t Bytes = 0; // About how much memory the entry holds
		std::chrono::steady_clock::time_point Started;
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
		uint64_t Begin(const CommandOrigin& origin, std::string_view command, bool mutating, const nlohmann::json& parameters, bool playing);
		// Notes the undo step a command recorded: when `undo` recorded or merged a step since `recordCountBefore`
		// (UndoStack::GetRecordCount), its latest step is the command's.
		void NoteUndo(uint64_t id, const UndoStack& undo, uint64_t recordCountBefore);
		// A deferred command was polled once more.
		void NotePoll(uint64_t id);
		// Ends the entry (only the first call counts) and, for a command begun with Begin, ends it as the running command.
		void Complete(uint64_t id, const EditorCommandResult& result);
		// A deferred command's handler returned: it is no longer the running command (its entry stays pending).
		void Suspend(uint64_t id);

		const std::deque<CommandJournalEntry>& GetEntries() const { return m_Entries; }
		const CommandJournalEntry* Find(uint64_t id) const;
		// The innermost command whose handler runs now (0 when none), e.g. editor.activity's own entry.
		uint64_t GetRunningId() const { return m_Running.empty() ? 0 : m_Running.back(); }
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
	private:
		CommandJournalEntry* FindMutable(uint64_t id);
		void Trim();
	private:
		std::deque<CommandJournalEntry> m_Entries; // Oldest first, ids increasing
		std::vector<uint64_t> m_Running;           // The commands whose handlers run, innermost last
		uint64_t m_NextId = 1;
		uint64_t m_Version = 0;
		size_t m_Bytes = 0;
		bool m_Enabled = true;
	};

}
