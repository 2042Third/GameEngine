#pragma once

#include "Editor/EditorCommands.h"

#include <Strata/Core/Base.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
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
			bool FromUI = false; // Run for the person: failures and warnings show as notifications
			bool Done = false; // Completed or cancelled; erased at the end of the update
		};

		// Marks the command done and calls its completion.
		void Finish(size_t index, const EditorCommandResult& result);
	private:
		std::vector<PendingCommand> m_Pending; // In the order they were issued
		bool m_Updating = false;
	};

	// A list of commands run one after another, e.g. `StrataEditor --commands script.json`: a JSON array of
	// {"command": name, "parameters": {...}, "expect": {...}, "bind": {...}, "until": {...}} (all but "command" optional). A pending command
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
	// without running, as invalid parameters. The steps of an editor.batch step are a script of their own: their references
	// name what the batch's steps bind.
	//
	// "until" waits for a condition: {"steps": n, "seconds": s} (at least one; seconds default to c_DefaultUntilSeconds) repeats
	// the step, which must be a query (EditorCommandKind::Query) with an "expect", once per frame until its result meets the
	// expectations. It fails, logging the last result and the attempts made, when `steps` of the game's fixed steps
	// (EditorContext::GetFixedStepCount) ran or `seconds` passed since the step began. In a paused game each attempt that
	// misses runs one fixed step before the next (as play.step does), so a paused wait is exact whatever the frame rate. Only
	// the final attempt is recorded in the activity journal (its result with "attempts"), and "bind" reads its result. Steps
	// with "until" are checked against the registry before the script's first step runs (Update), and editor.batch refuses
	// them (a batch finishes within one frame).
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

		// A step's "until": the bounds of its wait.
		struct UntilBounds
		{
			std::optional<uint64_t> Steps; // Fixed steps of the game; none: only the seconds bound it
			double Seconds = c_DefaultUntilSeconds;
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
			std::optional<UntilBounds> Until;
		};

		static constexpr double c_DefaultUntilSeconds = 300.0;
		static constexpr uint64_t c_MaxUntilSteps = 10'000'000;
		static constexpr double c_MaxUntilSeconds = 24.0 * 60.0 * 60.0;

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
		bool IsFinished() const { return m_Next == m_Steps.size() && !m_Waiting && !m_Until; }
		bool HasFailed() const { return m_Failed; }
		size_t GetStepCount() const { return m_Steps.size(); }
		// Number of steps that completed.
		size_t GetCompletedCount() const { return m_Waiting || m_Until ? m_Next - 1 : m_Next; }
		// What is wrong with the script's "until" steps for `commands` (an unknown command, one that is not a query), or nullopt.
		std::optional<std::string> CheckUntilSteps(const EditorCommandRegistry& commands) const;
		// The values bound so far (name -> value).
		const std::unordered_map<std::string, nlohmann::json>& GetBindings() const { return m_Bindings; }
		const Step& GetStep(size_t index) const { return m_Steps[index]; }

		// Runs one step now through `commands` (EditorCommandRegistry::Execute, without a runner: editor.batch's steps, which
		// never defer; inside a running command they are its children in the journal) and takes its bindings. A step whose
		// references have no value fails as invalid parameters without running; a result that misses an expectation or a
		// value to bind, or that would answer later, fails the step.
		EditorCommandResult RunStepNow(size_t index, EditorContext& context, const EditorCommandRegistry& commands);
	private:
		// The parameters with every reference replaced by its value; an error when a binding has none.
		std::optional<nlohmann::json> Substitute(const nlohmann::json& value, std::string& outError) const;
		// A step's parameters substituted; those of an editor.batch step keep its steps' own references.
		std::optional<nlohmann::json> SubstituteStep(const Step& step, std::string& outError) const;
		// Takes a successful result's bindings (an error is logged for each value it lacks) or, for a failed step, drops them.
		void TakeBindings(const Step& step, const EditorCommandResult* result, bool succeeded);
		// Runs the "until" step being waited for: an attempt per frame, a fixed step between attempts while paused. Returns true
		// when the wait ended (the script goes on to its next step), false while it continues in a later frame.
		bool ContinueUntil(EditorCommandRunner& runner, EditorContext& context, const EditorCommandRegistry& commands);
		// Judges the last attempt's result; true when the wait ended (met, failed or out of bounds).
		bool JudgeUntilAttempt(EditorContext& context);
	private:
		// The "until" step being waited for.
		struct UntilWait
		{
			size_t Index = 0;
			nlohmann::json Parameters; // Substituted once, when the step began
			EditorCommandKind Kind = EditorCommandKind::Query;
			EditorCommandResults Results = EditorCommandResults::Content;
			uint64_t StartSteps = 0;
			std::chrono::steady_clock::time_point Started;
			uint32_t Attempts = 0;
			uint64_t LastAttemptUpdate = 0; // The Update call that issued the last attempt (one attempt per frame)
			std::optional<uint64_t> StepTarget; // Paused: the fixed step count the next attempt waits for
			std::optional<EditorCommandResult> Result; // The last attempt's result, not judged yet
		};
	private:
		std::vector<Step> m_Steps;
		std::unordered_map<std::string, nlohmann::json> m_Bindings;
		std::optional<UntilWait> m_Until;
		size_t m_Next = 0;
		uint64_t m_UpdateCount = 0;
		bool m_Waiting = false;
		bool m_Failed = false;
		bool m_CheckedUntil = false; // CheckUntilSteps ran before the first step
	};

}
