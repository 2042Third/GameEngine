#pragma once

#include <doctest/doctest.h>

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

#include <Strata/Core/Timestep.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <thread>

namespace Strata::Tests
{

	// Completes a command result the way EditorCommandRunner does: while it is pending, one editor frame
	// (EditorContext::Update) and one poll at a time, until it is final or `timeout` passed (REQUIREs that it finished).
	inline EditorCommandResult CompleteCommand(EditorContext& context, EditorCommandResult result, std::chrono::milliseconds timeout = std::chrono::minutes(2))
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (result.IsPending())
		{
			REQUIRE_MESSAGE(std::chrono::steady_clock::now() < deadline, "The command did not finish in time");
			context.Update(Timestep(1.0f / 60.0f));
			if (!PollCommandResult(result, context))
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return result;
	}

	// Runs a command to completion (CompleteCommand) and REQUIREs that it succeeded; returns its value.
	inline nlohmann::json RunCommandToCompletion(EditorContext& context, const EditorCommandRegistry& commands, std::string_view name,
		const nlohmann::json& parameters = nlohmann::json::object())
	{
		const EditorCommandResult result = CompleteCommand(context, commands.Execute(context, name, parameters));
		INFO(std::string(name), ": ", result.Error);
		REQUIRE(result.Success);
		return result.Value;
	}

}
