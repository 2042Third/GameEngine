#include "Editor/CommandJournal.h"

#include "Editor/EditorCommands.h"
#include "Editor/SceneEdit.h"
#include "Editor/UndoStack.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Reflection/PropertyJson.h>

#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Strata
{

	namespace
	{

		// What an entry holds besides its JSON and strings.
		constexpr size_t c_EntryOverhead = sizeof(CommandJournalEntry) + 64;
		// Bytes a JSON value costs beyond its strings (the node and its place in its container).
		constexpr size_t c_JsonNodeBytes = 48;
		// Shortened parameters: strings and arrays cut to these lengths, containers deeper than this replaced.
		constexpr size_t c_ShortStringBytes = 1024;
		constexpr size_t c_ShortArrayElements = 64;
		constexpr int c_ShortMaxDepth = 32;
		// Result summaries: scalars of the top level, strings cut to this length, at most this many members.
		constexpr size_t c_SummaryStringBytes = 256;
		constexpr size_t c_SummaryMembers = 32;
		// Nodes of a result the ID scan looks at, and how deep it and the size estimate go (deeper values count as too
		// large to keep and hold no IDs a replay could use).
		constexpr size_t c_IdScanNodes = 4096;
		constexpr int c_MaxWalkDepth = 64;
		constexpr size_t c_MaxErrorBytes = 4096;

		int64_t GetUnixMilliseconds()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		}

		// Adds about how much memory a JSON value holds to `bytes`, stopping once that is over `limit`. Recursive rather than
		// with a stack of its own: every command's parameters pass through here, and most are a few values.
		void AddEstimatedBytes(const nlohmann::json& value, size_t limit, size_t& bytes, int depth = 0)
		{
			bytes += c_JsonNodeBytes;
			if (depth > c_MaxWalkDepth)
			{
				bytes = std::max(bytes, limit + 1);
				return;
			}
			if (value.is_string())
			{
				bytes += value.get_ref<const std::string&>().size();
			}
			else if (value.is_object())
			{
				for (auto it = value.begin(); it != value.end() && bytes <= limit; ++it)
				{
					bytes += it.key().size();
					AddEstimatedBytes(it.value(), limit, bytes, depth + 1);
				}
			}
			else if (value.is_array())
			{
				for (auto it = value.begin(); it != value.end() && bytes <= limit; ++it)
					AddEstimatedBytes(*it, limit, bytes, depth + 1);
			}
		}

		// About how much memory a JSON value holds; once it is over `limit`, the result is just above it.
		size_t EstimateBytes(const nlohmann::json& value, size_t limit)
		{
			size_t bytes = 0;
			AddEstimatedBytes(value, limit, bytes);
			return bytes;
		}

		std::string ShortenString(const std::string& text, size_t maxBytes)
		{
			if (text.size() <= maxBytes)
				return text;
			// Cut at a UTF-8 character boundary.
			size_t end = maxBytes;
			while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
				end--;
			return fmt::format("{}... ({} bytes)", text.substr(0, end), text.size());
		}

		// A copy without long strings and long arrays (parameters too large to keep whole).
		nlohmann::json Shorten(const nlohmann::json& value, int depth)
		{
			if (value.is_string())
				return ShortenString(value.get_ref<const std::string&>(), c_ShortStringBytes);
			if (!value.is_structured())
				return value;
			if (depth >= c_ShortMaxDepth)
				return value.is_object() ? "{...}" : "[...]";
			if (value.is_object())
			{
				nlohmann::json shortened = nlohmann::json::object();
				for (const auto& [key, member] : value.items())
					shortened[key] = Shorten(member, depth + 1);
				return shortened;
			}
			nlohmann::json shortened = nlohmann::json::array();
			for (size_t index = 0; index < value.size() && index < c_ShortArrayElements; index++)
				shortened.push_back(Shorten(value[index], depth + 1));
			if (value.size() > c_ShortArrayElements)
				shortened.push_back(fmt::format("... ({} elements)", value.size()));
			return shortened;
		}

		std::string DescribeContainer(const nlohmann::json& value)
		{
			if (value.is_array())
				return fmt::format("[{} {}]", value.size(), value.size() == 1 ? "item" : "items");
			return fmt::format("{{{} {}}}", value.size(), value.size() == 1 ? "key" : "keys");
		}

		// The result as the journal keeps it: an object's scalar members (strings shortened) and its containers described
		// by their size; other values the same way.
		nlohmann::json Summarize(const nlohmann::json& value)
		{
			if (value.is_string())
				return ShortenString(value.get_ref<const std::string&>(), c_SummaryStringBytes);
			if (value.is_array())
				return DescribeContainer(value);
			if (!value.is_object())
				return value;
			nlohmann::json summary = nlohmann::json::object();
			size_t members = 0;
			for (const auto& [key, member] : value.items())
			{
				if (members++ == c_SummaryMembers)
				{
					summary["..."] = fmt::format("{} more members", value.size() - c_SummaryMembers);
					break;
				}
				if (member.is_string())
					summary[key] = ShortenString(member.get_ref<const std::string&>(), c_SummaryStringBytes);
				else if (member.is_structured())
					summary[key] = DescribeContainer(member);
				else
					summary[key] = member;
			}
			return summary;
		}

		bool IsIdText(const std::string& text)
		{
			return text.size() == 16 && std::all_of(text.begin(), text.end(), [](char character)
			{
				return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
			});
		}

		void AppendPointerToken(std::string& pointer, std::string_view token)
		{
			pointer += '/';
			for (const char character : token)
			{
				if (character == '~')
					pointer += "~0";
				else if (character == '/')
					pointer += "~1";
				else
					pointer += character;
			}
		}

		// The IDs a result holds by JSON pointer, looking at no more than `budget` nodes.
		void CollectIds(const nlohmann::json& value, std::string& pointer, std::vector<std::pair<std::string, UUID>>& out, size_t& budget, int depth = 0)
		{
			if (budget == 0 || out.size() >= CommandJournal::c_MaxResultIds || depth > c_MaxWalkDepth)
				return;
			budget--;
			if (value.is_string())
			{
				const std::string& text = value.get_ref<const std::string&>();
				if (IsIdText(text))
				{
					if (const std::optional<UUID> id = UUID::FromString(text); id && id->IsValid())
						out.emplace_back(pointer, *id);
				}
				return;
			}
			const size_t length = pointer.size();
			if (value.is_object())
			{
				for (const auto& [key, member] : value.items())
				{
					AppendPointerToken(pointer, key);
					CollectIds(member, pointer, out, budget, depth + 1);
					pointer.resize(length);
				}
			}
			else if (value.is_array())
			{
				for (size_t index = 0; index < value.size(); index++)
				{
					pointer += '/';
					pointer += std::to_string(index);
					CollectIds(value[index], pointer, out, budget, depth + 1);
					pointer.resize(length);
				}
			}
		}

		// A binding's name for an ID a step's result holds at a pointer: "e12.id", "e12.entities.0". Distinct pointers get
		// distinct names: '/' becomes '.', letters and digits stay, and every other character becomes '-' and its two
		// hexadecimal digits. Binding names have at most 64 characters: a longer one is the entry and the pointer's hash.
		std::string MakeBindingName(uint64_t entry, const std::string& pointer)
		{
			std::string name = fmt::format("e{}", entry);
			for (const char character : pointer)
			{
				if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9'))
					name += character;
				else if (character == '/')
					name += '.';
				else
					name += fmt::format("-{:02X}", static_cast<unsigned char>(character));
			}
			if (name.size() > 64)
				name = fmt::format("e{}-H{:016X}", entry, static_cast<uint64_t>(std::hash<std::string> {}(pointer)));
			return name;
		}

		struct ReplayReference
		{
			size_t Producer = 0; // Index among the looked-at entries
			std::string Pointer;
		};

		// The parameters with every ID a result of the session gave replaced by a reference to it; IDs no result gave yet are
		// what the project had, kept as they are (and remembered as such).
		nlohmann::json ReplaceProducedIds(const nlohmann::json& value, const std::unordered_map<UUID, ReplayReference>& produced, std::unordered_set<UUID>& literal,
			std::vector<ReplayReference>& references, const std::vector<uint64_t>& entryIds)
		{
			if (value.is_string())
			{
				const std::string& text = value.get_ref<const std::string&>();
				if (!IsIdText(text))
					return value;
				const std::optional<UUID> id = UUID::FromString(text);
				if (!id || !id->IsValid())
					return value;
				const auto producer = produced.find(*id);
				if (producer == produced.end())
				{
					literal.insert(*id);
					return value;
				}
				references.push_back(producer->second);
				return nlohmann::json { { "$ref", MakeBindingName(entryIds[producer->second.Producer], producer->second.Pointer) } };
			}
			if (value.is_object())
			{
				nlohmann::json replaced = nlohmann::json::object();
				for (const auto& [key, member] : value.items())
					replaced[key] = ReplaceProducedIds(member, produced, literal, references, entryIds);
				return replaced;
			}
			if (value.is_array())
			{
				nlohmann::json replaced = nlohmann::json::array();
				for (const nlohmann::json& element : value)
					replaced.push_back(ReplaceProducedIds(element, produced, literal, references, entryIds));
				return replaced;
			}
			return value;
		}

	}

	const char* CommandSourceToString(CommandSource source)
	{
		switch (source)
		{
			case CommandSource::UI:            return "UI";
			case CommandSource::Automation:    return "Automation";
			case CommandSource::CommandScript: return "CommandScript";
			case CommandSource::Internal:      return "Internal";
		}
		return "Unknown";
	}

	std::optional<CommandSource> CommandSourceFromString(std::string_view text)
	{
		for (const CommandSource source : { CommandSource::UI, CommandSource::Automation, CommandSource::CommandScript, CommandSource::Internal })
		{
			if (text == CommandSourceToString(source))
				return source;
		}
		return std::nullopt;
	}

	const char* CommandStatusToString(CommandStatus status)
	{
		switch (status)
		{
			case CommandStatus::Pending:   return "Pending";
			case CommandStatus::Succeeded: return "Succeeded";
			case CommandStatus::Failed:    return "Failed";
		}
		return "Unknown";
	}

	uint64_t CommandJournal::Begin(const CommandOrigin& origin, std::string_view command, EditorCommandKind kind, const nlohmann::json& parameters, bool playing)
	{
		if (!m_Enabled)
			return 0;
		CommandJournalEntry entry;
		entry.Id = m_NextId++;
		entry.Time = GetUnixMilliseconds();
		entry.Started = std::chrono::steady_clock::now();
		// A command that runs inside another one belongs to it, whoever the outer one came from.
		if (!m_Running.empty())
		{
			entry.Parent = m_Running.back();
			entry.Source = CommandSource::Internal;
		}
		else
		{
			entry.Source = origin.Source;
			entry.Client = origin.Client;
		}
		entry.Command = command;
		entry.Kind = kind;
		entry.Playing = playing;
		entry.EditsOutsideCommandsBefore = m_EditsOutsideCommands;
		const size_t parameterBytes = EstimateBytes(parameters, c_MaxParameterBytes);
		if (parameterBytes > c_MaxParameterBytes)
		{
			entry.Parameters = Shorten(parameters, 0);
			entry.ParametersTruncated = true;
			entry.Bytes = EstimateBytes(entry.Parameters, c_MaxBytes);
		}
		else
		{
			entry.Parameters = parameters;
			entry.Bytes = parameterBytes;
		}
		entry.Bytes += c_EntryOverhead + entry.Command.size() + entry.Client.size();
		m_Bytes += entry.Bytes;
		m_Running.push_back(entry.Id);
		m_Entries.push_back(std::move(entry));
		m_Version++;
		Trim();
		return m_NextId - 1;
	}

	void CommandJournal::NoteUndo(uint64_t id, const UndoStack& undo, uint64_t recordCountBefore)
	{
		m_SeenUndoRecords = std::max(m_SeenUndoRecords, undo.GetRecordCount());
		if (id == 0 || undo.GetRecordCount() == recordCountBefore)
			return;
		CommandJournalEntry* entry = FindMutable(id);
		const uint64_t serial = undo.GetLastRecordedSerial();
		const std::optional<size_t> index = serial != 0 ? undo.FindSerial(serial) : std::nullopt;
		if (!entry || !index)
			return;
		const EditorAction& action = undo.GetAction(*index);
		entry->UndoStep = action.GetName();
		entry->UndoSerial = serial;
		entry->Entities.clear();
		entry->EntityCount = 0;
		if (const Ref<const std::vector<EntityState>> states = action.GetEntityStates(false))
		{
			entry->EntityCount = states->size();
			for (size_t state = 0; state < states->size() && state < c_MaxEntities; state++)
				entry->Entities.push_back((*states)[state].ID);
		}
		const size_t added = entry->UndoStep.size() + entry->Entities.size() * sizeof(UUID);
		entry->Bytes += added;
		m_Bytes += added;
		m_Version++;
	}

	void CommandJournal::NoteEditsOutsideCommands(const UndoStack& undo)
	{
		if (!m_Running.empty())
			return;
		const uint64_t records = undo.GetRecordCount();
		if (records > m_SeenUndoRecords)
			m_EditsOutsideCommands += records - m_SeenUndoRecords;
		m_SeenUndoRecords = records;
	}

	void CommandJournal::NotePoll(uint64_t id)
	{
		if (CommandJournalEntry* entry = id != 0 ? FindMutable(id) : nullptr)
			entry->Frames++;
	}

	void CommandJournal::Suspend(uint64_t id)
	{
		if (const auto it = std::find(m_Running.rbegin(), m_Running.rend(), id); it != m_Running.rend())
			m_Running.erase(std::next(it).base());
	}

	void CommandJournal::Complete(uint64_t id, const EditorCommandResult& result)
	{
		if (id == 0)
			return;
		Suspend(id);
		CommandJournalEntry* entry = FindMutable(id);
		if (!entry || entry->Status != CommandStatus::Pending)
			return;
		entry->Status = result.Success ? CommandStatus::Succeeded : CommandStatus::Failed;
		entry->DurationMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - entry->Started).count();
		size_t added = 0;
		if (!result.Success)
		{
			entry->ErrorKind = result.ErrorKind;
			entry->Error = ShortenString(result.Error, c_MaxErrorBytes);
			added += entry->Error.size();
		}
		else
		{
			entry->Result = Summarize(result.Value);
			if (result.Value.is_object())
			{
				if (const auto warning = result.Value.find("warning"); warning != result.Value.end() && warning->is_string())
					entry->Warning = ShortenString(warning->get<std::string>(), c_MaxErrorBytes);
			}
			std::string pointer;
			size_t budget = c_IdScanNodes;
			CollectIds(result.Value, pointer, entry->ResultIds, budget);
			added += EstimateBytes(entry->Result, c_MaxBytes) + entry->Warning.size();
			for (const auto& [idPointer, value] : entry->ResultIds)
				added += idPointer.size() + sizeof(value) + c_JsonNodeBytes;
		}
		entry->Bytes += added;
		m_Bytes += added;
		m_Version++;
		Trim();
	}

	const CommandJournalEntry* CommandJournal::Find(uint64_t id) const
	{
		// Ids grow with the position: a binary search finds an entry in the full ring.
		const auto it = std::lower_bound(m_Entries.begin(), m_Entries.end(), id, [](const CommandJournalEntry& entry, uint64_t value) { return entry.Id < value; });
		return it != m_Entries.end() && it->Id == id ? &*it : nullptr;
	}

	CommandJournalEntry* CommandJournal::FindMutable(uint64_t id)
	{
		return const_cast<CommandJournalEntry*>(std::as_const(*this).Find(id));
	}

	void CommandJournal::Trim()
	{
		// The newest entry always stays.
		while (m_Entries.size() > 1 && (m_Entries.size() > c_MaxEntries || m_Bytes > c_MaxBytes))
		{
			m_Bytes -= std::min(m_Bytes, m_Entries.front().Bytes);
			m_Entries.pop_front();
		}
	}

	void CommandJournal::Clear()
	{
		m_Entries.clear();
		m_Bytes = 0;
		m_Version++;
	}

	nlohmann::json CommandJournal::Describe(const CommandJournalEntry& entry)
	{
		nlohmann::json entities = nlohmann::json::array();
		for (const UUID id : entry.Entities)
			entities.push_back(UUIDToJson(id));
		nlohmann::json description = {
			{ "id", entry.Id },
			{ "time", entry.Time },
			{ "source", CommandSourceToString(entry.Source) },
			{ "command", entry.Command },
			{ "kind", EditorCommandKindToString(entry.Kind) },
			{ "mutating", entry.Kind == EditorCommandKind::Edit },
			{ "parameters", entry.Parameters },
			{ "status", CommandStatusToString(entry.Status) },
			{ "durationMs", entry.DurationMs },
			{ "entities", std::move(entities) },
			{ "entityCount", entry.EntityCount } };
		if (entry.Parent != 0)
			description["parent"] = entry.Parent;
		if (!entry.Client.empty())
			description["client"] = entry.Client;
		if (entry.ParametersTruncated)
			description["parametersTruncated"] = true;
		if (entry.Playing)
			description["playing"] = true;
		if (entry.Status == CommandStatus::Failed)
			description["error"] = { { "kind", EditorCommandErrorToString(entry.ErrorKind) }, { "message", entry.Error } };
		if (entry.Status == CommandStatus::Succeeded)
			description["result"] = entry.Result;
		if (entry.Frames > 0)
			description["frames"] = entry.Frames;
		if (!entry.UndoStep.empty())
			description["undoStep"] = entry.UndoStep;
		if (!entry.Warning.empty())
			description["warning"] = entry.Warning;
		return description;
	}

	std::optional<ReplayScript> CommandJournal::BuildReplayScript(uint64_t since, std::string* outError) const
	{
		auto fail = [outError](std::string message) -> std::optional<ReplayScript>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};
		if (!m_Entries.empty() && m_Entries.front().Id > since + 1)
		{
			return fail(fmt::format("The journal keeps the latest {} commands: entries #{} to #{} are gone, so a script from #{} would miss them. "
				"Export from since {} or later", c_MaxEntries, since + 1, m_Entries.front().Id - 1, since + 1, m_Entries.front().Id - 1));
		}

		// The commands of the session that succeeded, at the top level (what commands ran inside them repeats with them).
		std::vector<const CommandJournalEntry*> entries;
		for (const CommandJournalEntry& entry : m_Entries)
		{
			if (entry.Id > since && entry.Parent == 0 && entry.Status == CommandStatus::Succeeded)
				entries.push_back(&entry);
		}
		std::vector<uint64_t> entryIds;
		for (const CommandJournalEntry* entry : entries)
			entryIds.push_back(entry->Id);

		ReplayScript script;
		script.Since = since;
		script.Latest = m_Entries.empty() ? since : m_Entries.back().Id;
		// Counted from the entry `since` on (the first one held when the ring starts right after it).
		const auto first = std::find_if(m_Entries.begin(), m_Entries.end(), [since](const CommandJournalEntry& entry) { return entry.Id >= since; });
		script.EditsOutsideCommands = m_EditsOutsideCommands - (first != m_Entries.end() && since != 0 ? first->EditsOutsideCommandsBefore : 0);

		// Forward: which ID each result gave first, and what each command's parameters refer to.
		std::unordered_map<UUID, ReplayReference> produced;
		std::unordered_set<UUID> literal;
		std::vector<nlohmann::json> parameters(entries.size());
		std::vector<std::vector<ReplayReference>> references(entries.size());
		std::vector<bool> needed(entries.size(), false);
		for (size_t index = 0; index < entries.size(); index++)
		{
			const CommandJournalEntry& entry = *entries[index];
			const bool edit = entry.Kind == EditorCommandKind::Edit && !entry.Playing;
			if (entry.Kind == EditorCommandKind::Edit && entry.Playing)
				script.SkippedWhilePlaying++;
			if (edit && entry.ParametersTruncated)
			{
				return fail(fmt::format("The parameters of #{} ({}) were too large to keep in the journal, so it cannot be repeated: export from since {} "
					"or later", entry.Id, entry.Command, entry.Id));
			}
			parameters[index] = ReplaceProducedIds(entry.Parameters, produced, literal, references[index], entryIds);
			needed[index] = edit;
			// Results of edits and queries give IDs a replay takes from its own run. Those of session commands (the projects and
			// scenes they open), of commands that ran while playing (the running copy's entities have the edited scene's IDs, and
			// what play created is gone after play.stop) and the handles of built-in assets name what every run has: they stay
			// as they are.
			const bool producing = edit || (entry.Kind == EditorCommandKind::Query && !entry.Playing);
			for (const auto& [pointer, id] : entry.ResultIds)
			{
				if (!producing || BuiltinAssets::IsBuiltin(id))
					literal.insert(id);
				else if (!literal.contains(id) && !produced.contains(id))
					produced.emplace(id, ReplayReference { index, pointer });
			}
		}

		// Backward: the steps the edits need for their references, and the values each of those binds.
		std::vector<std::set<std::string>> binds(entries.size());
		for (size_t index = entries.size(); index-- > 0;)
		{
			if (!needed[index])
				continue;
			for (const ReplayReference& reference : references[index])
			{
				needed[reference.Producer] = true;
				binds[reference.Producer].insert(reference.Pointer);
			}
		}

		for (size_t index = 0; index < entries.size(); index++)
		{
			if (!needed[index])
				continue;
			const CommandJournalEntry& entry = *entries[index];
			if (entry.ParametersTruncated)
			{
				return fail(fmt::format("The parameters of #{} ({}), whose result later edits use, were too large to keep in the journal, so it cannot "
					"be repeated: export from since {} or later", entry.Id, entry.Command, entry.Id));
			}
			nlohmann::json step = { { "command", entry.Command } };
			if (!parameters[index].empty())
				step["parameters"] = std::move(parameters[index]);
			if (!binds[index].empty())
			{
				nlohmann::json bind = nlohmann::json::object();
				for (const std::string& pointer : binds[index])
					bind[MakeBindingName(entry.Id, pointer)] = pointer;
				script.Bindings += binds[index].size();
				step["bind"] = std::move(bind);
			}
			if (entry.Kind == EditorCommandKind::Edit)
				script.Edits++;
			else
				script.Lookups++;
			script.Steps.push_back(std::move(step));
		}
		return script;
	}

}
