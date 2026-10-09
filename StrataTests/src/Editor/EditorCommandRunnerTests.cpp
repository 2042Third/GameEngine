#include <doctest/doctest.h>

#include "Editor/CommandUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	struct RunnerHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;
		std::vector<EditorCommandResult> Results; // Completions, in the order they were called

		RunnerHarness()
		{
			// Finishes after `polls` polls with {"polls": n}; fails instead when "fail" is set.
			Commands.Register({ "test.defer", "Test command that finishes after a number of polls.",
				CommandUtils::ObjectSchema({ { "polls", CommandUtils::IntegerSchema("Polls", 1, 100) }, { "fail", CommandUtils::BoolSchema("Fail") } }),
				[](EditorContext&, const nlohmann::json& parameters)
				{
					CommandArguments arguments(parameters);
					const int64_t polls = arguments.GetInt("polls", 1, 1, 100);
					const bool fail = arguments.GetBool("fail", false);
					if (!arguments.IsValid())
						return arguments.Fail();
					return EditorCommandResult::Defer([polls, fail, count = int64_t(0)](EditorContext&) mutable -> std::optional<EditorCommandResult>
					{
						if (++count < polls)
							return std::nullopt;
						return fail ? EditorCommandResult::Fail("Failed on purpose") : EditorCommandResult::Ok({ { "polls", count } });
					});
				} });
			// A poll function that throws, as third-party code (JSON access) may.
			Commands.Register({ "test.throw", "Test command whose poll throws.", CommandUtils::ObjectSchema({}),
				[](EditorContext&, const nlohmann::json&)
				{
					return EditorCommandResult::Defer([](EditorContext&) -> std::optional<EditorCommandResult>
					{
						throw std::runtime_error("broken poll");
					});
				} });
			// A poll that finishes with another pending result (a command made of stages).
			Commands.Register({ "test.chain", "Test command with two pending stages.", CommandUtils::ObjectSchema({}),
				[](EditorContext&, const nlohmann::json&)
				{
					return EditorCommandResult::Defer([](EditorContext&) -> std::optional<EditorCommandResult>
					{
						return EditorCommandResult::Defer([](EditorContext&) -> std::optional<EditorCommandResult>
						{
							return EditorCommandResult::Ok("second stage");
						});
					});
				} });
		}

		bool Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Runner.Run(Context, Commands, name, parameters, [this](const EditorCommandResult& result) { Results.push_back(result); });
		}
	};

}

