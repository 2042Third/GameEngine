#include "Editor/CommandJournal.h"

#include "Editor/EditorCommands.h"
#include "Editor/SceneEdit.h"
#include "Editor/UndoStack.h"

#include <Strata/Reflection/PropertyJson.h>

#include <algorithm>
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

	uint64_t CommandJournal::Begin(const CommandOrigin& origin, std::string_view command, bool mutating, const nlohmann::json& parameters, bool playing)
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
		entry.Mutating = mutating;
		entry.Playing = playing;
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
			{ "mutating", entry.Mutating },
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

}
