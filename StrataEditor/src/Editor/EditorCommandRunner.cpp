#include "Editor/EditorCommandRunner.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>

#include <algorithm>
#include <exception>
#include <utility>

namespace Strata
{

	namespace
	{

		// Results can be large (entity listings, images): the log keeps the start.
		constexpr size_t c_MaxLoggedResultSize = 2000;

		std::string DescribeResultValue(const nlohmann::json& value)
		{
			if (value.is_null())
				return "ok";
			std::string text = JsonUtils::Dump(value);
			if (text.size() > c_MaxLoggedResultSize)
				text = fmt::format("{}... ({} bytes)", text.substr(0, c_MaxLoggedResultSize), text.size());
			return text;
		}

	}

	bool PollCommandResult(EditorCommandResult& result, EditorContext& context)
	{
		if (!result.IsPending())
			return true;

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
		if (!finished)
			return false;
		// The poll function has returned, so replacing it does not destroy a running function.
		result = std::move(*finished);
		return !result.IsPending();
	}

	////////////////////////////////////////////////////////////////////////////////
	// EditorCommandRunner
	////////////////////////////////////////////////////////////////////////////////

	EditorCommandRunner::~EditorCommandRunner()
	{
		CancelAll("The editor is shutting down");
	}

	bool EditorCommandRunner::Run(EditorContext& context, const EditorCommandRegistry& commands, std::string_view name,
		const nlohmann::json& parameters, Completion onComplete)
	{
		EditorCommandResult result = commands.Execute(context, name, parameters);
		if (!result.IsPending())
		{
			if (onComplete)
				onComplete(result);
			return false;
		}
		m_Pending.push_back({ std::string(name), std::move(result), std::move(onComplete), false });
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
				continue; // Cancelled while it was polled
			if (finished)
				Finish(index, result);
			else
				m_Pending[index].Result = std::move(result);
		}
		std::erase_if(m_Pending, [](const PendingCommand& command) { return command.Done; });
		m_Updating = false;
	}

	void EditorCommandRunner::CancelAll(const std::string& reason)
	{
		// Also cancels commands issued by the completions of cancelled commands.
		for (size_t index = 0; index < m_Pending.size(); index++)
		{
			if (!m_Pending[index].Done)
				Finish(index, EditorCommandResult::Fail(reason, EditorCommandError::Cancelled));
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
		for (size_t index = 0; index < json.size(); index++)
		{
			const nlohmann::json& step = json[index];
			if (!step.is_object())
				return fail(fmt::format("Step {} must be an object {{\"command\": name, \"parameters\": {{...}}}}", index + 1));
			for (const auto& [key, value] : step.items())
			{
				if (key != "command" && key != "parameters")
					return fail(fmt::format("Step {}: unknown key '{}' (expected \"command\" and \"parameters\")", index + 1, key));
			}
			const nlohmann::json* command = JsonUtils::Find(step, "command");
			if (!command || !command->is_string() || command->get<std::string>().empty())
				return fail(fmt::format("Step {} needs a \"command\" name", index + 1));
			const nlohmann::json* parameters = JsonUtils::Find(step, "parameters");
			if (parameters && !parameters->is_object())
				return fail(fmt::format("Step {} ({}): \"parameters\" must be an object", index + 1, command->get<std::string>()));
			steps.push_back({ command->get<std::string>(), parameters ? *parameters : nlohmann::json::object() });
		}
		return CreateScope<EditorCommandScript>(std::move(steps));
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

	bool EditorCommandScript::Update(EditorCommandRunner& runner, EditorContext& context, const EditorCommandRegistry& commands)
	{
		while (!m_Waiting && m_Next < m_Steps.size())
		{
			const size_t index = m_Next++;
			m_Waiting = true;
			runner.Run(context, commands, m_Steps[index].Command, m_Steps[index].Parameters, [this, index](const EditorCommandResult& result)
			{
				const std::string& name = m_Steps[index].Command;
				if (result.Success)
				{
					ST_INFO("{} -> {}", name, DescribeResultValue(result.Value));
				}
				else
				{
					ST_ERROR("{} failed: {}", name, result.Error);
					m_Failed = true;
				}
				m_Waiting = false;
			});
		}
		return IsFinished();
	}

}