TEST_SUITE("Editor.CommandRunner")
{
	TEST_CASE("Commands that finish at once complete before Run returns")
	{
		RunnerHarness harness;
		CHECK_FALSE(harness.Run("selection.get"));
		REQUIRE(harness.Results.size() == 1);
		CHECK(harness.Results[0].Success);
		CHECK(harness.Results[0].Value.contains("entities"));
		CHECK(harness.Runner.GetPendingCount() == 0);

		CHECK_FALSE(harness.Run("no.such.command"));
		REQUIRE(harness.Results.size() == 2);
		CHECK_FALSE(harness.Results[1].Success);
	}

	TEST_CASE("Pending commands are polled once per update and complete in order")
	{
		RunnerHarness harness;
		CHECK(harness.Run("test.defer", { { "polls", 3 } }));
		CHECK(harness.Run("test.defer", { { "polls", 1 } }));
		CHECK(harness.Runner.GetPendingCount() == 2);
		CHECK(harness.Results.empty());

		harness.Runner.Update(harness.Context);
		REQUIRE(harness.Results.size() == 1);
		CHECK(harness.Results[0].Value["polls"] == 1);
		harness.Runner.Update(harness.Context);
		CHECK(harness.Results.size() == 1);
		harness.Runner.Update(harness.Context);
		REQUIRE(harness.Results.size() == 2);
		CHECK(harness.Results[1].Value["polls"] == 3);
		CHECK(harness.Runner.GetPendingCount() == 0);
		CHECK_FALSE(harness.Results[1].IsPending());
	}

	TEST_CASE("Invalid parameters fail at once instead of deferring")
	{
		RunnerHarness harness;
		CHECK_FALSE(harness.Run("test.defer", { { "polls", 0 } }));
		REQUIRE(harness.Results.size() == 1);
		CHECK_FALSE(harness.Results[0].Success);
		CHECK(harness.Runner.GetPendingCount() == 0);
	}

	TEST_CASE("Failures, exceptions and staged commands")
	{
		RunnerHarness harness;
		harness.Run("test.defer", { { "polls", 1 }, { "fail", true } });
		harness.Run("test.throw");
		harness.Run("test.chain");
		harness.Runner.Update(harness.Context);
		REQUIRE(harness.Results.size() == 2);
		CHECK(harness.Results[0].Error == "Failed on purpose");
		CHECK_FALSE(harness.Results[1].Success);
		CHECK(harness.Results[1].Error.find("broken poll") != std::string::npos);
		CHECK(harness.Runner.GetPendingCount() == 1); // The chain moved to its second stage

		harness.Runner.Update(harness.Context);
		REQUIRE(harness.Results.size() == 3);
		CHECK(harness.Results[2].Value == "second stage");
	}

	TEST_CASE("Commands issued by completions are polled from the next update")
	{
		RunnerHarness harness;
		std::vector<std::string> order;
		harness.Runner.Run(harness.Context, harness.Commands, "test.defer", { { "polls", 1 } }, [&](const EditorCommandResult&)
		{
			order.push_back("first");
			// Issued during the update: must not be polled by it, even though it needs a single poll.
			harness.Runner.Run(harness.Context, harness.Commands, "test.defer", { { "polls", 1 } }, [&](const EditorCommandResult&) { order.push_back("second"); });
		});
		harness.Runner.Update(harness.Context);
		CHECK(order == std::vector<std::string> { "first" });
		CHECK(harness.Runner.GetPendingCount() == 1);
		harness.Runner.Update(harness.Context);
		CHECK(order == std::vector<std::string> { "first", "second" });
	}

	TEST_CASE("Cancelling completes every pending command with the reason")
	{
		RunnerHarness harness;
		harness.Run("test.defer", { { "polls", 5 } });
		harness.Run("test.defer", { { "polls", 5 } });
		harness.Runner.CancelAll("Closing");
		REQUIRE(harness.Results.size() == 2);
		for (const EditorCommandResult& result : harness.Results)
		{
			CHECK_FALSE(result.Success);
			CHECK(result.Error == "Closing");
		}
		CHECK(harness.Runner.GetPendingCount() == 0);
		harness.Runner.Update(harness.Context);
		CHECK(harness.Results.size() == 2);

		SUBCASE("A completion may cancel the commands polled after it in the same update")
		{
			RunnerHarness inner;
			inner.Runner.Run(inner.Context, inner.Commands, "test.defer", { { "polls", 1 } }, [&](const EditorCommandResult&) { inner.Runner.CancelAll("Stop"); });
			inner.Run("test.defer", { { "polls", 1 } });
			inner.Runner.Update(inner.Context);
			REQUIRE(inner.Results.size() == 1);
			CHECK(inner.Results[0].Error == "Stop");
			CHECK(inner.Runner.GetPendingCount() == 0);
		}
	}

	TEST_CASE("Destroying the runner cancels pending commands")
	{
		EditorContext context(EditorContextSpecification { false });
		EditorCommandRegistry commands;
		std::string error;
		{
			EditorCommandRunner runner;
			runner.Run(context, commands, "editor.wait", { { "frames", 10 } }, [&](const EditorCommandResult& result) { error = result.Error; });
		}
		CHECK_FALSE(error.empty());
	}

	TEST_CASE("editor.wait finishes after the given number of frames")
	{
		RunnerHarness harness;
		CHECK(harness.Run("editor.wait", { { "frames", 3 } }));
		harness.Runner.Update(harness.Context);
		harness.Runner.Update(harness.Context);
		CHECK(harness.Results.empty());
		harness.Runner.Update(harness.Context);
		REQUIRE(harness.Results.size() == 1);
		CHECK(harness.Results[0].Value["frames"] == 3);

		// Default: one frame.
		CHECK(harness.Run("editor.wait"));
		harness.Runner.Update(harness.Context);
		REQUIRE(harness.Results.size() == 2);
		CHECK(harness.Results[1].Value["frames"] == 1);

		for (const nlohmann::json& frames : { nlohmann::json(0), nlohmann::json(-1), nlohmann::json(1'000'001), nlohmann::json("2"), nlohmann::json(1.5) })
		{
			INFO("frames = ", frames.dump());
			CHECK_FALSE(harness.Run("editor.wait", { { "frames", frames } }));
			CHECK_FALSE(harness.Results.back().Success);
		}
		CHECK(harness.Runner.GetPendingCount() == 0);
	}
}

