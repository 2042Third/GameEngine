#include <doctest/doctest.h>

#include "Editor/CommandUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>

#include <chrono>
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
			Commands.Register(MayDefer({ "test.defer", "Test command that finishes after a number of polls.",
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
				} }));
			// A poll function that throws, as third-party code (JSON access) may.
			Commands.Register(MayDefer({ "test.throw", "Test command whose poll throws.", CommandUtils::ObjectSchema({}),
				[](EditorContext&, const nlohmann::json&)
				{
					return EditorCommandResult::Defer([](EditorContext&) -> std::optional<EditorCommandResult>
					{
						throw std::runtime_error("broken poll");
					});
				} }));
			// A poll that finishes with another pending result (a command made of stages).
			Commands.Register(MayDefer({ "test.chain", "Test command with two pending stages.", CommandUtils::ObjectSchema({}),
				[](EditorContext&, const nlohmann::json&)
				{
					return EditorCommandResult::Defer([](EditorContext&) -> std::optional<EditorCommandResult>
					{
						return EditorCommandResult::Defer([](EditorContext&) -> std::optional<EditorCommandResult>
						{
							return EditorCommandResult::Ok("second stage");
						});
					});
				} }));
		}

		bool Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			return Runner.Run(Context, Commands, name, parameters, [this](const EditorCommandResult& result) { Results.push_back(result); });
		}

		// Registers test.steps, a query that answers the game's fixed steps ({"steps": n}), for "until" waits.
		void RegisterStepQuery()
		{
			EditorCommand steps { "test.steps", "Test query that answers the game's fixed steps.", CommandUtils::ObjectSchema({}),
				[](EditorContext& context, const nlohmann::json&) { return EditorCommandResult::Ok({ { "steps", context.GetFixedStepCount() } }); } };
			steps.Kind = EditorCommandKind::Query;
			Commands.Register(std::move(steps));
		}

		// Runs a script frame by frame, as the editor does (the script, the runner, then the game's update), until it
		// finishes or `seconds` of wall-clock time passed.
		void RunScript(EditorCommandScript& script, double seconds = 10.0)
		{
			const auto start = std::chrono::steady_clock::now();
			while (!script.Update(Runner, Context, Commands) && std::chrono::steady_clock::now() - start < std::chrono::duration<double>(seconds))
			{
				Runner.Update(Context);
				Context.Update(Timestep(1.0f / 60.0f));
			}
		}

		// The activity journal's entries of a command.
		std::vector<const CommandJournalEntry*> GetJournalEntries(std::string_view command)
		{
			std::vector<const CommandJournalEntry*> entries;
			for (const CommandJournalEntry& entry : Context.GetJournal().GetEntries())
			{
				if (entry.Command == command)
					entries.push_back(&entry);
			}
			return entries;
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
		CHECK(harness.Results[0].ErrorKind == EditorCommandError::InvalidParameters);
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
		CHECK(harness.Results[0].ErrorKind == EditorCommandError::Failed);
		CHECK_FALSE(harness.Results[1].Success);
		CHECK(harness.Results[1].Error.find("broken poll") != std::string::npos);
		CHECK(harness.Results[1].ErrorKind == EditorCommandError::Internal);
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
			CHECK(result.ErrorKind == EditorCommandError::Cancelled);
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

	TEST_CASE("editor.wait reports the CPU time of the frames it waited through")
	{
		// Without an application recording frame times there are no statistics.
		RunnerHarness harness;
		CHECK(harness.Run("editor.wait", { { "frames", 2 } }));
		harness.Runner.Update(harness.Context);
		harness.Runner.Update(harness.Context);
		REQUIRE(harness.Results.size() == 1);
		CHECK_FALSE(harness.Results[0].Value.contains("frameTimes"));

		// Frames as the editor layer runs them: the time of the frame before, then the commands. The wait starts in a frame
		// whose time (9 ms) also covers whatever ran before it, so only the frames after that one count.
		harness.Context.RecordFrameTime(50.0);
		CHECK(harness.Run("editor.wait", { { "frames", 5 } }));
		for (const double frameTime : { 9.0, 1.0, 4.0, 2.0, 3.0 })
		{
			CHECK(harness.Results.size() == 1);
			harness.Context.RecordFrameTime(frameTime);
			harness.Runner.Update(harness.Context);
		}
		REQUIRE(harness.Results.size() == 2);
		CHECK(harness.Results[1].Value["frames"] == 5);
		const nlohmann::json& frameTimes = harness.Results[1].Value["frameTimes"];
		CHECK(frameTimes["count"] == 4);
		CHECK(frameTimes["medianMs"] == 2.5);
		CHECK(frameTimes["meanMs"] == 2.5);
		CHECK(frameTimes["p95Ms"] == 4.0);
		CHECK(frameTimes["maxMs"] == 4.0);
		CHECK(harness.Context.GetRecordedFrameCount() == 6);
		CHECK(harness.Context.GetLastFrameTime() == 3.0);
	}

	TEST_CASE("Frame times go into storage reserved when the command starts, which copies of its poll function share")
	{
		// Recording in the frames a command measures must not allocate there (CommandUtils::FrameTimeRecorder): a frame
		// that waits for the heap while a scene is freed in the background measures the wait.
		RunnerHarness harness;
		CommandUtils::FrameTimeRecorder recorder(harness.Context.GetRecordedFrameCount() + 1, 16);
		const size_t capacity = recorder.GetCapacity();
		CHECK(capacity >= 16);
		recorder.Update(harness.Context); // Before the first measured frame: nothing
		CHECK(recorder.IsEmpty());
		const CommandUtils::FrameTimeRecorder copy = recorder;
		for (int frame = 1; frame <= 16; frame++)
		{
			harness.Context.RecordFrameTime(static_cast<double>(frame));
			recorder.Update(harness.Context);
			recorder.Update(harness.Context); // Polled twice in a frame: recorded once
		}
		CHECK(recorder.GetCount() == 16);
		CHECK(recorder.GetCapacity() == capacity); // Never reallocated
		CHECK(copy.GetCount() == 16);
		const nlohmann::json described = copy.Describe();
		CHECK(described["count"] == 16);
		CHECK(described["medianMs"] == 8.5);
		CHECK(described["maxMs"] == 16.0);

		// The commands' reserve holds the frames of the longest open the perf tests measure (PerfGPU.Editor's scene.open of a
		// million entities records up to 12,992), which would otherwise grow the storage in one of the frames it measures.
		CHECK(CommandUtils::c_FrameTimeReserve >= 13000);
		const CommandUtils::FrameTimeRecorder commandRecorder(harness.Context.GetRecordedFrameCount() + 1, CommandUtils::c_FrameTimeReserve);
		CHECK(commandRecorder.GetCapacity() >= CommandUtils::c_FrameTimeReserve);
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

	TEST_CASE("Until repeats a query until its expectation holds")
	{
		RunnerHarness harness;
		harness.RegisterStepQuery();
		REQUIRE(harness.Context.Play());
		std::string error;
		Scope<EditorCommandScript> script = EditorCommandScript::FromJson(nlohmann::json::parse(R"([
			{ "command": "test.steps", "expect": { "/steps": { "min": 5 } }, "until": { "steps": 600 }, "bind": { "steps": "/steps" } },
			{ "command": "selection.get" }
		])"), &error);
		REQUIRE_MESSAGE(script, error);
		harness.RunScript(*script);
		REQUIRE(script->IsFinished());
		CHECK_FALSE(script->HasFailed());
		// The condition was false on the first attempt: the step waited for the game, and bind read the final result.
		CHECK(script->GetBindings().at("steps").get<uint64_t>() >= 5);
		const std::vector<const CommandJournalEntry*> entries = harness.GetJournalEntries("test.steps");
		REQUIRE(entries.size() == 1);
		CHECK(entries[0]->Status == CommandStatus::Succeeded);
		CHECK(entries[0]->Source == CommandSource::CommandScript);
		const nlohmann::json summary = nlohmann::json::parse(entries[0]->ResultSummary);
		CHECK(summary["attempts"].get<int64_t>() > 1);
		CHECK(harness.GetJournalEntries("selection.get").size() == 1);
	}

	TEST_CASE("Until steps a paused game exactly, one fixed step per attempt")
	{
		RunnerHarness harness;
		harness.RegisterStepQuery();
		REQUIRE(harness.Context.Play());
		harness.Context.SetPaused(true);
		std::string error;
		Scope<EditorCommandScript> script = EditorCommandScript::FromJson(nlohmann::json::parse(R"([
			{ "command": "test.steps", "expect": { "/steps": { "min": 5 } }, "until": { "steps": 600 }, "bind": { "steps": "/steps" } }
		])"), &error);
		REQUIRE_MESSAGE(script, error);
		harness.RunScript(*script);
		REQUIRE(script->IsFinished());
		CHECK_FALSE(script->HasFailed());
		CHECK(script->GetBindings().at("steps") == 5);
		CHECK(harness.Context.GetFixedStepCount() == 5);
		CHECK(harness.Context.IsPaused());
		const std::vector<const CommandJournalEntry*> entries = harness.GetJournalEntries("test.steps");
		REQUIRE(entries.size() == 1);
		CHECK(nlohmann::json::parse(entries[0]->ResultSummary)["attempts"] == 6);
	}

	TEST_CASE("Until fails once its bounds are reached")
	{
		RunnerHarness harness;
		harness.RegisterStepQuery();
		REQUIRE(harness.Context.Play());
		harness.Context.SetPaused(true);
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		Scope<EditorCommandScript> script = EditorCommandScript::FromJson(nlohmann::json::parse(R"([
			{ "command": "test.steps", "expect": { "/steps": { "min": 1000 } }, "until": { "steps": 20 }, "bind": { "steps": "/steps" } }
		])"));
		REQUIRE(script);
		harness.RunScript(*script);
		REQUIRE(script->IsFinished());
		CHECK(script->HasFailed());
		CHECK(harness.Context.GetFixedStepCount() == 20);
		CHECK_FALSE(script->GetBindings().contains("steps"));
		bool reported = false;
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(logStart))
		{
			if (entry.Level == LogLevel::Error && entry.Message.find("expected '/steps' to be a number in [1000, inf], but it is 20 (until: 21 attempts in 20 fixed steps") != std::string::npos)
				reported = entry.Message.find("the last result: {\"steps\":20}") != std::string::npos;
		}
		CHECK(reported);
		REQUIRE(harness.GetJournalEntries("test.steps").size() == 1);
		CHECK(harness.GetJournalEntries("test.steps")[0]->Status == CommandStatus::Succeeded); // The command itself answered

		// A game that neither plays nor steps: only the seconds end the wait.
		harness.Context.Stop();
		Scope<EditorCommandScript> idle = EditorCommandScript::FromJson(nlohmann::json::parse(R"([
			{ "command": "test.steps", "expect": { "/steps": { "min": 1 } }, "until": { "seconds": 0.2 } }
		])"));
		REQUIRE(idle);
		const auto start = std::chrono::steady_clock::now();
		harness.RunScript(*idle);
		CHECK(idle->IsFinished());
		CHECK(idle->HasFailed());
		CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(200));
	}

	TEST_CASE("Malformed waits are refused before anything runs")
	{
		struct Case
		{
			const char* Step;
			const char* Error;
		};
		const Case cases[] = {
			{ R"({ "command": "test.steps", "until": { "steps": 5 } })", "give an \"expect\"" },
			{ R"({ "command": "test.steps", "expect": { "/steps": { "min": 1 } }, "until": 5 })", "\"until\" must be an object" },
			{ R"({ "command": "test.steps", "expect": { "/steps": { "min": 1 } }, "until": {} })", "\"until\" must be an object" },
			{ R"({ "command": "test.steps", "expect": { "/steps": { "min": 1 } }, "until": { "steps": 0 } })", "\"steps\" must be an integer from 1" },
			{ R"({ "command": "test.steps", "expect": { "/steps": { "min": 1 } }, "until": { "steps": 1.5 } })", "\"steps\" must be an integer from 1" },
			{ R"({ "command": "test.steps", "expect": { "/steps": { "min": 1 } }, "until": { "seconds": 0 } })", "\"seconds\" must be a number above 0" },
			{ R"({ "command": "test.steps", "expect": { "/steps": { "min": 1 } }, "until": { "seconds": -1 } })", "\"seconds\" must be a number above 0" },
			{ R"({ "command": "test.steps", "expect": { "/steps": { "min": 1 } }, "until": { "frames": 5 } })", "unknown key 'frames'" },
		};
		for (const Case& testCase : cases)
		{
			INFO("step: ", testCase.Step);
			std::string error;
			CHECK_FALSE(EditorCommandScript::FromJson(nlohmann::json::array({ nlohmann::json::parse(testCase.Step) }), &error));
			CHECK(error.find("Step 1 (test.steps)") != std::string::npos);
			CHECK(error.find(testCase.Error) != std::string::npos);
		}

		// A wait on a command that is not a query, or that does not exist, fails the script before its first step runs.
		for (const char* command : { "entity.create", "no.such.command" })
		{
			INFO("command: ", command);
			RunnerHarness harness;
			const nlohmann::json steps = nlohmann::json::array({ { { "command", "entity.create" }, { "parameters", { { "name", "First" } } } },
				{ { "command", command }, { "expect", { { "/id", { { "min", 0 } } } } }, { "until", { { "steps", 5 } } } } });
			Scope<EditorCommandScript> script = EditorCommandScript::FromJson(steps);
			REQUIRE(script);
			harness.RunScript(*script);
			CHECK(script->IsFinished());
			CHECK(script->HasFailed());
			CHECK(harness.GetJournalEntries("entity.create").empty());
		}

		// editor.batch runs its steps within one frame: it refuses waits.
		RunnerHarness harness;
		harness.RegisterStepQuery();
		const EditorCommandResult batch = harness.Commands.Execute(harness.Context, "editor.batch", { { "steps", nlohmann::json::array({
			{ { "command", "test.steps" }, { "expect", { { "/steps", { { "min", 1 } } } } }, { "until", { { "steps", 5 } } } } }) } });
		CHECK_FALSE(batch.Success);
		CHECK(batch.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(batch.Error.find("\"until\"") != std::string::npos);
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
