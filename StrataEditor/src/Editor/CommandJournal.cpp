#include "Editor/CommandJournal.h"

#include "Editor/EditorCommands.h"
#include "Editor/SceneEdit.h"
#include "Editor/UndoStack.h"

#include <Strata/Asset/BuiltinAssets.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Reflection/PropertyJson.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iterator>
#include <set>
#include <string_view>
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

		// Appends text as a JSON string: quoted, with quotes, backslashes and control characters escaped (other characters,
		// UTF-8 included, as they are).
		void AppendJsonString(std::string& out, std::string_view text)
		{
			out += '"';
			for (const char character : text)
			{
				switch (character)
				{
					case '"':  out += "\\\""; break;
					case '\\': out += "\\\\"; break;
					case '\n': out += "\\n"; break;
					case '\r': out += "\\r"; break;
					case '\t': out += "\\t"; break;
					default:
						if (static_cast<unsigned char>(character) < 0x20)
							fmt::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned int>(static_cast<unsigned char>(character)));
						else
							out += character;
				}
			}
			out += '"';
		}

		// A string shortened to at most c_SummaryStringBytes (at a UTF-8 character boundary), as a JSON string.
		void AppendShortString(std::string& out, const std::string& text)
		{
			if (text.size() <= c_SummaryStringBytes)
			{
				AppendJsonString(out, text);
				return;
			}
			AppendJsonString(out, ShortenString(text, c_SummaryStringBytes));
		}

		// A container described by its size, as a JSON string: "[3 items]", "{1 key}".
		void AppendContainer(std::string& out, const nlohmann::json& value)
		{
			if (value.is_array())
				fmt::format_to(std::back_inserter(out), "\"[{} {}]\"", value.size(), value.size() == 1 ? "item" : "items");
			else
				fmt::format_to(std::back_inserter(out), "\"{{{} {}}}\"", value.size(), value.size() == 1 ? "key" : "keys");
		}

		void AppendSummaryValue(std::string& out, const nlohmann::json& value)
		{
			switch (value.type())
			{
				case nlohmann::json::value_t::string:
					AppendShortString(out, value.get_ref<const std::string&>());
					break;
				case nlohmann::json::value_t::boolean:
					out += value.get<bool>() ? "true" : "false";
					break;
				case nlohmann::json::value_t::number_integer:
					fmt::format_to(std::back_inserter(out), "{}", value.get<int64_t>());
					break;
				case nlohmann::json::value_t::number_unsigned:
					fmt::format_to(std::back_inserter(out), "{}", value.get<uint64_t>());
					break;
				case nlohmann::json::value_t::number_float:
				{
					// JSON has no infinities or NaN (nlohmann writes them as null too); the shortest form that reads back the same.
					const double number = value.get<double>();
					if (std::isfinite(number))
						fmt::format_to(std::back_inserter(out), "{}", number);
					else
						out += "null";
					break;
				}
				case nlohmann::json::value_t::array:
				case nlohmann::json::value_t::object:
				case nlohmann::json::value_t::binary:
					AppendContainer(out, value);
					break;
				case nlohmann::json::value_t::null:
				case nlohmann::json::value_t::discarded:
					out += "null";
					break;
			}
		}

		// The result as the journal keeps it, as compact JSON text: an object's scalar members (strings shortened) and its
		// containers described by their size; other values the same way. Written straight into one string: every command
		// pays for it, and a summary built as a JSON tree costs allocations per member (Describe parses the text when asked).
		void WriteSummary(std::string& out, const nlohmann::json& value)
		{
			out.clear();
			if (!value.is_object())
			{
				if (value.is_structured())
					AppendContainer(out, value);
				else
					AppendSummaryValue(out, value);
				return;
			}
			out += '{';
			size_t members = 0;
			for (auto it = value.begin(); it != value.end(); ++it)
			{
				if (members > 0)
					out += ',';
				if (members++ == c_SummaryMembers)
				{
					fmt::format_to(std::back_inserter(out), "\"...\":\"{} more members\"", value.size() - c_SummaryMembers);
					break;
				}
				AppendJsonString(out, it.key());
				out += ':';
				AppendSummaryValue(out, it.value());
			}
			out += '}';
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

		// A step of the way to a value inside a result: an object member's key, or else an array index.
		struct PathToken
		{
			const std::string* Key = nullptr;
			size_t Index = 0;
		};

		// The way from the result to the value CollectIds looks at: one token per level it walks down.
		using IdScanPath = std::array<PathToken, c_MaxWalkDepth + 1>;

		// The IDs a result holds by JSON pointer, looking at no more than `budget` nodes. The pointer is written only for the
		// IDs found: every command's result passes through here, and most of its values are no IDs. `path` holds the way to
		// `value` in its first `depth` tokens.
		void CollectIds(const nlohmann::json& value, IdScanPath& path, std::vector<std::pair<std::string, UUID>>& out, size_t& budget, int depth = 0)
		{
			if (budget == 0 || out.size() >= CommandJournal::c_MaxResultIds || depth > c_MaxWalkDepth)
				return;
			budget--;
			if (value.is_string())
			{
				const std::string& text = value.get_ref<const std::string&>();
				if (!IsIdText(text))
					return;
				const std::optional<UUID> id = UUID::FromString(text);
				if (!id || !id->IsValid())
					return;
				std::string pointer;
				for (int level = 0; level < depth; level++)
				{
					const PathToken& token = path[static_cast<size_t>(level)];
					if (token.Key)
					{
						AppendPointerToken(pointer, *token.Key);
					}
					else
					{
						pointer += '/';
						fmt::format_to(std::back_inserter(pointer), "{}", token.Index);
					}
				}
				out.emplace_back(std::move(pointer), *id);
				return;
			}
			PathToken& token = path[static_cast<size_t>(depth)];
			if (value.is_object())
			{
				for (auto it = value.begin(); it != value.end(); ++it)
				{
					token = { &it.key(), 0 };
					CollectIds(it.value(), path, out, budget, depth + 1);
				}
			}
			else if (value.is_array())
			{
				for (size_t index = 0; index < value.size(); index++)
				{
					token = { nullptr, index };
					CollectIds(value[index], path, out, budget, depth + 1);
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

	uint64_t CommandJournal::Begin(const CommandOrigin& origin, std::string_view command, EditorCommandKind kind, EditorCommandResults results,
		const nlohmann::json& parameters, bool playing)
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
		entry.Results = results;
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
		m_SeenUndoSteps = std::max(m_SeenUndoSteps, undo.GetStepCount());
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
		const uint64_t steps = undo.GetStepCount();
		if (records > m_SeenUndoRecords)
		{
			// Steps count once however many merges made them (a drag of a hundred frames is one edit); merges alone, into a step
			// recorded before, count as one edit more.
			m_EditsOutsideCommands += std::max<uint64_t>(steps > m_SeenUndoSteps ? steps - m_SeenUndoSteps : 0, 1);
		}
		m_SeenUndoRecords = records;
		m_SeenUndoSteps = std::max(m_SeenUndoSteps, steps);
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

	bool CommandJournal::Resume(uint64_t id)
	{
		const CommandJournalEntry* entry = id != 0 ? Find(id) : nullptr;
		if (!entry || entry->Status != CommandStatus::Pending || std::find(m_Running.begin(), m_Running.end(), id) != m_Running.end())
			return false;
		m_Running.push_back(id);
		return true;
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
			WriteSummary(entry->ResultSummary, result.Value);
			if (result.Value.is_object())
			{
				if (const auto warning = result.Value.find("warning"); warning != result.Value.end() && warning->is_string())
					entry->Warning = ShortenString(warning->get<std::string>(), c_MaxErrorBytes);
			}
			entry->RunningCopy = result.RunningCopy;
			// IDs of results that follow the session are never bound by replays: not collected.
			if (entry->Results == EditorCommandResults::Content)
			{
				size_t budget = c_IdScanNodes;
				IdScanPath path;
				CollectIds(result.Value, path, entry->ResultIds, budget);
			}
			added += entry->ResultSummary.size() + entry->Warning.size();
			for (const auto& [idPointer, value] : entry->ResultIds)
				added += idPointer.size() + sizeof(value) + c_JsonNodeBytes;
		}
		entry->Bytes += added;
		m_Bytes += added;
		m_Version++;
		// What an agent changed, not what it looked at: a query after an edit leaves the chip naming the edit.
		const bool mutating = entry->Kind == EditorCommandKind::Edit || entry->RunningCopy;
		if (entry->Source == CommandSource::Automation && entry->Parent == 0 && mutating)
		{
			m_LatestAutomation.Id = entry->Id;
			m_LatestAutomation.Command = entry->Command;
			m_LatestAutomation.Client = entry->Client;
			m_LatestAutomation.Succeeded = result.Success;
			m_LatestAutomation.Finished = std::chrono::steady_clock::now();
			m_LatestAutomation.Entities = result.Success ? entry->Entities : std::vector<UUID> {};
		}
		Trim();
	}

	const CommandJournalEntry* CommandJournal::Find(uint64_t id) const
	{
		// The command being recorded is nearly always the newest: found without touching the rest of the ring, which the
		// command's own work has pushed out of the caches.
		if (!m_Entries.empty() && m_Entries.back().Id == id)
			return &m_Entries.back();
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

	uint64_t CommandJournal::RecordEvent(std::string_view name, const nlohmann::json& details, bool playing)
	{
		const uint64_t id = Begin(CommandOrigin {}, name, EditorCommandKind::Session, EditorCommandResults::Session, details, playing);
		if (CommandJournalEntry* entry = FindMutable(id))
			entry->Event = true;
		Complete(id, EditorCommandResult::Ok());
		return id;
	}

	std::string CommandJournal::Summarize(const CommandJournalEntry& entry)
	{
		if (entry.Status == CommandStatus::Pending)
			return "running";
		if (entry.Status == CommandStatus::Failed)
		{
			const std::string_view error = entry.Error;
			return std::string(error.substr(0, error.find('\n')));
		}
		if (!entry.Warning.empty())
			return entry.Warning;
		// A command that replaced the edited scene with force threw away unsaved changes (kept as recovery data). The result
		// summary is compact JSON this journal wrote, so the member reads exactly so.
		if (entry.ResultSummary.find("\"discardedChanges\":true") != std::string::npos)
			return "discarded changes";
		if (!entry.UndoStep.empty())
			return entry.UndoStep;
		return "done";
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
			{ "summary", Summarize(entry) },
			{ "mutating", entry.Kind == EditorCommandKind::Edit || entry.RunningCopy },
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
		if (entry.Event)
			description["event"] = true;
		if (entry.RunningCopy)
			description["runningCopy"] = true;
		if (entry.Status == CommandStatus::Failed)
			description["error"] = { { "kind", EditorCommandErrorToString(entry.ErrorKind) }, { "message", entry.Error } };
		if (entry.Status == CommandStatus::Succeeded)
		{
			// The summary is JSON text written by the journal; text that does not read back (a result with invalid UTF-8) shows
			// as it is.
			std::optional<nlohmann::json> summary = JsonUtils::Parse(entry.ResultSummary);
			description["result"] = summary ? std::move(*summary) : nlohmann::json(entry.ResultSummary);
		}
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
			// A script that misses the first commands of what it repeats would not build the same scene.
			return fail(fmt::format("The journal keeps at most {} commands and {} MB of them: entries #{} to #{} are gone, so a script from #{} "
				"would miss them. Export from since {} or later", c_MaxEntries, c_MaxBytes / (1024 * 1024), since + 1, m_Entries.front().Id - 1,
				since + 1, m_Entries.front().Id - 1));
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
			// Edits of a playing scene's running copy went with play.stop; those that outlast play mode (assets, documents,
			// scene.save) are repeated like any other.
			const bool edit = entry.Kind == EditorCommandKind::Edit && !entry.RunningCopy;
			if (entry.Kind == EditorCommandKind::Edit && entry.RunningCopy)
				script.SkippedWhilePlaying++;
			// Other commands that recorded an undo step changed the scene in a way the script cannot repeat (scene.recover
			// applies a recovery file the copy does not have).
			if (entry.Kind != EditorCommandKind::Edit && entry.UndoSerial != 0)
				script.UnrepeatableEdits++;
			if (edit && entry.ParametersTruncated)
			{
				return fail(fmt::format("The parameters of #{} ({}) were too large to keep in the journal, so it cannot be repeated: export from since {} "
					"or later", entry.Id, entry.Command, entry.Id));
			}
			parameters[index] = ReplaceProducedIds(entry.Parameters, produced, literal, references[index], entryIds);
			needed[index] = edit;
			// Results of edits and queries give IDs a replay takes from its own run. Those of session commands (the projects and
			// scenes they open), of edits of the running copy and queries while playing (the running copy's entities have the
			// edited scene's IDs, and what play created is gone after play.stop) and the handles of built-in assets name what
			// every run has: they stay as they are. Queries whose results follow the session (the selection) collect no IDs.
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
