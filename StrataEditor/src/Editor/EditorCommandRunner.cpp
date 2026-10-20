#include "Editor/EditorCommandRunner.h"

#include "Editor/EditorContext.h"
#include "Editor/EditorNotifications.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Profiling.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace Strata
{

	namespace
	{

		// Results can be large (entity listings, images): the log keeps the start. Summaries (editor.status, editor.perf) fit
		// whole, so that tools reading a command script's log can parse them.
		constexpr size_t c_MaxLoggedResultSize = 16 * 1024;

		std::string DescribeResultValue(const nlohmann::json& value)
		{
			if (value.is_null())
				return "ok";
			std::string text = JsonUtils::Dump(value);
			if (text.size() > c_MaxLoggedResultSize)
				text = fmt::format("{}... ({} bytes)", text.substr(0, c_MaxLoggedResultSize), text.size());
			return text;
		}

		// The reference tokens of a JSON pointer (RFC 6901: "" or "/token/token", with "~0" for '~' and "~1" for '/'),
		// or nullopt when it is malformed.
		std::optional<std::vector<std::string>> ParseJsonPointer(std::string_view pointer)
		{
			std::vector<std::string> path;
			if (pointer.empty())
				return path;
			if (pointer.front() != '/')
				return std::nullopt;
			size_t start = 1;
			while (true)
			{
				const size_t end = pointer.find('/', start);
				const std::string_view raw = pointer.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
				std::string token;
				for (size_t index = 0; index < raw.size(); index++)
				{
					if (raw[index] != '~')
					{
						token += raw[index];
						continue;
					}
					if (index + 1 >= raw.size() || (raw[index + 1] != '0' && raw[index + 1] != '1'))
						return std::nullopt;
					token += raw[index + 1] == '0' ? '~' : '/';
					index++;
				}
				path.push_back(std::move(token));
				if (end == std::string_view::npos)
					return path;
				start = end + 1;
			}
		}

		// The value at path inside value, or null when there is none.
		const nlohmann::json* ResolveJsonPath(const nlohmann::json& value, const std::vector<std::string>& path)
		{
			const nlohmann::json* current = &value;
			for (const std::string& token : path)
			{
				if (current->is_object())
				{
					current = JsonUtils::Find(*current, token);
					if (!current)
						return nullptr;
					continue;
				}
				// Array indices are decimal numbers without leading zeros.
				if (!current->is_array() || token.empty() || token.size() > 9 || (token.size() > 1 && token[0] == '0')
					|| !std::all_of(token.begin(), token.end(), [](char character) { return character >= '0' && character <= '9'; }))
					return nullptr;
				size_t index = 0;
				for (const char digit : token)
					index = index * 10 + static_cast<size_t>(digit - '0');
				if (index >= current->size())
					return nullptr;
				current = &(*current)[index];
			}
			return current;
		}

		// Why `result` does not meet the expectations, or an empty string when it does.
		std::string CheckExpectations(const nlohmann::json& result, const std::vector<EditorCommandScript::Expectation>& expectations)
		{
			for (const EditorCommandScript::Expectation& expectation : expectations)
			{
				const nlohmann::json* value = ResolveJsonPath(result, expectation.Path);
				if (!value)
					return fmt::format("expected a value at '{}', but the result has none", expectation.Pointer);
				if (expectation.Equals && *value != *expectation.Equals)
					return fmt::format("expected '{}' to be {}, but it is {}", expectation.Pointer, JsonUtils::Dump(*expectation.Equals), JsonUtils::Dump(*value));
				if (!expectation.Min && !expectation.Max)
					continue;
				const double number = value->is_number() ? value->get<double>() : 0.0;
				if (!value->is_number() || std::isnan(number) || (expectation.Min && number < *expectation.Min) || (expectation.Max && number > *expectation.Max))
				{
					return fmt::format("expected '{}' to be a number in [{}, {}], but it is {}", expectation.Pointer,
						expectation.Min ? fmt::format("{}", *expectation.Min) : std::string("-inf"), expectation.Max ? fmt::format("{}", *expectation.Max) : std::string("inf"),
						JsonUtils::Dump(*value));
				}
			}
			return {};
		}

		// The "expect" object of a script step; an error message when it is malformed.
		std::optional<std::string> ParseExpectations(const nlohmann::json& expect, std::vector<EditorCommandScript::Expectation>& out)
		{
			constexpr std::string_view c_Format = "{\"<JSON pointer>\": {\"equals\": value, \"min\": number, \"max\": number}}";
			if (!expect.is_object())
				return fmt::format("\"expect\" must be an object {}", c_Format);
			for (const auto& [pointer, condition] : expect.items())
			{
				EditorCommandScript::Expectation expectation;
				expectation.Pointer = pointer;
				std::optional<std::vector<std::string>> path = ParseJsonPointer(pointer);
				if (!path)
					return fmt::format("'{}' in \"expect\" is not a JSON pointer (\"\" for the whole result, or \"/key/0\")", pointer);
				expectation.Path = std::move(*path);
				if (!condition.is_object() || condition.empty())
					return fmt::format("the condition of '{}' must be an object with \"equals\", \"min\" or \"max\"", pointer);
				for (const auto& [key, value] : condition.items())
				{
					if (key == "equals")
						expectation.Equals = value;
					else if (key == "min" && value.is_number())
						expectation.Min = value.get<double>();
					else if (key == "max" && value.is_number())
						expectation.Max = value.get<double>();
					else
						return fmt::format("invalid condition '{}' for '{}' (\"equals\": value, \"min\" and \"max\": numbers)", key, pointer);
				}
				if (expectation.Min && expectation.Max && *expectation.Min > *expectation.Max)
					return fmt::format("the condition of '{}' has a minimum above its maximum", pointer);
				out.push_back(std::move(expectation));
			}
			return std::nullopt;
		}

		// The "until" object of a script step; an error message when it is malformed.
		std::optional<std::string> ParseUntil(const nlohmann::json& until, EditorCommandScript::UntilBounds& out)
		{
			constexpr std::string_view c_Format = "{\"steps\": fixed steps, \"seconds\": seconds} (at least one)";
			if (!until.is_object() || until.empty())
				return fmt::format("\"until\" must be an object {}", c_Format);
			for (const auto& [key, value] : until.items())
			{
				if (key == "steps")
				{
					if (!value.is_number_integer() || value.get<int64_t>() < 1 || value.get<int64_t>() > static_cast<int64_t>(EditorCommandScript::c_MaxUntilSteps))
						return fmt::format("\"until\": \"steps\" must be an integer from 1 to {}", EditorCommandScript::c_MaxUntilSteps);
					out.Steps = value.get<uint64_t>();
				}
				else if (key == "seconds")
				{
					if (!value.is_number() || !(value.get<double>() > 0.0) || value.get<double>() > EditorCommandScript::c_MaxUntilSeconds)
						return fmt::format("\"until\": \"seconds\" must be a number above 0 and at most {}", EditorCommandScript::c_MaxUntilSeconds);
					out.Seconds = value.get<double>();
				}
				else
				{
					return fmt::format("\"until\": unknown key '{}' (expected {})", key, c_Format);
				}
			}
			return std::nullopt;
		}

		// Turns the activity journal off while it lives, so that the attempts of an "until" step leave no entries of their own.
		class JournalPause
		{
		public:
			explicit JournalPause(CommandJournal& journal)
				: m_Journal(journal), m_WasEnabled(journal.IsEnabled())
			{
				m_Journal.SetEnabled(false);
			}
			~JournalPause() { m_Journal.SetEnabled(m_WasEnabled); }
			JournalPause(const JournalPause&) = delete;
			JournalPause& operator=(const JournalPause&) = delete;
		private:
			CommandJournal& m_Journal;
			bool m_WasEnabled;
		};

		// Binding names: letters, digits, '_', '-' and '.', at most 64 of them.
		bool IsValidBindingName(const std::string& name)
		{
			return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](char character)
			{
				return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')
					|| character == '_' || character == '-' || character == '.';
			});
		}

		// The "bind" object of a script step; an error message when it is malformed.
		std::optional<std::string> ParseBindings(const nlohmann::json& bind, std::vector<EditorCommandScript::Binding>& out)
		{
			if (!bind.is_object())
				return std::string("\"bind\" must be an object {\"<name>\": \"<JSON pointer into the result>\"}");
			for (const auto& [name, pointer] : bind.items())
			{
				if (!IsValidBindingName(name))
					return fmt::format("'{}' in \"bind\" is not a name (letters, digits, '_', '-' and '.', at most 64)", name);
				if (!pointer.is_string())
					return fmt::format("the binding '{}' must be a JSON pointer into the result, e.g. \"/id\"", name);
				std::optional<std::vector<std::string>> path = ParseJsonPointer(pointer.get<std::string>());
				if (!path)
					return fmt::format("'{}' (binding '{}') is not a JSON pointer (\"\" for the whole result, or \"/key/0\")", pointer.get<std::string>(), name);
				out.push_back({ name, pointer.get<std::string>(), std::move(*path) });
			}
			return std::nullopt;
		}

		// Checks the references of a step's parameters against the names earlier steps bind; an error message for a malformed
		// reference or an unknown name. Sets `found` when there is a reference.
		// editor.batch's steps are a script of their own: their references name what its steps bind, not the outer script's.
		bool KeepsOwnReferences(std::string_view command)
		{
			return command == "editor.batch";
		}

		std::optional<std::string> CheckReferences(const nlohmann::json& value, const std::unordered_set<std::string>& bound, bool& found)
		{
			if (EditorCommandScript::IsReference(value))
			{
				found = true;
				const std::string& name = value["$ref"].get_ref<const std::string&>();
				if (!bound.contains(name))
					return fmt::format("unknown binding '{}': no earlier step binds it (\"bind\": {{\"{}\": \"/id\"}})", name, name);
				if (const auto pointer = value.find("pointer"); pointer != value.end() && !ParseJsonPointer(pointer->get<std::string>()))
					return fmt::format("the \"pointer\" of the reference to '{}' is not a JSON pointer", name);
				return std::nullopt;
			}
			if (value.is_object())
			{
				if (value.contains("$ref"))
					return std::string("a {\"$ref\"} object takes a binding's name and optionally a \"pointer\" (strings), nothing else");
				for (const auto& [key, member] : value.items())
				{
					if (std::optional<std::string> error = CheckReferences(member, bound, found))
						return error;
				}
			}
			else if (value.is_array())
			{
				for (const nlohmann::json& element : value)
				{
					if (std::optional<std::string> error = CheckReferences(element, bound, found))
						return error;
				}
			}
			return std::nullopt;
		}

	}

	bool PollCommandResult(EditorCommandResult& result, EditorContext& context)
	{
		if (!result.IsPending())
			return true;

		// The frame profiler's stage of command work, as in EditorCommandRegistry::Execute.
		ST_PROFILE_STAGE("Editor.Commands");
		const uint64_t journalId = result.JournalId;
		CommandJournal& journal = context.GetJournal();
		journal.NoteEditsOutsideCommands(context.GetUndoStack());
		const uint64_t records = context.GetUndoStack().GetRecordCount();
		// While its poll runs the command is the running one again: what the poll does is known to be its work.
		journal.Resume(journalId);
		std::optional<EditorCommandResult> finished;
		try
		{
			finished = result.Pending(context);
		}
		catch (const std::exception& exception)
		{
			// Engine code does not throw; this guards third-party code (JSON access) used by poll functions.
			finished = EditorCommandResult::Fail(fmt::format("Command failed: {}", exception.what()), EditorCommandError::Internal);
		}
		journal.Suspend(journalId);
		journal.NotePoll(journalId);
		journal.NoteUndo(journalId, context.GetUndoStack(), records);
		if (!finished)
			return false;
		// The poll function has returned, so replacing it does not destroy a running function.
		result = std::move(*finished);
		result.JournalId = journalId;
		if (result.IsPending())
			return false;
		journal.Complete(journalId, result);
		return true;
	}

	////////////////////////////////////////////////////////////////////////////////
	// EditorCommandRunner
	////////////////////////////////////////////////////////////////////////////////

	EditorCommandRunner::~EditorCommandRunner()
	{
		CancelAll("The editor is shutting down");
	}

	bool EditorCommandRunner::Run(EditorContext& context, const EditorCommandRegistry& commands, std::string_view name,
		const nlohmann::json& parameters, Completion onComplete, const CommandOrigin& origin)
	{
		EditorCommandResult result = commands.Execute(context, name, parameters, origin);
		const bool fromUI = origin.Source == CommandSource::UI;
		if (!result.IsPending())
		{
			if (fromUI)
				NotifyCommandOutcome(context.GetNotifications(), name, result);
			if (onComplete)
				onComplete(result);
			return false;
		}
		m_Pending.push_back({ std::string(name), std::move(result), std::move(onComplete), context.GetJournalRef(), fromUI, false });
		return true;
	}

	void EditorCommandRunner::Update(EditorContext& context)
	{
		ST_ASSERT(!m_Updating, "EditorCommandRunner::Update must not be called from a poll function or completion");
		m_Updating = true;
		// Poll functions and completions may issue commands (appended to m_Pending, which can reallocate it), so nothing
		// refers into the vector while they run, and only the commands issued before this update are polled.
		const size_t count = m_Pending.size();
		for (size_t index = 0; index < count; index++)
		{
			if (m_Pending[index].Done)
				continue; // Cancelled by an earlier completion

			EditorCommandResult result = std::move(m_Pending[index].Result);
			m_Pending[index].Result = {};
			const bool finished = PollCommandResult(result, context);
			if (m_Pending[index].Done)
			{
				// Cancelled while it was polled (its result was out of the list then): its journal entry ends cancelled too.
				if (!finished && m_Pending[index].Journal)
					m_Pending[index].Journal->Complete(result.JournalId, EditorCommandResult::Fail("Cancelled while it ran", EditorCommandError::Cancelled));
				continue;
			}
			if (finished)
			{
				if (m_Pending[index].FromUI)
					NotifyCommandOutcome(context.GetNotifications(), m_Pending[index].Name, result);
				Finish(index, result);
			}
			else
			{
				m_Pending[index].Result = std::move(result);
			}
		}
		std::erase_if(m_Pending, [](const PendingCommand& command) { return command.Done; });
		m_Updating = false;
	}

	void EditorCommandRunner::CancelAll(const std::string& reason)
	{
		// Also cancels commands issued by the completions of cancelled commands.
		for (size_t index = 0; index < m_Pending.size(); index++)
		{
			if (m_Pending[index].Done)
				continue;
			const EditorCommandResult cancelled = EditorCommandResult::Fail(reason, EditorCommandError::Cancelled);
			if (m_Pending[index].Journal)
				m_Pending[index].Journal->Complete(m_Pending[index].Result.JournalId, cancelled);
			Finish(index, cancelled);
		}
		// During an update, the entries are erased when it ends.
		if (!m_Updating)
			m_Pending.clear();
	}

	size_t EditorCommandRunner::GetPendingCount() const
	{
		return static_cast<size_t>(std::count_if(m_Pending.begin(), m_Pending.end(), [](const PendingCommand& command) { return !command.Done; }));
	}

	void EditorCommandRunner::Finish(size_t index, const EditorCommandResult& result)
	{
		m_Pending[index].Done = true;
		// The completion may issue commands, which can reallocate m_Pending: take it out first.
		Completion onComplete = std::move(m_Pending[index].OnComplete);
		if (onComplete)
			onComplete(result);
	}

	////////////////////////////////////////////////////////////////////////////////
	// EditorCommandScript
	////////////////////////////////////////////////////////////////////////////////

	Scope<EditorCommandScript> EditorCommandScript::FromJson(const nlohmann::json& json, std::string* outError)
	{
		auto fail = [outError](std::string error) -> Scope<EditorCommandScript>
		{
			if (outError)
				*outError = std::move(error);
			return nullptr;
		};

		if (!json.is_array())
			return fail("A command script must be a JSON array of {\"command\": name, \"parameters\": {...}}");

		std::vector<Step> steps;
		steps.reserve(json.size());
		std::unordered_set<std::string> bound; // Names the steps so far bind
		for (size_t index = 0; index < json.size(); index++)
		{
			const nlohmann::json& step = json[index];
			if (!step.is_object())
				return fail(fmt::format("Step {} must be an object {{\"command\": name, \"parameters\": {{...}}}}", index + 1));
			for (const auto& [key, value] : step.items())
			{
				if (key != "command" && key != "parameters" && key != "expect" && key != "bind" && key != "until")
				{
					return fail(fmt::format("Step {}: unknown key '{}' (expected \"command\", \"parameters\", \"expect\", \"bind\" and \"until\")", index + 1,
						key));
				}
			}
			const nlohmann::json* command = JsonUtils::Find(step, "command");
			if (!command || !command->is_string() || command->get<std::string>().empty())
				return fail(fmt::format("Step {} needs a \"command\" name", index + 1));
			const nlohmann::json* parameters = JsonUtils::Find(step, "parameters");
			if (parameters && !parameters->is_object())
				return fail(fmt::format("Step {} ({}): \"parameters\" must be an object", index + 1, command->get<std::string>()));
			std::vector<Expectation> expectations;
			if (const nlohmann::json* expect = JsonUtils::Find(step, "expect"))
			{
				if (const std::optional<std::string> error = ParseExpectations(*expect, expectations))
					return fail(fmt::format("Step {} ({}): {}", index + 1, command->get<std::string>(), *error));
			}
			// References are checked before anything runs: each must name a binding of an earlier step. A batch's steps keep
			// theirs, which its own steps bind (KeepsOwnReferences).
			bool usesBindings = false;
			if (parameters)
			{
				nlohmann::json checked = *parameters;
				if (KeepsOwnReferences(command->get<std::string>()))
					checked.erase("steps");
				if (const std::optional<std::string> error = CheckReferences(checked, bound, usesBindings))
					return fail(fmt::format("Step {} ({}): {}", index + 1, command->get<std::string>(), *error));
			}
			std::vector<Binding> bindings;
			if (const nlohmann::json* bind = JsonUtils::Find(step, "bind"))
			{
				if (const std::optional<std::string> error = ParseBindings(*bind, bindings))
					return fail(fmt::format("Step {} ({}): {}", index + 1, command->get<std::string>(), *error));
			}
			std::optional<UntilBounds> until;
			if (const nlohmann::json* untilJson = JsonUtils::Find(step, "until"))
			{
				UntilBounds bounds;
				if (const std::optional<std::string> error = ParseUntil(*untilJson, bounds))
					return fail(fmt::format("Step {} ({}): {}", index + 1, command->get<std::string>(), *error));
				if (expectations.empty())
				{
					return fail(fmt::format("Step {} ({}): \"until\" repeats the step until its \"expect\" holds: give an \"expect\"", index + 1,
						command->get<std::string>()));
				}
				until = bounds;
			}
			for (const Binding& binding : bindings)
				bound.insert(binding.Name);
			steps.push_back({ command->get<std::string>(), parameters ? *parameters : nlohmann::json::object(), std::move(expectations), std::move(bindings),
				usesBindings, until });
		}
		return CreateScope<EditorCommandScript>(std::move(steps));
	}

	bool EditorCommandScript::IsReference(const nlohmann::json& value)
	{
		if (!value.is_object() || value.size() > 2)
			return false;
		const auto name = value.find("$ref");
		if (name == value.end() || !name->is_string())
			return false;
		const auto pointer = value.find("pointer");
		return value.size() == 1 || (pointer != value.end() && pointer->is_string());
	}

	std::optional<nlohmann::json> EditorCommandScript::SubstituteStep(const Step& step, std::string& outError) const
	{
		if (!KeepsOwnReferences(step.Command) || !step.Parameters.contains("steps"))
			return Substitute(step.Parameters, outError);
		nlohmann::json parameters = step.Parameters;
		nlohmann::json steps = std::move(parameters["steps"]);
		parameters.erase("steps");
		std::optional<nlohmann::json> substituted = Substitute(parameters, outError);
		if (substituted)
			(*substituted)["steps"] = std::move(steps);
		return substituted;
	}

	std::optional<nlohmann::json> EditorCommandScript::Substitute(const nlohmann::json& value, std::string& outError) const
	{
		if (IsReference(value))
		{
			const std::string& name = value["$ref"].get_ref<const std::string&>();
			const auto bound = m_Bindings.find(name);
			if (bound == m_Bindings.end())
			{
				outError = fmt::format("the binding '{}' has no value (the step that binds it failed, or its result held nothing there)", name);
				return std::nullopt;
			}
			const auto pointer = value.find("pointer");
			if (pointer == value.end())
				return bound->second;
			const std::optional<std::vector<std::string>> path = ParseJsonPointer(pointer->get<std::string>());
			const nlohmann::json* resolved = path ? ResolveJsonPath(bound->second, *path) : nullptr;
			if (!resolved)
			{
				outError = fmt::format("the binding '{}' has no value at '{}'", name, pointer->get<std::string>());
				return std::nullopt;
			}
			return *resolved;
		}
		if (value.is_object())
		{
			nlohmann::json substituted = nlohmann::json::object();
			for (const auto& [key, member] : value.items())
			{
				std::optional<nlohmann::json> replaced = Substitute(member, outError);
				if (!replaced)
					return std::nullopt;
				substituted[key] = std::move(*replaced);
			}
			return substituted;
		}
		if (value.is_array())
		{
			nlohmann::json substituted = nlohmann::json::array();
			for (const nlohmann::json& element : value)
			{
				std::optional<nlohmann::json> replaced = Substitute(element, outError);
				if (!replaced)
					return std::nullopt;
				substituted.push_back(std::move(*replaced));
			}
			return substituted;
		}
		return value;
	}

	Scope<EditorCommandScript> EditorCommandScript::Load(const std::filesystem::path& path, std::string* outError)
	{
		std::optional<std::string> text = FileSystem::ReadText(path);
		if (!text)
		{
			if (outError)
				*outError = fmt::format("Cannot read '{}'", FileSystem::ToUTF8(path));
			return nullptr;
		}
		std::string error;
		std::optional<nlohmann::json> json = JsonUtils::Parse(*text, &error);
		if (!json)
		{
			if (outError)
				*outError = fmt::format("'{}' is not valid JSON: {}", FileSystem::ToUTF8(path), error);
			return nullptr;
		}
		Scope<EditorCommandScript> script = FromJson(*json, &error);
		if (!script && outError)
			*outError = fmt::format("'{}': {}", FileSystem::ToUTF8(path), error);
		return script;
	}

	EditorCommandScript::EditorCommandScript(std::vector<Step> steps)
		: m_Steps(std::move(steps))
	{
	}

	EditorCommandResult EditorCommandScript::RunStepNow(size_t index, EditorContext& context, const EditorCommandRegistry& commands)
	{
		const Step& step = m_Steps[index];
		nlohmann::json substituted;
		if (step.UsesBindings)
		{
			std::string error;
			std::optional<nlohmann::json> parameters = SubstituteStep(step, error);
			if (!parameters)
			{
				for (const Binding& binding : step.Bindings)
					m_Bindings.erase(binding.Name);
				return EditorCommandResult::InvalidParameters(error);
			}
			substituted = std::move(*parameters);
		}
		EditorCommandResult result = commands.Execute(context, step.Command, step.UsesBindings ? substituted : step.Parameters);
		if (result.IsPending())
			result = EditorCommandResult::Fail(fmt::format("{} answers later, which a batch cannot wait for", step.Command));
		if (!result.Success)
		{
			// Later steps that use its bindings fail instead of running with values of an earlier step.
			for (const Binding& binding : step.Bindings)
				m_Bindings.erase(binding.Name);
			return result;
		}
		if (const std::string unmet = CheckExpectations(result.Value, step.Expectations); !unmet.empty())
		{
			for (const Binding& binding : step.Bindings)
				m_Bindings.erase(binding.Name);
			return EditorCommandResult::Fail(unmet);
		}
		for (const Binding& binding : step.Bindings)
		{
			const nlohmann::json* value = ResolveJsonPath(result.Value, binding.Path);
			if (!value)
			{
				m_Bindings.erase(binding.Name);
				return EditorCommandResult::Fail(fmt::format("The result has no value at '{}' to bind as '{}'", binding.Pointer, binding.Name));
			}
			m_Bindings.insert_or_assign(binding.Name, *value);
		}
		return result;
	}

	std::optional<std::string> EditorCommandScript::CheckUntilSteps(const EditorCommandRegistry& commands) const
	{
		for (size_t index = 0; index < m_Steps.size(); index++)
		{
			const Step& step = m_Steps[index];
			if (!step.Until)
				continue;
			const EditorCommand* command = commands.Find(step.Command);
			if (!command)
				return fmt::format("Step {} ({}): unknown command (editor.commands lists them)", index + 1, step.Command);
			if (command->Kind != EditorCommandKind::Query)
			{
				return fmt::format("Step {} ({}): \"until\" repeats a query, and {} is not one (its kind is {})", index + 1, step.Command, step.Command,
					EditorCommandKindToString(command->Kind));
			}
		}
		return std::nullopt;
	}

	void EditorCommandScript::TakeBindings(const Step& step, const EditorCommandResult* result, bool succeeded)
	{
		for (const Binding& binding : step.Bindings)
		{
			if (succeeded && result)
			{
				if (const nlohmann::json* value = ResolveJsonPath(result->Value, binding.Path))
				{
					m_Bindings.insert_or_assign(binding.Name, *value);
					continue;
				}
				ST_ERROR("{}: the result has no value at '{}' to bind as '{}'", step.Command, binding.Pointer, binding.Name);
				m_Failed = true;
			}
			// Later steps that use its bindings fail instead of running with values of an earlier step.
			m_Bindings.erase(binding.Name);
		}
	}

	bool EditorCommandScript::JudgeUntilAttempt(EditorContext& context)
	{
		UntilWait& wait = *m_Until;
		const Step& step = m_Steps[wait.Index];
		EditorCommandResult result = std::move(*wait.Result);
		wait.Result.reset();

		const uint64_t steps = context.GetFixedStepCount();
		const uint64_t stepsRun = steps >= wait.StartSteps ? steps - wait.StartSteps : steps; // A new play session counts from 0
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wait.Started).count();
		std::string unmet;
		if (result.Success)
		{
			unmet = CheckExpectations(result.Value, step.Expectations);
			if (!unmet.empty() && (!step.Until->Steps || stepsRun < *step.Until->Steps) && seconds < step.Until->Seconds)
			{
				// Not yet: the next attempt comes in a later frame, after one more fixed step while the game is paused.
				if (context.IsPlaying() && context.IsPaused() && !context.IsInputSequenceRunning())
				{
					if (!context.HasPendingSteps())
						context.Step(1);
					wait.StepTarget = context.GetFixedStepCount() + context.GetPendingSteps();
				}
				return false;
			}
		}

		// The final attempt is the step's one entry in the activity journal, with how many attempts it took.
		CommandJournal& journal = context.GetJournal();
		const uint64_t entry = journal.Begin(CommandOrigin { CommandSource::CommandScript }, step.Command, wait.Kind, wait.Results, wait.Parameters,
			context.IsPlaying());
		EditorCommandResult recorded = result;
		if (recorded.Value.is_object())
			recorded.Value["attempts"] = wait.Attempts;
		journal.Complete(entry, recorded);

		if (!result.Success)
		{
			ST_ERROR("{} failed: {}", step.Command, result.Error);
			m_Failed = true;
			TakeBindings(step, nullptr, false);
		}
		else if (unmet.empty())
		{
			ST_INFO("{} -> {} (after {} {})", step.Command, DescribeResultValue(result.Value), wait.Attempts, wait.Attempts == 1 ? "attempt" : "attempts");
			TakeBindings(step, &result, true);
		}
		else
		{
			ST_ERROR("{}: {} (until: {} attempts in {} fixed steps and {:.1f} s; the last result: {})", step.Command, unmet, wait.Attempts, stepsRun, seconds,
				DescribeResultValue(result.Value));
			m_Failed = true;
			TakeBindings(step, nullptr, false);
		}
		m_Until.reset();
		return true;
	}

	bool EditorCommandScript::ContinueUntil(EditorCommandRunner& runner, EditorContext& context, const EditorCommandRegistry& commands)
	{
		// The result of an attempt that answered over frames.
		if (m_Until->Result && JudgeUntilAttempt(context))
			return true;

		UntilWait& wait = *m_Until;
		if (wait.LastAttemptUpdate == m_UpdateCount)
			return false; // One attempt per frame
		if (wait.StepTarget)
		{
			// Paused: the next attempt sees the fixed step asked for, unless the game was resumed or stopped (which drops it).
			if (context.IsPlaying() && context.IsPaused() && context.HasPendingSteps() && context.GetFixedStepCount() < *wait.StepTarget
				&& std::chrono::duration<double>(std::chrono::steady_clock::now() - wait.Started).count() < m_Steps[wait.Index].Until->Seconds)
			{
				return false;
			}
			wait.StepTarget.reset();
		}

		wait.LastAttemptUpdate = m_UpdateCount;
		wait.Attempts++;
		m_Waiting = true;
		{
			const JournalPause pause(context.GetJournal());
			runner.Run(context, commands, m_Steps[wait.Index].Command, wait.Parameters, [this](const EditorCommandResult& result)
			{
				m_Until->Result = result;
				m_Waiting = false;
			}, CommandOrigin { CommandSource::CommandScript });
		}
		// An attempt that answered at once is judged now; one that answers later, in the frame its result arrives.
		return !m_Waiting && JudgeUntilAttempt(context);
	}

	bool EditorCommandScript::Update(EditorCommandRunner& runner, EditorContext& context, const EditorCommandRegistry& commands)
	{
		m_UpdateCount++;
		if (!m_CheckedUntil)
		{
			// A script whose waits cannot work is refused before its first step runs.
			m_CheckedUntil = true;
			if (const std::optional<std::string> error = CheckUntilSteps(commands))
			{
				ST_ERROR("The command script cannot run: {}", *error);
				m_Failed = true;
				m_Next = m_Steps.size();
				return IsFinished();
			}
		}

		while (!m_Waiting)
		{
			if (m_Until)
			{
				if (!ContinueUntil(runner, context, commands))
					break;
				continue;
			}
			if (m_Next >= m_Steps.size())
				break;
			const size_t index = m_Next++;
			const Step& step = m_Steps[index];
			nlohmann::json parameters;
			if (step.UsesBindings)
			{
				// A step whose references have no value cannot be run as written: it fails as invalid parameters.
				std::string error;
				std::optional<nlohmann::json> substituted = SubstituteStep(step, error);
				if (!substituted)
				{
					ST_ERROR("{} failed (invalid parameters): {}", step.Command, error);
					m_Failed = true;
					continue;
				}
				parameters = std::move(*substituted);
			}
			if (step.Until)
			{
				const EditorCommand* command = commands.Find(step.Command);
				UntilWait wait;
				wait.Index = index;
				wait.Parameters = step.UsesBindings ? std::move(parameters) : step.Parameters;
				wait.Kind = command ? command->Kind : EditorCommandKind::Query;
				wait.Results = command ? command->Results : EditorCommandResults::Content;
				wait.StartSteps = context.GetFixedStepCount();
				wait.Started = std::chrono::steady_clock::now();
				m_Until = std::move(wait);
				continue;
			}
			m_Waiting = true;
			runner.Run(context, commands, step.Command, step.UsesBindings ? parameters : step.Parameters, [this, index](const EditorCommandResult& result)
			{
				const Step& finished = m_Steps[index];
				if (result.Success)
				{
					ST_INFO("{} -> {}", finished.Command, DescribeResultValue(result.Value));
					const std::string unmet = CheckExpectations(result.Value, finished.Expectations);
					if (!unmet.empty())
					{
						ST_ERROR("{}: {}", finished.Command, unmet);
						m_Failed = true;
					}
					TakeBindings(finished, &result, true);
				}
				else
				{
					ST_ERROR("{} failed: {}", finished.Command, result.Error);
					m_Failed = true;
					TakeBindings(finished, nullptr, false);
				}
				m_Waiting = false;
			}, CommandOrigin { CommandSource::CommandScript });
		}
		return IsFinished();
	}

}
