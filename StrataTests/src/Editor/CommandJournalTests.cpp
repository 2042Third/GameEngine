#include <doctest/doctest.h>

#include "Editor/CommandJournal.h"
#include "Editor/CommandTestUtils.h"
#include "Editor/CommandUtils.h"
#include "Editor/EditorAutomation.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Network/NetworkTestHelpers.h"
#include "TestHelpers.h"
#include "UI/EditorActions.h"

#include <Strata/Core/Timestep.h>
#include <Strata/Network/RpcClient.h>
#include <Strata/Reflection/PropertyJson.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

// The activity journal (CommandJournal): every command the editor runs, recorded once, with who ran it and how it went.

using namespace Strata;

namespace
{

	constexpr std::chrono::milliseconds c_CallTimeout = std::chrono::milliseconds(10000);

	struct JournalHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;
		EditorAutomation Automation { Context, Commands, Runner };

		JournalHarness()
		{
			// A command that runs another one, as some commands do.
			Commands.Register({ "test.nested", "Test command that creates an entity through entity.create.", CommandUtils::ObjectSchema({}),
				[this](EditorContext& context, const nlohmann::json&)
				{
					return Commands.Execute(context, "entity.create", { { "name", "Inner" } });
				}, EditorCommandKind::Edit });
		}

		~JournalHarness()
		{
			Runner.CancelAll("The test is over");
			Automation.Stop();
		}

		CommandJournal& Journal() { return Context.GetJournal(); }

		void Frame()
		{
			Context.Update(Timestep(1.0f / 60.0f));
			Runner.Update(Context);
			Automation.Update();
		}

		bool FramesUntil(const std::function<bool()>& condition)
		{
			const auto deadline = std::chrono::steady_clock::now() + c_CallTimeout;
			while (!condition())
			{
				if (std::chrono::steady_clock::now() > deadline)
					return false;
				Frame();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return true;
		}

		// The entries of a command, oldest first.
		std::vector<const CommandJournalEntry*> Entries(std::string_view command)
		{
			std::vector<const CommandJournalEntry*> entries;
			for (const CommandJournalEntry& entry : Journal().GetEntries())
			{
				if (entry.Command == command)
					entries.push_back(&entry);
			}
			return entries;
		}

		const CommandJournalEntry& Latest() { return Journal().GetEntries().back(); }
	};

}

