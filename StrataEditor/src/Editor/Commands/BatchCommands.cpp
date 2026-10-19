#include "Editor/CommandUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/UndoStack.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>

// editor.batch: several commands in one call and one undo step.

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// More steps than this are refused: a batch runs within one frame.
		constexpr size_t c_MaxBatchSteps = 1000;

		nlohmann::json DescribeStepError(const EditorCommandResult& result)
		{
			return { { "kind", EditorCommandErrorToString(result.ErrorKind) }, { "message", result.Error } };
		}

	}

	void RegisterBatchCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "editor.batch",
			"Runs several commands in this frame, one after another, as one undo step: steps [{command, parameters?, bind?, expect?}] as a command "
			"script writes them (bind names values of a step's result, {\"crate\": \"/id\"}, and later parameters use them as {\"$ref\": \"crate\"} or "
			"{\"$ref\": \"crate\", \"pointer\": \"/entities/0\"}; expect checks a result like a failure). At most 1,000 steps; commands that can take "
			"frames (loads, builds, imports, captures, saves, play.step, input with wait: MayDefer) and editor.batch itself are refused before anything runs. "
			"atomic (default true): a failed step undoes the steps before it, records no undo step and fails the batch naming the step's index; "
			"atomic false runs every step and keeps what succeeded. Returns {steps: [{command, result} or {command, error: {kind, message}}], "
			"bindings}.",
			ObjectSchema({ { "steps", { { "type", "array" }, { "items", { { "type", "object" } } }, { "minItems", 1 }, { "maxItems", c_MaxBatchSteps },
					{ "description", "The commands: [{command, parameters?, bind?, expect?}]" } } },
				{ "atomic", BoolSchema("Undo every step when one fails (default true)") } }, { "steps" }),
			[&registry](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool atomic = arguments.GetBool("atomic", true);
				if (!arguments.IsValid())
					return arguments.Fail();
				const nlohmann::json& steps = parameters["steps"];
				if (!steps.is_array() || steps.empty())
					return EditorCommandResult::InvalidParameters("Parameter 'steps': give at least one step [{command, parameters?}]");
				if (steps.size() > c_MaxBatchSteps)
					return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'steps': at most {} steps, not {}", c_MaxBatchSteps, steps.size()));
				std::string error;
				const Scope<EditorCommandScript> script = EditorCommandScript::FromJson(steps, &error);
				if (!script)
					return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'steps': {}", error));
				// Every step must finish within this frame: what cannot is refused before anything runs.
				for (size_t index = 0; index < script->GetStepCount(); index++)
				{
					const std::string& name = script->GetStep(index).Command;
					const EditorCommand* command = registry.Find(name);
					if (!command)
						return EditorCommandResult::InvalidParameters(fmt::format("Step {}: unknown command '{}' (editor.commands lists them)", index, name));
					if (command->Name == "editor.batch")
						return EditorCommandResult::InvalidParameters(fmt::format("Step {}: a batch cannot run another batch", index));
					if (command->MayDefer)
					{
						return EditorCommandResult::InvalidParameters(fmt::format("Step {}: {} can take frames (it loads, builds, waits or captures), which a "
							"batch cannot wait for: run it on its own", index, name));
					}
				}

				UndoStack& undo = context.GetUndoStack();
				if (!undo.BeginGroup(fmt::format("Batch of {} {}", script->GetStepCount(), script->GetStepCount() == 1 ? "command" : "commands")))
					return EditorCommandResult::Fail("Another batch is running");
				nlohmann::json results = nlohmann::json::array();
				bool runningCopy = false;
				for (size_t index = 0; index < script->GetStepCount(); index++)
				{
					const EditorCommandResult result = script->RunStepNow(index, context, registry);
					const std::string& name = script->GetStep(index).Command;
					runningCopy = runningCopy || result.RunningCopy;
					if (result.Success)
					{
						results.push_back({ { "command", name }, { "result", result.Value } });
						continue;
					}
					results.push_back({ { "command", name }, { "error", DescribeStepError(result) } });
					if (atomic)
					{
						// The steps before it go, as if the batch had not run.
						std::string undoError;
						const bool reverted = undo.CancelGroup(&undoError);
						return EditorCommandResult::Fail(fmt::format("Step {} ({}) failed: {}{}", index, name, result.Error,
							reverted ? std::string(" (the steps before it were undone)") : fmt::format(" (undoing the steps before it failed: {})", undoError)),
							result.ErrorKind == EditorCommandError::InvalidParameters ? EditorCommandError::InvalidParameters : EditorCommandError::Failed);
					}
				}
				undo.EndGroup();
				nlohmann::json bindings = nlohmann::json::object();
				for (const auto& [name, value] : script->GetBindings())
					bindings[name] = value;
				nlohmann::json value = { { "steps", std::move(results) }, { "bindings", std::move(bindings) } };
				EditorCommandResult answer = EditorCommandResult::Ok(std::move(value));
				if (runningCopy)
				{
					answer.Value["warning"] = "Steps changed the running copy of the playing scene: play.stop discards those changes";
					answer.RunningCopy = true;
				}
				return answer;
			}, EditorCommandKind::Edit });
	}

}
