#include <doctest/doctest.h>

#include "Editor/CommandUtils.h"
#include "Editor/EditorAutomation.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Network/NetworkTestHelpers.h"
#include "TestHelpers.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Network/EditorSession.h>
#include <Strata/Network/JsonRpc.h>
#include <Strata/Network/RpcClient.h>
#include <Strata/Reflection/PropertyJson.h>

#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Strata;

namespace
{

	constexpr std::chrono::milliseconds c_CallTimeout = std::chrono::milliseconds(10000);
	constexpr const char* c_ImageData = "iVBORw0KGgo=";

	// The editor's automation without the UI: commands, runner and server, with frames run by the test thread.
	struct AutomationHarness
	{
		EditorContext Context { EditorContextSpecification { false } };
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;
		// Declared last: stopped before the runner and the context go away, as in the editor.
		EditorAutomation Automation { Context, Commands, Runner };

		AutomationHarness()
		{
			// An image result, like viewport.capture: passed through as-is for MCP clients to show.
			Commands.Register({ "test.image", "Test command that returns an image.", CommandUtils::ObjectSchema({}),
				[](EditorContext&, const nlohmann::json&)
				{
					return EditorCommandResult::Ok({ { "Width", 1 }, { "Height", 1 }, { "Image", { { "MimeType", "image/png" }, { "Data", c_ImageData } } } });
				} });
			// Finishes after a number of frames with {"polls": n}.
			Commands.Register({ "test.defer", "Test command that finishes after a number of frames.",
				CommandUtils::ObjectSchema({ { "polls", CommandUtils::IntegerSchema("Frames", 1, 1000000) } }),
				[](EditorContext&, const nlohmann::json& parameters)
				{
					CommandArguments arguments(parameters);
					const int64_t polls = arguments.GetInt("polls", 1, 1, 1000000);
					if (!arguments.IsValid())
						return arguments.Fail();
					return EditorCommandResult::Defer([polls, count = int64_t(0)](EditorContext&) mutable -> std::optional<EditorCommandResult>
					{
						if (++count < polls)
							return std::nullopt;
						return EditorCommandResult::Ok({ { "polls", count } });
					});
				} });
			Commands.Register({ "test.throw", "Test command that throws, as third-party code may.", CommandUtils::ObjectSchema({}),
				[](EditorContext&, const nlohmann::json&) -> EditorCommandResult
				{
					throw std::runtime_error("broken handler");
				} });
		}

		~AutomationHarness()
		{
			Runner.CancelAll("The test is over");
			Automation.Stop();
		}

		AutomationHarness(const AutomationHarness&) = delete;
		AutomationHarness& operator=(const AutomationHarness&) = delete;

		bool Start(bool publishSession = false)
		{
			EditorAutomationSpecification specification;
			specification.AuthToken = Tests::c_TestServerToken;
			specification.PublishSession = publishSession;
			specification.ShutdownGracePeriod = std::chrono::milliseconds(2000);
			std::string error;
			const bool started = Automation.Start(specification, &error);
			INFO(error);
			CHECK(started);
			return started;
		}

		// One editor frame, in the editor's order.
		void Frame()
		{
			Context.Update(Timestep(1.0f / 60.0f));
			Runner.Update(Context);
			Automation.Update();
		}

		// Runs frames until the condition holds (or about ten seconds of frames have passed).
		bool RunFramesUntil(const std::function<bool()>& condition, size_t* frames = nullptr)
		{
			const auto deadline = std::chrono::steady_clock::now() + c_CallTimeout;
			size_t count = 0;
			while (!condition())
			{
				if (std::chrono::steady_clock::now() > deadline)
					return false;
				Frame();
				count++;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			if (frames)
				*frames = count;
			return true;
		}

		// Calls a method from another thread while this thread runs the editor's frames, like a tool talking to the editor.
		RpcResult Call(RpcClient& client, const std::string& method, const nlohmann::json& params = nlohmann::json::object(), size_t* frames = nullptr)
		{
			std::future<RpcResult> call = std::async(std::launch::async, [&client, method, params]() { return client.Call(method, params, c_CallTimeout); });
			RunFramesUntil([&call]() { return call.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }, frames);
			return call.get();
		}

		void Connect(RpcClient& client)
		{
			REQUIRE_MESSAGE(client.Connect("127.0.0.1", Automation.GetPort(), Tests::c_TestServerToken, std::chrono::milliseconds(5000)), client.GetLastError());
		}
	};