TEST_SUITE("Editor.CommandScript")
{
	TEST_CASE("Scripts are validated before anything runs")
	{
		std::string error;
		CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::object(), &error));
		CHECK(error.find("array") != std::string::npos);
		CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::parse(R"([ "scene.new" ])"), &error));
		CHECK(error.find("Step 1") != std::string::npos);
		CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::parse(R"([ { "command": "scene.new" }, { "parameters": {} } ])"), &error));
		CHECK(error.find("Step 2") != std::string::npos);
		CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::parse(R"([ { "command": "" } ])"), &error));
		CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::parse(R"([ { "command": "scene.new", "parameters": [] } ])"), &error));
		CHECK(error.find("parameters") != std::string::npos);
		CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::parse(R"([ { "command": "scene.new", "parameter": {} } ])"), &error));
		CHECK(error.find("unknown key 'parameter'") != std::string::npos);

		Scope<EditorCommandScript> empty = EditorCommandScript::FromJson(nlohmann::json::array(), &error);
		REQUIRE(empty);
		CHECK(empty->IsFinished());
	}

	TEST_CASE("Malformed expectations are rejected before anything runs")
	{
		struct Case
		{
			const char* Expect;
			const char* Error;
		};
		const Case cases[] = {
			{ R"([])", "\"expect\" must be an object" },
			{ R"({ "values": { "equals": 1 } })", "'values' in \"expect\" is not a JSON pointer" },
			{ R"({ "/a~2": { "equals": 1 } })", "'/a~2' in \"expect\" is not a JSON pointer" },
			{ R"({ "/a": 1 })", "the condition of '/a' must be an object" },
			{ R"({ "/a": {} })", "the condition of '/a' must be an object" },
			{ R"({ "/a": { "min": "1" } })", "invalid condition 'min' for '/a'" },
			{ R"({ "/a": { "near": 1 } })", "invalid condition 'near' for '/a'" },
			{ R"({ "/a": { "min": 2, "max": 1 } })", "minimum above its maximum" },
		};
		for (const Case& testCase : cases)
		{
			INFO("expect: ", testCase.Expect);
			const nlohmann::json script = nlohmann::json::array({ { { "command", "scene.new" }, { "expect", nlohmann::json::parse(testCase.Expect) } } });
			std::string error;
			CHECK_FALSE(EditorCommandScript::FromJson(script, &error));
			CHECK(error.find("Step 1 (scene.new)") != std::string::npos);
			CHECK(error.find(testCase.Error) != std::string::npos);
		}

		// Escapes of JSON pointers ("~0" is '~', "~1" is '/') and the whole result ("").
		std::string error;
		CHECK(EditorCommandScript::FromJson(nlohmann::json::parse(R"([ { "command": "scene.new", "expect": { "/a~0b/c~1d/0": { "equals": 1 }, "": { "equals": null } } } ])"), &error));
	}

	TEST_CASE("Expectations check the results of successful steps")
	{
		auto run = [](const char* json)
		{
			RunnerHarness harness;
			std::string error;
			Scope<EditorCommandScript> script = EditorCommandScript::FromJson(nlohmann::json::parse(json), &error);
			REQUIRE_MESSAGE(script, error);
			for (int32_t frame = 0; frame < 10 && !script->Update(harness.Runner, harness.Context, harness.Commands); frame++)
				harness.Runner.Update(harness.Context);
			REQUIRE(script->IsFinished());
			return !script->HasFailed();
		};

		CHECK(run(R"([
			{ "command": "test.defer", "parameters": { "polls": 2 }, "expect": { "/polls": { "equals": 2, "min": 1.5, "max": 2.5 }, "": { "equals": { "polls": 2 } } } },
			{ "command": "editor.wait", "parameters": { "frames": 1 }, "expect": { "/frames": { "min": 1, "max": 1 } } },
			{ "command": "selection.get", "expect": { "/entities": { "equals": [] } } }
		])"));

		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		CHECK_FALSE(run(R"([ { "command": "test.defer", "parameters": { "polls": 2 }, "expect": { "/polls": { "equals": 3 } } } ])"));
		CHECK_FALSE(run(R"([ { "command": "test.defer", "parameters": { "polls": 2 }, "expect": { "/polls": { "max": 1.5 } } } ])"));
		CHECK_FALSE(run(R"([ { "command": "test.defer", "parameters": { "polls": 2 }, "expect": { "/polls": { "min": 2.5 } } } ])"));
		CHECK_FALSE(run(R"([ { "command": "test.defer", "parameters": { "polls": 2 }, "expect": { "/missing": { "equals": 1 } } } ])"));
		CHECK_FALSE(run(R"([ { "command": "test.defer", "parameters": { "polls": 2 }, "expect": { "": { "min": 0 } } } ])"));
		CHECK_FALSE(run(R"([ { "command": "selection.get", "expect": { "/entities/0": { "equals": "0000000000000001" } } } ])"));
		CHECK_FALSE(run(R"([ { "command": "selection.get", "expect": { "/entities/x": { "equals": 1 } } } ])"));

		// Each failure says what was expected and what the result has.
		std::vector<std::string> errors;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(logStart))
		{
			if (entry.Level == LogLevel::Error && entry.Message.find(": expected ") != std::string::npos)
				errors.push_back(entry.Message);
		}
		const std::vector<std::string> expected = {
			"test.defer: expected '/polls' to be 3, but it is 2",
			"test.defer: expected '/polls' to be a number in [-inf, 1.5], but it is 2",
			"test.defer: expected '/polls' to be a number in [2.5, inf], but it is 2",
			"test.defer: expected a value at '/missing', but the result has none",
			"test.defer: expected '' to be a number in [0, inf], but it is {\"polls\":2}",
			"selection.get: expected a value at '/entities/0', but the result has none",
			"selection.get: expected a value at '/entities/x', but the result has none",
		};
		CHECK(errors == expected);
	}

	TEST_CASE("Scripts load from files")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("CommandScript");
		std::string error;
		CHECK_FALSE(EditorCommandScript::Load(directory / "Missing.json", &error));
		CHECK(error.find("Cannot read") != std::string::npos);

		REQUIRE(FileSystem::WriteText(directory / "Broken.json", "[ { \"command\": "));
		CHECK_FALSE(EditorCommandScript::Load(directory / "Broken.json", &error));
		CHECK(error.find("not valid JSON") != std::string::npos);

		REQUIRE(FileSystem::WriteText(directory / "Script.json", R"([ { "command": "scene.new", "parameters": { "name": "Loaded" } } ])"));
		Scope<EditorCommandScript> script = EditorCommandScript::Load(directory / "Script.json", &error);
		REQUIRE(script);
		CHECK(script->GetStepCount() == 1);
	}

	TEST_CASE("Pending steps hold the script until they complete")
	{
		RunnerHarness harness;
		Scope<EditorCommandScript> script = EditorCommandScript::FromJson(nlohmann::json::parse(R"([
			{ "command": "scene.new", "parameters": { "name": "First" } },
			{ "command": "editor.wait", "parameters": { "frames": 2 } },
			{ "command": "scene.new", "parameters": { "name": "Second" } }
		])"));
		REQUIRE(script);

		// The first frame runs up to the pending step.
		CHECK_FALSE(script->Update(harness.Runner, harness.Context, harness.Commands));
		CHECK(harness.Context.GetEditScene()->GetName() == "First");
		CHECK(script->GetCompletedCount() == 1);

		harness.Runner.Update(harness.Context);
		CHECK_FALSE(script->Update(harness.Runner, harness.Context, harness.Commands));
		CHECK(harness.Context.GetEditScene()->GetName() == "First");

		harness.Runner.Update(harness.Context);
		CHECK(script->Update(harness.Runner, harness.Context, harness.Commands));
		CHECK(harness.Context.GetEditScene()->GetName() == "Second");
		CHECK(script->IsFinished());
		CHECK_FALSE(script->HasFailed());
		CHECK(script->GetCompletedCount() == 3);
	}

	TEST_CASE("A failed step marks the script failed and the rest still runs")
	{
		RunnerHarness harness;
		Scope<EditorCommandScript> script = EditorCommandScript::FromJson(nlohmann::json::parse(R"([
			{ "command": "no.such.command" },
			{ "command": "test.defer", "parameters": { "polls": 1, "fail": true } },
			{ "command": "scene.new", "parameters": { "name": "After" } }
		])"));
		REQUIRE(script);
		CHECK_FALSE(script->Update(harness.Runner, harness.Context, harness.Commands));
		CHECK(script->HasFailed());
		harness.Runner.Update(harness.Context);
		CHECK(script->Update(harness.Runner, harness.Context, harness.Commands));
		CHECK(harness.Context.GetEditScene()->GetName() == "After");
		CHECK(script->HasFailed());
	}

	TEST_CASE("Cancelling the runner ends a waiting script")
	{
		RunnerHarness harness;
		Scope<EditorCommandScript> script = EditorCommandScript::FromJson(nlohmann::json::parse(R"([
			{ "command": "editor.wait", "parameters": { "frames": 100 } },
			{ "command": "scene.new", "parameters": { "name": "Never" } }
		])"));
		REQUIRE(script);
		script->Update(harness.Runner, harness.Context, harness.Commands);
		harness.Runner.CancelAll("Closing");
		CHECK(script->HasFailed());
		CHECK(script->GetCompletedCount() == 1);
	}
}
