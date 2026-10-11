#pragma once

#include "Editor/EditorCommands.h"

#include <Strata/Core/Base.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Strata
{

	class EditorContext;

	// Polls a result once if it is pending; exceptions from third-party code inside the poll become an error result.
	// Returns true when the result is final, and then completes its entry in the activity journal (CommandJournal).
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

		// Runs a command for `origin` (recorded in the activity journal). If it finishes immediately, `onComplete` is called
		// before Run returns; otherwise it is called from a later Update. Returns true when the command is pending.
		bool Run(EditorContext& context, const EditorCommandRegistry& commands, std::string_view name, const nlohmann::json& parameters,
			Completion onComplete, const CommandOrigin& origin = {});
		// Once per frame: polls the pending commands in the order they were issued. Commands issued by completions are
		// polled from the next frame on.
		void Update(EditorContext& context);
		// Completes every pending command with a Cancelled error carrying `reason` (e.g. the editor is closing), also in the
		// activity journal.
		void CancelAll(const std::string& reason);

		size_t GetPendingCount() const;
	private:
		struct PendingCommand
		{
			std::string Name;
			EditorCommandResult Result;
			Completion OnComplete;
			// The journal of the command's entry: cancelling completes it there, also after the context is gone.
			Ref<CommandJournal> Journal;
			bool Done = false; // Completed or cancelled; erased at the end of the update
		};

		// Marks the command done and calls its completion.
		void Finish(size_t index, const EditorCommandResult& result);
	private:
		std::vector<PendingCommand> m_Pending; // In the order they were issued
		bool m_Updating = false;
	};

	// A list of commands run one after another, e.g. `StrataEditor --commands script.json`: a JSON array of
	// {"command": name, "parameters": {...}, "expect": {...}, "bind": {...}} (all but "command" optional). A pending command
	// holds the script until it completes. A failed command is logged and marks the script as failed; the following
	// commands still run. Its commands are recorded in the activity journal as CommandSource::CommandScript.
	//
	// "expect" checks the result of a successful command: it maps JSON pointers into the result ("" is the whole result,
	// "/values/Translation/1" an element) to conditions {"equals": value, "min": number, "max": number} (any of them); a
	// result that does not meet them fails the step like a failed command.
	//
	// "bind" names values of a step's result for later steps, {"crate": "/id"} (JSON pointers into the result), and later
	// parameters use them anywhere in nested objects and arrays as {"$ref": "crate"} or {"$ref": "crate", "pointer":
	// "/entities/0"} (a pointer into the bound value), which the value replaces. So a script can change entities it created,
	// whose IDs are new every run. A reference to a name no earlier step binds fails the script before anything runs
	// (FromJson); a step whose binding has no value (the step that binds it failed, or its result had nothing there) fails
	// without running, as invalid parameters.
	class EditorCommandScript
	{
	public:
		struct Expectation
		{
			std::string Pointer;           // As written in the script, for messages
			std::vector<std::string> Path; // The pointer's reference tokens
			std::optional<nlohmann::json> Equals;
			std::optional<double> Min;
			std::optional<double> Max;
		};

		struct Binding
		{
			std::string Name;
			std::string Pointer;           // As written in the script, for messages
			std::vector<std::string> Path; // The pointer's reference tokens
		};

		struct Step
		{
			std::string Command;
			nlohmann::json Parameters;
			std::vector<Expectation> Expectations;
			std::vector<Binding> Bindings;
			bool UsesBindings = false; // Its parameters hold {"$ref"} objects to replace before it runs
		};

		// A {"$ref": name, "pointer"?: pointer} object of a script's parameters.
		static bool IsReference(const nlohmann::json& value);

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
		// The values bound so far (name -> value).
		const std::unordered_map<std::string, nlohmann::json>& GetBindings() const { return m_Bindings; }
	private:
		// The parameters with every reference replaced by its value; an error when a binding has none.
		std::optional<nlohmann::json> Substitute(const nlohmann::json& value, std::string& outError) const;
	private:
		std::vector<Step> m_Steps;
		std::unordered_map<std::string, nlohmann::json> m_Bindings;
		size_t m_Next = 0;
		bool m_Waiting = false;
		bool m_Failed = false;
	};

}