TEST_SUITE("Editor.Journal")
{
	TEST_CASE("Commands of every source are recorded once, with their source and the client's name")
	{
		JournalHarness harness;

		// The UI.
		CHECK_FALSE(RunEditorCommand(harness.Context, harness.Commands, "entity.create", { { "name", "FromUI" } }).is_null());
		REQUIRE(harness.Entries("entity.create").size() == 1);
		CHECK(harness.Latest().Source == CommandSource::UI);
		CHECK(harness.Latest().Status == CommandStatus::Succeeded);
		CHECK(harness.Latest().Parameters == nlohmann::json { { "name", "FromUI" } });

		// The runner, for a command script.
		Scope<EditorCommandScript> script = EditorCommandScript::FromJson(nlohmann::json::array({
			{ { "command", "entity.create" }, { "parameters", { { "name", "FromScript" } } } },
			{ { "command", "scene.info" } } }));
		REQUIRE(script);
		REQUIRE(harness.FramesUntil([&]() { return script->Update(harness.Runner, harness.Context, harness.Commands); }));
		CHECK_FALSE(script->HasFailed());
		REQUIRE(harness.Entries("entity.create").size() == 2);
		CHECK(harness.Entries("entity.create")[1]->Source == CommandSource::CommandScript);
		REQUIRE(harness.Entries("scene.info").size() == 1);
		CHECK(harness.Entries("scene.info")[0]->Source == CommandSource::CommandScript);

		// An automation client that named itself in its handshake.
		EditorAutomationSpecification specification;
		specification.AuthToken = Tests::c_TestServerToken;
		specification.PublishSession = false;
		std::string error;
		REQUIRE_MESSAGE(harness.Automation.Start(specification, &error), error);
		RpcClient client;
		REQUIRE_MESSAGE(client.Connect("127.0.0.1", harness.Automation.GetPort(), Tests::c_TestServerToken, std::chrono::milliseconds(5000), "TestAgent"),
			client.GetLastError());
		std::future<RpcResult> call = std::async(std::launch::async, [&client]() { return client.Call("entity.create", { { "name", "FromAgent" } }, c_CallTimeout); });
		REQUIRE(harness.FramesUntil([&call]() { return call.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }));
		REQUIRE(call.get().IsSuccess());
		REQUIRE(harness.Entries("entity.create").size() == 3);
		CHECK(harness.Entries("entity.create")[2]->Source == CommandSource::Automation);
		CHECK(harness.Entries("entity.create")[2]->Client == "TestAgent");
		// editor.status names who is connected.
		const nlohmann::json clients = harness.Automation.DescribeStatus()["connectedClients"];
		REQUIRE(clients.size() == 1);
		CHECK(clients[0]["name"] == "TestAgent");

		// Code that runs a command directly: the editor itself.
		REQUIRE(harness.Commands.Execute(harness.Context, "scene.info").Success);
		CHECK(harness.Latest().Source == CommandSource::Internal);
		CHECK(harness.Entries("scene.info").size() == 2);
	}

	TEST_CASE("A deferred command is recorded once, when it finishes")
	{
		JournalHarness harness;
		bool done = false;
		REQUIRE(harness.Runner.Run(harness.Context, harness.Commands, "editor.wait", { { "frames", 3 } }, [&done](const EditorCommandResult&) { done = true; },
			CommandOrigin { CommandSource::UI }));
		REQUIRE(harness.Entries("editor.wait").size() == 1);
		CHECK(harness.Entries("editor.wait")[0]->Status == CommandStatus::Pending);
		REQUIRE(harness.FramesUntil([&done]() { return done; }));
		REQUIRE(harness.Entries("editor.wait").size() == 1);
		const CommandJournalEntry& entry = *harness.Entries("editor.wait")[0];
		CHECK(entry.Status == CommandStatus::Succeeded);
		CHECK(entry.Source == CommandSource::UI);
		CHECK(entry.Frames >= 2);
		CHECK(entry.DurationMs > 0.0);

		// One cancelled while pending ends as Cancelled.
		REQUIRE(harness.Runner.Run(harness.Context, harness.Commands, "editor.wait", { { "frames", 100 } }, nullptr));
		harness.Runner.CancelAll("Cancelled by the test");
		CHECK(harness.Latest().Status == CommandStatus::Failed);
		CHECK(harness.Latest().ErrorKind == EditorCommandError::Cancelled);
		// Results polled outside the runner complete their entry too.
		EditorCommandResult result = harness.Commands.Execute(harness.Context, "editor.wait", { { "frames", 2 } });
		REQUIRE(result.IsPending());
		result = Tests::CompleteCommand(harness.Context, std::move(result));
		CHECK(harness.Latest().Status == CommandStatus::Succeeded);
	}

	TEST_CASE("Failures carry their kind and message")
	{
		JournalHarness harness;
		CHECK_FALSE(harness.Commands.Execute(harness.Context, "no.such").Success);
		CHECK(harness.Latest().Command == "no.such");
		CHECK(harness.Latest().ErrorKind == EditorCommandError::UnknownCommand);
		CHECK_FALSE(harness.Commands.Execute(harness.Context, "entity.get", { { "entity", 3 } }).Success);
		CHECK(harness.Latest().ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Latest().Error.find("entity") != std::string::npos);
		CHECK_FALSE(harness.Commands.Execute(harness.Context, "edit.undo").Success);
		CHECK(harness.Latest().ErrorKind == EditorCommandError::Failed);
		const nlohmann::json described = CommandJournal::Describe(harness.Latest());
		CHECK(described["status"] == "Failed");
		CHECK(described["error"]["kind"] == "Failed");
	}

	TEST_CASE("Entries name the undo step and the entities a command changed, and commands that run commands")
	{
		JournalHarness harness;
		const std::string a = Tests::RunCommandToCompletion(harness.Context, harness.Commands, "entity.create", { { "name", "A" } })["id"].get<std::string>();
		CHECK(harness.Latest().UndoStep.find("Create") != std::string::npos);
		REQUIRE(harness.Latest().Entities.size() == 1);
		CHECK(UUIDToJson(harness.Latest().Entities[0]) == a);
		CHECK(harness.Latest().Mutating);
		REQUIRE(harness.Latest().ResultIds.size() == 1);
		CHECK(harness.Latest().ResultIds[0].first == "/id");

		const std::string b = Tests::RunCommandToCompletion(harness.Context, harness.Commands, "entity.create", { { "name", "B" } })["id"].get<std::string>();
		Tests::RunCommandToCompletion(harness.Context, harness.Commands, "component.set",
			{ { "entities", { a, b } }, { "component", "Transform" }, { "values", { { "Translation", { 1, 2, 3 } } } } });
		CHECK(harness.Latest().EntityCount == 2);
		CHECK(harness.Latest().UndoSerial == harness.Context.GetUndoStack().GetLatestSerial());
		Tests::RunCommandToCompletion(harness.Context, harness.Commands, "scene.info");
		CHECK(harness.Latest().UndoStep.empty());
		CHECK_FALSE(harness.Latest().Mutating);

		Tests::RunCommandToCompletion(harness.Context, harness.Commands, "test.nested");
		const std::vector<const CommandJournalEntry*> nested = harness.Entries("test.nested");
		REQUIRE(nested.size() == 1);
		const CommandJournalEntry& inner = harness.Latest();
		CHECK(inner.Command == "entity.create");
		CHECK(inner.Parent == nested[0]->Id);
		CHECK(inner.Source == CommandSource::Internal);
	}

	TEST_CASE("The ring keeps at most 2,000 entries and 8 MB, and shortens huge parameters")
	{
		JournalHarness harness;
		for (size_t index = 0; index < CommandJournal::c_MaxEntries + 100; index++)
			harness.Commands.Execute(harness.Context, "selection.get");
		CHECK(harness.Journal().GetEntries().size() == CommandJournal::c_MaxEntries);
		CHECK(harness.Journal().GetEntries().front().Id == 101);
		CHECK(harness.Journal().GetLatestId() == CommandJournal::c_MaxEntries + 100);

		// Parameters below the limit are kept whole, for replays; the ring drops the oldest to stay within its bytes.
		const std::string large(200 * 1024, 'x');
		for (int index = 0; index < 60; index++)
			harness.Commands.Execute(harness.Context, "entity.find", { { "name", large } });
		CHECK(harness.Journal().GetBytes() <= CommandJournal::c_MaxBytes);
		CHECK(harness.Entries("entity.find").size() < 60);
		CHECK(harness.Latest().Parameters["name"] == large);
		CHECK_FALSE(harness.Latest().ParametersTruncated);

		const std::string huge(CommandJournal::c_MaxParameterBytes + 1024, 'y');
		harness.Commands.Execute(harness.Context, "entity.find", { { "name", huge } });
		CHECK(harness.Latest().ParametersTruncated);
		CHECK(harness.Latest().Parameters["name"].get<std::string>().size() < 2048);
	}

	TEST_CASE("editor.activity returns the entries after a point, filtered by source and command")
	{
		JournalHarness harness;
		Tests::RunCommandToCompletion(harness.Context, harness.Commands, "scene.info");
		const nlohmann::json start = Tests::RunCommandToCompletion(harness.Context, harness.Commands, "editor.activity");
		REQUIRE(start["entries"].size() == 1); // scene.info; not the call itself
		const uint64_t since = harness.Journal().GetLatestId();
		RunEditorCommand(harness.Context, harness.Commands, "entity.create", { { "name", "One" } });
		harness.Commands.Execute(harness.Context, "entity.create", { { "name", "Two" } });
		harness.Commands.Execute(harness.Context, "scene.info");

		nlohmann::json activity = Tests::RunCommandToCompletion(harness.Context, harness.Commands, "editor.activity", { { "since", since } });
		REQUIRE(activity["entries"].size() == 3); // Not the call itself, which has not finished when it answers
		CHECK(activity["entries"][0]["command"] == "entity.create");
		CHECK(activity["entries"][0]["source"] == "UI");
		CHECK(activity["entries"][0]["undoStep"].get<std::string>().find("Create") != std::string::npos);
		CHECK(activity["more"] == false);

		activity = Tests::RunCommandToCompletion(harness.Context, harness.Commands, "editor.activity", { { "since", since }, { "source", "UI" } });
		CHECK(activity["entries"].size() == 1);
		activity = Tests::RunCommandToCompletion(harness.Context, harness.Commands, "editor.activity", { { "since", since }, { "command", "entity." } });
		CHECK(activity["entries"].size() == 2);
		activity = Tests::RunCommandToCompletion(harness.Context, harness.Commands, "editor.activity", { { "since", since }, { "limit", 2 } });
		CHECK(activity["entries"].size() == 2);
		CHECK(activity["more"] == true);
		CHECK(activity["entries"][1]["id"] == activity["latest"]);

		const EditorCommandResult invalid = harness.Commands.Execute(harness.Context, "editor.activity", { { "source", "Robot" } });
		CHECK(invalid.ErrorKind == EditorCommandError::InvalidParameters);
	}
}