	const nlohmann::json* FindMethod(const nlohmann::json& listing, const std::string& name)
	{
		for (const nlohmann::json& method : listing["methods"])
		{
			if (method["name"] == name)
				return &method;
		}
		return nullptr;
	}

}

TEST_SUITE("Editor.Automation")
{
	TEST_CASE("Every command is a method with its description and parameter schema")
	{
		AutomationHarness harness;
		REQUIRE(harness.Start());
		RpcClient client;
		harness.Connect(client);

		const RpcResult listing = client.Call("rpc.listMethods", nlohmann::json::object(), c_CallTimeout);
		REQUIRE(listing.IsSuccess());
		for (const EditorCommand* command : harness.Commands.GetAll())
		{
			CAPTURE(command->Name);
			const nlohmann::json* method = FindMethod(listing.GetValue(), command->Name);
			REQUIRE(method != nullptr);
			CHECK((*method)["description"] == command->Description);
			CHECK((*method)["paramsSchema"] == command->Parameters);
		}
		CHECK(harness.Automation.DescribeStatus()["methods"] == harness.Commands.GetAll().size());

		// Commands registered later are offered from the next frame on; registering one again updates its description.
		harness.Commands.Register({ "test.late", "Registered after the server started.", CommandUtils::ObjectSchema({}),
			[](EditorContext&, const nlohmann::json&) { return EditorCommandResult::Ok("late"); } });
		harness.Frame();
		const RpcResult late = harness.Call(client, "test.late");
		REQUIRE(late.IsSuccess());
		CHECK(late.GetValue() == "late");

		harness.Commands.Register({ "test.late", "Registered again with another description.", CommandUtils::ObjectSchema({}),
			[](EditorContext&, const nlohmann::json&) { return EditorCommandResult::Ok("again"); } });
		harness.Frame();
		const RpcResult relisted = client.Call("rpc.listMethods", nlohmann::json::object(), c_CallTimeout);
		REQUIRE(relisted.IsSuccess());
		const nlohmann::json* updated = FindMethod(relisted.GetValue(), "test.late");
		REQUIRE(updated != nullptr);
		CHECK((*updated)["description"] == "Registered again with another description.");
		CHECK(harness.Call(client, "test.late").GetValue() == "again");
	}

	TEST_CASE("Requests run commands and answer with their results")
	{
		AutomationHarness harness;
		REQUIRE(harness.Start());
		RpcClient client;
		harness.Connect(client);

		const RpcResult created = harness.Call(client, "entity.create", { { "name", "Player" } });
		REQUIRE(created.IsSuccess());
		const std::optional<UUID> id = UUIDFromJson(created.GetValue()["id"]);
		REQUIRE(id.has_value());
		CHECK(harness.Context.GetEditScene()->GetEntityByUUID(*id).GetName() == "Player");
		CHECK(harness.Automation.GetCompletedRequestCount() == 1);

		// Images pass through untouched (MCP clients receive them as image content).
		const RpcResult image = harness.Call(client, "test.image");
		REQUIRE(image.IsSuccess());
		CHECK(image.GetValue()["Image"]["MimeType"] == "image/png");
		CHECK(image.GetValue()["Image"]["Data"] == c_ImageData);

		// editor.status carries the automation section.
		const RpcResult status = harness.Call(client, "editor.status");
		REQUIRE(status.IsSuccess());
		const nlohmann::json& automation = status.GetValue()["automation"];
		CHECK(automation["running"] == true);
		CHECK(automation["port"] == harness.Automation.GetPort());
		CHECK(automation["clients"] == 1);
		CHECK(automation["sessionPublished"] == false);
		CHECK(status.GetValue()["scene"]["entityCount"] == 1);
	}

	TEST_CASE("Asset streaming is observable and adjustable over automation")
	{
		AutomationHarness harness;
		REQUIRE(harness.Start());
		RpcClient client;
		harness.Connect(client);
		const Ref<AssetManagerBase>& manager = AssetManager::GetActive();
		REQUIRE(manager);

		const RpcResult stats = harness.Call(client, "asset.stats", { { "assets", true } });
		REQUIRE(stats.IsSuccess());
		CHECK(stats.GetValue()["registered"] == manager->GetStats().RegisteredAssets);
		CHECK(stats.GetValue()["pools"]["cpu"]["residentBytes"] == manager->GetStats().Resident.Cpu);
		CHECK(stats.GetValue()["assets"].size() == manager->GetResidencyInfo().size()); // The built-in assets

		const RpcResult budget = harness.Call(client, "asset.setBudget", { { "gpuTexturesMB", 128 } });
		REQUIRE(budget.IsSuccess());
		CHECK(manager->GetResidencyBudgets().GpuTextures == 128ull << 20);
		CHECK(budget.GetValue()["budgets"]["gpuTexturesBytes"] == 128ull << 20);
		const RpcResult rejected = harness.Call(client, "asset.setBudget", { { "gpuTexturesMB", -5 } });
		REQUIRE(rejected.IsError());
		CHECK(rejected.GetError().Code == JsonRpc::ErrorCode::InvalidParams);

		const RpcResult status = harness.Call(client, "editor.status");
		REQUIRE(status.IsSuccess());
		CHECK(status.GetValue()["assets"]["pools"]["gpuTextures"]["budgetBytes"] == 128ull << 20);
	}

	TEST_CASE("Failures become JSON-RPC errors by kind")
	{
		AutomationHarness harness;
		REQUIRE(harness.Start());
		RpcClient client;
		harness.Connect(client);

		const RpcResult unknownParameter = harness.Call(client, "entity.create", { { "nmae", "Typo" } });
		REQUIRE(unknownParameter.IsError());
		CHECK(unknownParameter.GetError().Code == JsonRpc::ErrorCode::InvalidParams);
		CHECK(unknownParameter.GetError().Message.find("Unknown parameter 'nmae'") != std::string::npos);
		// The schema comes along, so a client can correct its request.
		CHECK(unknownParameter.GetError().Data["command"] == "entity.create");
		CHECK(unknownParameter.GetError().Data["parameters"] == harness.Commands.Find("entity.create")->Parameters);

		const RpcResult positional = harness.Call(client, "entity.create", nlohmann::json::array({ "Player" }));
		REQUIRE(positional.IsError());
		CHECK(positional.GetError().Code == JsonRpc::ErrorCode::InvalidParams);

		const RpcResult missingEntity = harness.Call(client, "entity.get", { { "entity", "00000000DEADBEEF" } });
		CHECK(missingEntity.GetError().Code == JsonRpc::ErrorCode::InvalidParams);

		const RpcResult nothingToUndo = harness.Call(client, "edit.undo");
		REQUIRE(nothingToUndo.IsError());
		CHECK(nothingToUndo.GetError().Code == JsonRpc::ErrorCode::OperationFailed);
		CHECK(nothingToUndo.GetError().Message == "Nothing to undo");

		const RpcResult broken = harness.Call(client, "test.throw");
		CHECK(broken.GetError().Code == JsonRpc::ErrorCode::InternalError);

		const RpcResult unknownMethod = harness.Call(client, "no.such.command");
		CHECK(unknownMethod.GetError().Code == JsonRpc::ErrorCode::MethodNotFound);

		// The mapping itself, including the kinds the editor produces only in special situations.
		CHECK(EditorAutomation::ToRpcResult(EditorCommandResult::Fail("Gone", EditorCommandError::UnknownCommand), "x.y", {}).GetError().Code
			== JsonRpc::ErrorCode::MethodNotFound);
		const RpcResult cancelled = EditorAutomation::ToRpcResult(EditorCommandResult::Fail("Closing", EditorCommandError::Cancelled), "x.y", {});
		CHECK(cancelled.GetError().Code == JsonRpc::ErrorCode::Cancelled);
		CHECK(cancelled.GetError().Message == "Closing");
		CHECK(EditorAutomation::ToRpcResult(EditorCommandResult::Ok(nullptr), "x.y", {}).IsSuccess());
	}

	TEST_CASE("Deferred commands answer when they complete")
	{
		AutomationHarness harness;
		REQUIRE(harness.Start());
		RpcClient client;
		harness.Connect(client);

		size_t frames = 0;
		const RpcResult waited = harness.Call(client, "editor.wait", { { "frames", 5 } }, &frames);
		REQUIRE(waited.IsSuccess());
		CHECK(waited.GetValue()["frames"] == 5);
		CHECK(frames >= 5);

		// While it runs, the request counts as pending.
		std::future<RpcResult> call = std::async(std::launch::async, [&client]() { return client.Call("test.defer", { { "polls", 20 } }, c_CallTimeout); });
		REQUIRE(harness.RunFramesUntil([&]() { return harness.Automation.GetPendingRequestCount() == 1; }));
		CHECK(harness.Automation.DescribeStatus()["pendingCommands"] == 1);
		REQUIRE(harness.RunFramesUntil([&]() { return call.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready; }));
		const RpcResult deferred = call.get();
		REQUIRE(deferred.IsSuccess());
		CHECK(deferred.GetValue()["polls"] == 20);
		CHECK(harness.Automation.GetPendingRequestCount() == 0);
	}

	TEST_CASE("A client that disconnects while its command is pending loses only the answer")
	{
		AutomationHarness harness;
		REQUIRE(harness.Start());

		{
			Tests::RawRpcConnection connection;
			REQUIRE(connection.Connect(harness.Automation.GetPort()));
			REQUIRE(connection.Authenticate());
			REQUIRE(connection.SendLine(JsonRpc::Serialize(JsonRpc::MakeRequest(1, "test.defer", { { "polls", 10 } }))));
			REQUIRE(harness.RunFramesUntil([&]() { return harness.Automation.GetPendingRequestCount() == 1; }));
		}

		// The command still finishes; its answer has nowhere to go and is dropped.
		REQUIRE(harness.RunFramesUntil([&]() { return harness.Automation.GetPendingRequestCount() == 0; }));
		CHECK(harness.Runner.GetPendingCount() == 0);
		REQUIRE(harness.RunFramesUntil([&]() { return harness.Automation.GetClientCount() == 0; }));

		RpcClient client;
		harness.Connect(client);
		CHECK(harness.Call(client, "entity.create", { { "name", "After" } }).IsSuccess());
	}

	TEST_CASE("Pending requests are answered when the editor closes")
	{
		AutomationHarness harness;
		REQUIRE(harness.Start());
		RpcClient client;
		harness.Connect(client);

		// The client disconnects once it has its answer, so the server need not wait for it to go away.
		std::future<RpcResult> call = std::async(std::launch::async, [&client]()
		{
			RpcResult result = client.Call("editor.wait", { { "frames", 100000 } }, c_CallTimeout);
			client.Close();
			return result;
		});
		REQUIRE(harness.RunFramesUntil([&]() { return harness.Automation.GetPendingRequestCount() == 1; }));

		// The editor's shutdown order: cancel what is pending, then stop serving (which still delivers the answers).
		harness.Runner.CancelAll("The editor is closing");
		harness.Automation.Stop();
		const RpcResult result = call.get();
		REQUIRE(result.IsError());
		CHECK(result.GetError().Code == JsonRpc::ErrorCode::Cancelled);
		CHECK(result.GetError().Message == "The editor is closing");
		CHECK_FALSE(harness.Automation.IsRunning());
		CHECK_FALSE(harness.Context.GetStatusProviders().contains("automation"));
	}

	TEST_CASE("editor.quit is answered before the editor closes")
	{
		AutomationHarness harness;
		REQUIRE(harness.Start());
		RpcClient client;
		harness.Connect(client);

		harness.Call(client, "entity.create", { { "name", "Unsaved" } });
		const RpcResult refused = harness.Call(client, "editor.quit");
		REQUIRE(refused.IsError());
		CHECK(refused.GetError().Code == JsonRpc::ErrorCode::OperationFailed);
		CHECK_FALSE(harness.Context.IsQuitRequested());

		const RpcResult quit = harness.Call(client, "editor.quit", { { "force", true } });
		REQUIRE(quit.IsSuccess());
		CHECK(quit.GetValue()["quitting"] == true);
		CHECK(harness.Context.IsQuitRequested());
	}

	TEST_CASE("The session is published, follows the project and is removed")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("AutomationSessions") / "Sessions";
		const std::filesystem::path projectDirectory = Tests::CreateTemporaryDirectory("AutomationProject") / "Game";
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory));

		AutomationHarness harness;
		REQUIRE(harness.Start(true));
		std::vector<EditorSessionInfo> sessions = EditorSession::FindSessions(sessionDirectory);
		REQUIRE(sessions.size() == 1);
		CHECK(sessions[0].ProcessId == Platform::GetProcessID());
		CHECK(sessions[0].Port == harness.Automation.GetPort());
		CHECK(sessions[0].ProjectPath.empty());
		CHECK(sessions[0].Token == Tests::c_TestServerToken);
		CHECK(harness.Automation.DescribeStatus()["sessionPublished"] == true);

		// A client finds the editor through the session file alone. A project that a command creates is published before
		// the answer goes out: no frame runs between the command and the checks below, so a client that calls again
		// with --project right after the answer finds the editor.
		Tests::RawRpcConnection connection;
		REQUIRE(connection.Connect(sessions[0].Port));
		REQUIRE(connection.Authenticate(sessions[0].Token));
		REQUIRE(connection.SendLine(JsonRpc::Serialize(JsonRpc::MakeRequest(1, "project.create",
			{ { "directory", FileSystem::ToUTF8(projectDirectory) }, { "name", "Game" } }))));
		REQUIRE(harness.RunFramesUntil([&]() { return harness.Automation.GetCompletedRequestCount() == 1; }));
		const std::optional<nlohmann::json> created = connection.ReadMessage();
		REQUIRE(created.has_value());
		CHECK(created->contains("result"));
		const std::optional<EditorSessionInfo> projectSession = EditorSession::ReadProjectSession(projectDirectory, sessionDirectory);
		REQUIRE(projectSession.has_value());
		CHECK(projectSession->Port == harness.Automation.GetPort());
		sessions = EditorSession::FindSessions(sessionDirectory);
		REQUIRE(sessions.size() == 1);
		CHECK(EditorSession::IsSameProject(sessions[0].ProjectPath, projectDirectory));

		connection.GetSocket().Close();
		harness.Automation.Stop();
		CHECK(EditorSession::FindSessions(sessionDirectory).empty());
		CHECK_FALSE(FileSystem::Exists(EditorSession::GetProjectSessionFilePath(projectDirectory)));
		CHECK_FALSE(harness.Automation.GetSession().has_value());
	}

	TEST_CASE("The idle timeout runs while no client is connected and nothing is pending")
	{
		AutomationHarness harness;
		EditorAutomationSpecification specification;
		specification.AuthToken = Tests::c_TestServerToken;
		specification.PublishSession = false;
		specification.IdleTimeout = std::chrono::milliseconds(300);
		REQUIRE(harness.Automation.Start(specification));
		CHECK_FALSE(harness.Automation.HasIdledOut());

		// A connected client keeps the editor alive, however long it stays quiet.
		{
			RpcClient client;
			harness.Connect(client);
			REQUIRE(harness.RunFramesUntil([&]() { return harness.Automation.GetClientCount() == 1; }));
			const auto connectedUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
			while (std::chrono::steady_clock::now() < connectedUntil)
			{
				harness.Frame();
				CHECK_FALSE(harness.Automation.HasIdledOut());
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}

		// Once it is gone, the timeout runs from its last frame.
		REQUIRE(harness.RunFramesUntil([&]() { return harness.Automation.GetClientCount() == 0; }));
		CHECK_FALSE(harness.Automation.HasIdledOut());
		CHECK(harness.RunFramesUntil([&]() { return harness.Automation.HasIdledOut(); }));

		// Without a timeout, never.
		harness.Automation.Stop();
		CHECK_FALSE(harness.Automation.HasIdledOut());
		specification.IdleTimeout = std::chrono::milliseconds(0);
		REQUIRE(harness.Automation.Start(specification));
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		harness.Frame();
		CHECK_FALSE(harness.Automation.HasIdledOut());
	}

	TEST_CASE("Start reports why the editor cannot be served")
	{
		AutomationHarness harness;
		EditorAutomationSpecification specification;
		specification.PublishSession = false;
		specification.BindAddress = "0.0.0.0";
		std::string error;
		CHECK_FALSE(harness.Automation.Start(specification, &error));
		CHECK(error.find("loopback") != std::string::npos);
		CHECK_FALSE(harness.Automation.IsRunning());

		// A port that is taken.
		RpcServer other;
		REQUIRE(other.Start(Tests::MakeTestServerSpecification()));
		specification.BindAddress = "127.0.0.1";
		specification.Port = other.GetPort();
		CHECK_FALSE(harness.Automation.Start(specification, &error));
		CHECK(error.find(std::to_string(other.GetPort())) != std::string::npos);

		// It still starts afterwards, with a fresh token when none is given.
		specification.Port = 0;
		REQUIRE(harness.Automation.Start(specification, &error));
		CHECK(harness.Automation.GetPort() != 0);
		RpcClient wrongToken;
		CHECK_FALSE(wrongToken.Connect("127.0.0.1", harness.Automation.GetPort(), Tests::c_TestServerToken, std::chrono::milliseconds(2000)));
	}

}
