#pragma once

#include "Editor/EditorCommands.h"

#include <Strata/Core/Base.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	class EditorContext;

	// Polls a result once if it is pending; exceptions from third-party code inside the poll become an error result.
	// Returns true when the result is final.
	bool PollCommandResult(EditorCommandResult& result, EditorContext& context);

	// Runs editor commands for the UI, command scripts and automation. A command that finishes over several frames
	// (EditorCommandResult::Defer) is polled once per frame by Update, starting with the frame after it was issued, and
	// reports through its completion callback. Main thread only.
	class EditorCommandRunner
	{
	public:
		using Completion = std::function<void(const EditorCommandResult& result)>;

		EditorCommandRunner() = default;
		// Pending commands are cancelled (see CancelAll).
		~EditorCommandRunner();

		EditorCommandRunner(const EditorCommandRunner&) = delete;
		EditorCommandRunner& operator=(const EditorCommandRunner&) = delete;

		// Runs a command. If it finishes immediately, `onComplete` is called before Run returns; otherwise it is called
		// from a later Update. Returns true when the command is pending.
		bool Run(EditorContext& context, const EditorCommandRegistry& commands, std::string_view name, const nlohmann::json& parameters,
			Completion onComplete);
		// Once per frame: polls the pending commands in the order they were issued. Commands issued by completions are
		// polled from the next frame on.
		void Update(EditorContext& context);
		// Completes every pending command with a Cancelled error carrying `reason` (e.g. the editor is closing).
		void CancelAll(const std::string& reason);

		size_t GetPendingCount() const;
	private:
		struct PendingCommand
		{
			std::string Name;
			EditorCommandResult Result;
			Completion OnComplete;
			bool Done = false; // Completed or cancelled; erased at the end of the update
		};

		// Marks the command done and calls its completion.
		void Finish(size_t index, const EditorCommandResult& result);
	private:
		std::vector<PendingCommand> m_Pending; // In the order they were issued
		bool m_Updating = false;
	};

	// A list of commands run one after another, e.g. `StrataEditor --commands script.json`: a JSON array of
	// {"command": name, "parameters": {...}} ("parameters" is optional). A pending command holds the script until it
	// completes. A failed command is logged and marks the script as failed; the following commands still run.
	class EditorCommandScript
	{
	public:
		struct Step
		{
			std::string Command;
			nlohmann::json Parameters;
		};

		static Scope<EditorCommandScript> FromJson(const nlohmann::json& json, std::string* outError = nullptr);
		static Scope<EditorCommandScript> Load(const std::filesystem::path& path, std::string* outError = nullptr);

		explicit EditorCommandScript(std::vector<Step> steps);
		// Running commands refer to the script: it must stay where it is until it finished or the runner was cancelled.
		EditorCommandScript(const EditorCommandScript&) = delete;
		EditorCommandScript& operator=(const EditorCommandScript&) = delete;

		// Runs the next steps through `runner` until one is pending or the script ends. Call once per frame, after
		// runner.Update. Returns true when the script has finished.
		bool Update(EditorCommandRunner& runner, EditorContext& context, const EditorCommandRegistry& commands);
		bool IsFinished() const { return m_Next == m_Steps.size() && !m_Waiting; }
		bool HasFailed() const { return m_Failed; }
		size_t GetStepCount() const { return m_Steps.size(); }
		// Number of steps that completed.
		size_t GetCompletedCount() const { return m_Waiting ? m_Next - 1 : m_Next; }
	private:
		std::vector<Step> m_Steps;
		size_t m_Next = 0;
		bool m_Waiting = false;
		bool m_Failed = false;
	};

}
