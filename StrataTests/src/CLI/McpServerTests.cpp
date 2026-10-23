#if defined(STRATA_TESTS_HAVE_CLI)

#include <doctest/doctest.h>

#include "CLI/FakeEditor.h"
#include "CLI/McpServer.h"
#include "CLI/McpTestClient.h"
#include "CLI/McpToolsets.h"
#include "Strata/Core/Version.h"
#include "Strata/Network/EditorSession.h"
#include "Strata/Network/JsonRpc.h"
#include "Strata/Network/RpcClient.h"
#include "TestHelpers.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::CLI;

namespace
{
	// The fake editor's methods in the core toolset (entity.create, viewport.capture); scene.fail, math.add and list.sum
	// are in others.
	constexpr size_t c_FakeCoreTools = 2;

	std::string GetText(const nlohmann::json& toolResult, size_t index = 0)
	{
		return toolResult.at("content").at(index).at("text").get<std::string>();
	}
}

TEST_SUITE("CLI.Mcp")
{
	TEST_CASE("Initialize negotiates the protocol version")
	{
		Tests::McpTestClient client(Tests::MakeMcpSpecification(Tests::CreateTemporaryDirectory("McpInitialize")));

		for (const char* version : { "2024-11-05", "2025-03-26", "2025-06-18" })
		{
			nlohmann::json response = client.Request("initialize", nlohmann::json { { "protocolVersion", version } });
			CHECK(response["result"]["protocolVersion"] == version);
		}

		nlohmann::json unknown = client.Request("initialize", nlohmann::json { { "protocolVersion", "1999-01-01" } });
		CHECK(unknown["result"]["protocolVersion"] == "2025-06-18");
		nlohmann::json missing = client.Request("initialize", nlohmann::json::object());
		CHECK(missing["result"]["protocolVersion"] == "2025-06-18");
		CHECK(client.GetServer().GetProtocolVersion() == "2025-06-18");

		nlohmann::json& result = missing["result"];
		CHECK(result["serverInfo"]["name"] == "strata");
		CHECK(result["serverInfo"]["version"] == c_EngineVersion);
		CHECK(result["capabilities"]["tools"]["listChanged"] == true);
		REQUIRE(result["instructions"].is_string());
		// Saves answer later: agents must not expect the file at once, nor batch them.
		const std::string instructions = result["instructions"].get<std::string>();
		CHECK(instructions.find("scene_save and scene_saveAs write in the background and answer once the file is written") != std::string::npos);
		CHECK(instructions.find("editor_batch refuses saves") != std::string::npos);

		CHECK_FALSE(client.GetServer().IsInitialized());
		const size_t outputBefore = client.GetOutput().size();
		client.Notify("notifications/initialized");
		CHECK(client.GetServer().IsInitialized());
		CHECK(client.GetOutput().size() == outputBefore); // Notifications are never answered
	}

	TEST_CASE("Protocol-level errors and utilities")
	{
		Tests::McpTestClient client(Tests::MakeMcpSpecification(Tests::CreateTemporaryDirectory("McpProtocol")));

		CHECK(client.Request("ping")["result"] == nlohmann::json::object());

		nlohmann::json unknown = client.Request("resources/list");
		CHECK(unknown["error"]["code"] == JsonRpc::ErrorCode::MethodNotFound);

		nlohmann::json badCall = client.Request("tools/call", nlohmann::json::array({ 1 }));
		CHECK(badCall["error"]["code"] == JsonRpc::ErrorCode::InvalidParams);

		client.SendLine("{definitely not json");
		std::optional<nlohmann::json> parseError = JsonRpc::Parse(client.GetOutput().back());
		REQUIRE(parseError.has_value());
		CHECK((*parseError)["error"]["code"] == JsonRpc::ErrorCode::ParseError);
		CHECK((*parseError)["id"].is_null());

		client.SendLine(R"({"jsonrpc":"2.0","id":5})");
		CHECK(JsonRpc::Parse(client.GetOutput().back()).value()["error"]["code"] == JsonRpc::ErrorCode::InvalidRequest);

		// Blank lines, responses and unknown notifications produce no output.
		const size_t outputBefore = client.GetOutput().size();
		client.SendLine("   \r");
		client.SendLine(R"({"jsonrpc":"2.0","id":1,"result":{}})");
		client.Notify("notifications/cancelled");
		CHECK(client.GetOutput().size() == outputBefore);

		// Batches (MCP 2025-03-26) are answered with an array of responses.
		client.SendLine(R"([{"jsonrpc":"2.0","id":"a","method":"ping"},{"jsonrpc":"2.0","method":"notifications/initialized"},{"jsonrpc":"2.0","id":"b","method":"ping"}])");
		nlohmann::json batch = JsonRpc::Parse(client.GetOutput().back()).value();
		REQUIRE(batch.is_array());
		REQUIRE(batch.size() == 2);
		CHECK(batch[0]["id"] == "a");
		CHECK(batch[1]["id"] == "b");

		client.SendLine("[]");
		CHECK(JsonRpc::Parse(client.GetOutput().back()).value()["error"]["code"] == JsonRpc::ErrorCode::InvalidRequest);

		// Every output line is a single compact JSON message.
		for (const std::string& line : client.GetOutput())
		{
			CHECK(line.find('\n') == std::string::npos);
			CHECK(JsonRpc::Parse(line).has_value());
		}
	}

	TEST_CASE("Only the generic tools are listed without an editor")
	{
		Tests::McpTestClient client(Tests::MakeMcpSpecification(Tests::CreateTemporaryDirectory("McpNoEditor")));
		client.Initialize();

		nlohmann::json response = client.Request("tools/list");
		nlohmann::json& tools = response["result"]["tools"];
		REQUIRE(tools.size() == Tests::c_GenericMcpTools.size());
		for (size_t index = 0; index < tools.size(); index++)
		{
			CHECK(tools[index]["name"] == Tests::c_GenericMcpTools[index]);
			CHECK_FALSE(tools[index]["description"].get<std::string>().empty());
			CHECK(tools[index]["inputSchema"]["type"] == "object");
		}

		nlohmann::json status = client.CallTool("strata_status");
		CHECK(status["isError"] == false);
		CHECK(status["structuredContent"]["connected"] == false);
		CHECK(status["structuredContent"]["knownSessions"].empty());

		nlohmann::json editorTool = client.CallTool("entity_create", nlohmann::json { { "Name", "x" } });
		CHECK(editorTool["isError"] == true);
		CHECK(GetText(editorTool).find("no Strata editor is connected") != std::string::npos);

		nlohmann::json call = client.CallTool("strata_call", nlohmann::json { { "method", "entity.create" } });
		CHECK(call["isError"] == true);
		CHECK(client.CallTool("strata_list_methods")["isError"] == true);
		CHECK(GetText(client.CallTool("strata_describe")).find("No Strata editor is connected") != std::string::npos);
		// Toolsets can be chosen before an editor connects.
		const nlohmann::json toolsets = client.CallTool("strata_toolsets", nlohmann::json { { "enable", { "scene" } } });
		CHECK(toolsets["isError"] == false);
		CHECK(toolsets["structuredContent"]["enabled"] == nlohmann::json::array({ "core", "scene" }));
		CHECK(toolsets["structuredContent"]["editorTools"] == 0);
	}

	TEST_CASE("Editor methods are exposed as tools")
	{
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));
		Tests::McpTestClient client(Tests::MakeMcpSpecification(Tests::CreateTemporaryDirectory("McpTools"), editor.GetPort()));
		client.Initialize();

		nlohmann::json response = client.Request("tools/list");
		nlohmann::json& tools = response["result"]["tools"];
		std::vector<std::string> names;
		for (const nlohmann::json& tool : tools)
		{
			names.push_back(tool["name"].get<std::string>());
			CHECK(tool["inputSchema"]["type"] == "object");
			CHECK(tool["inputSchema"]["properties"].is_object());
		}

		for (const std::string& generic : Tests::c_GenericMcpTools)
			CHECK(std::find(names.begin(), names.end(), generic) != names.end());
		// The core toolset by default.
		for (const char* expected : { "entity_create", "viewport_capture" })
			CHECK(std::find(names.begin(), names.end(), expected) != names.end());
		for (const std::string& name : names)
			CHECK(name.rfind("rpc_", 0) == std::string::npos);
		CHECK(names.size() == Tests::c_GenericMcpTools.size() + c_FakeCoreTools);

		const auto create = std::find_if(tools.begin(), tools.end(), [](const nlohmann::json& tool) { return tool.value("name", "") == "entity_create"; });
		REQUIRE(create != tools.end());
		CHECK((*create)["description"] == "Creates an entity");
		CHECK((*create)["inputSchema"]["required"] == nlohmann::json::array({ "Name" }));
		CHECK((*create)["inputSchema"]["properties"]["Name"]["type"] == "string");

		// Joining after the initial listing, the client was never told about a change.
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 0);

		// Every method once every toolset is enabled; the change is announced.
		const nlohmann::json enabled = client.CallTool("strata_toolsets", nlohmann::json { { "enable", { "all" } } });
		CHECK(enabled["structuredContent"]["editorTools"] == 5);
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 1);
		names = client.ListToolNames();
		for (const char* expected : { "entity_create", "scene_fail", "viewport_capture", "math_add", "list_sum" })
			CHECK(std::find(names.begin(), names.end(), expected) != names.end());
		CHECK(names.size() == Tests::c_GenericMcpTools.size() + 5);
		client.CallTool("strata_toolsets", nlohmann::json { { "disable", { "all" } }, { "enable", { "math" } } });
		CHECK(client.ListToolNames().size() == Tests::c_GenericMcpTools.size() + 1);
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 2);
		CHECK(client.CallTool("strata_toolsets", nlohmann::json { { "enable", "scene" } })["isError"] == true);
	}

	TEST_CASE("Tool calls are routed to the editor and results are mapped")
	{
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));
		Tests::McpTestClient client(Tests::MakeMcpSpecification(Tests::CreateTemporaryDirectory("McpCalls"), editor.GetPort()));
		client.Initialize();
		client.ListToolNames();

		SUBCASE("Object results become text and structured content")
		{
			nlohmann::json result = client.CallTool("entity_create", nlohmann::json { { "Name", "Player" } });
			CHECK(result["isError"] == false);
			REQUIRE(result["content"].size() == 1);
			CHECK(result["content"][0]["type"] == "text");
			nlohmann::json text = JsonRpc::Parse(GetText(result)).value();
			CHECK(text["Entity"] == 42);
			CHECK(text["Name"] == "Player");
			CHECK(result["structuredContent"]["Entity"] == 42);
		}

		SUBCASE("Errors become isError results")
		{
			nlohmann::json result = client.CallTool("scene_fail");
			CHECK(result["isError"] == true);
			const std::string text = GetText(result);
			CHECK(text.find("Scene 'x' not found") != std::string::npos);
			CHECK(text.find("-32602") != std::string::npos);
			CHECK(text.find("\"Scene\"") != std::string::npos); // Error data is included

			nlohmann::json invalid = client.CallTool("entity_create");
			CHECK(invalid["isError"] == true);
			CHECK(GetText(invalid).find("Missing 'Name'") != std::string::npos);
		}

		SUBCASE("Images become image content")
		{
			nlohmann::json result = client.CallTool("viewport_capture");
			CHECK(result["isError"] == false);
			REQUIRE(result["content"].size() == 2);
			CHECK(result["content"][0]["type"] == "text");
			CHECK(result["content"][1]["type"] == "image");
			CHECK(result["content"][1]["mimeType"] == "image/png");
			CHECK(result["content"][1]["data"] == "iVBORw0KGgo=");

			nlohmann::json text = JsonRpc::Parse(GetText(result)).value();
			CHECK(text["Width"] == 2);
			CHECK_FALSE(text.contains("Image"));
			CHECK_FALSE(result["structuredContent"].contains("Image"));
			CHECK(result["structuredContent"]["Height"] == 1);
		}

		SUBCASE("Non-object results are text only")
		{
			nlohmann::json result = client.CallTool("math_add", nlohmann::json { { "a", 2 }, { "b", 3 } });
			CHECK(result["isError"] == false);
			CHECK(JsonRpc::Parse(GetText(result)).value() == 5.0);
			CHECK_FALSE(result.contains("structuredContent"));
		}

		SUBCASE("Generic tools reach the editor")
		{
			nlohmann::json call = client.CallTool("strata_call", nlohmann::json { { "method", "entity.create" }, { "params", { { "Name", "Generic" } } } });
			CHECK(call["isError"] == false);
			CHECK(call["structuredContent"]["Name"] == "Generic");

			nlohmann::json methods = client.CallTool("strata_list_methods");
			CHECK(methods["isError"] == false);
			CHECK(methods["structuredContent"]["methods"].size() == 9); // 4 built-ins + 5 editor methods

			nlohmann::json status = client.CallTool("strata_status");
			CHECK(status["structuredContent"]["connected"] == true);
			CHECK(status["structuredContent"]["endpoint"]["port"] == editor.GetPort());
			CHECK(status["structuredContent"]["endpoint"]["source"] == "explicit");
			CHECK(status["structuredContent"]["editorTools"] == c_FakeCoreTools);
			CHECK(status["structuredContent"]["editorCommands"] == 5);
			CHECK(status["structuredContent"]["toolsets"] == nlohmann::json::array({ "core" }));

			// Methods outside the enabled toolsets are still called by their tool names and through strata_call.
			CHECK(client.CallTool("strata_call", nlohmann::json { { "method", "math.add" }, { "params", { { "a", 1 }, { "b", 2 } } } })["isError"] == false);
		}

		SUBCASE("Tool name and argument errors are isError results")
		{
			CHECK(client.CallTool("no_such_tool")["isError"] == true);
			CHECK(GetText(client.CallTool("no_such_tool")).find("Unknown tool") != std::string::npos);

			nlohmann::json badArguments = client.Request("tools/call", nlohmann::json { { "name", "entity_create" }, { "arguments", "text" } });
			CHECK(badArguments["result"]["isError"] == true);
			nlohmann::json missingName = client.Request("tools/call", nlohmann::json { { "arguments", nlohmann::json::object() } });
			CHECK(missingName["result"]["isError"] == true);

			CHECK(client.CallTool("strata_call")["isError"] == true);
			CHECK(client.CallTool("strata_call", nlohmann::json { { "method", 5 } })["isError"] == true);
			CHECK(client.CallTool("strata_call", nlohmann::json { { "method", "entity.create" }, { "params", "x" } })["isError"] == true);
		}
	}

	TEST_CASE("Tool list changes are announced when an editor connects and disconnects")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("McpListChanged");
		Tests::McpTestClient client(Tests::MakeMcpSpecification(sessionDirectory));
		client.Initialize();
		CHECK(client.ListToolNames() == Tests::c_GenericMcpTools);

		client.GetServer().Tick();
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 0);

		Tests::LiveProcess owner;
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(owner.GetProcessId(), editor.GetPort(), EditorSession::GetCurrentTimestamp())));

		client.GetServer().Tick();
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 1);
		CHECK(client.ListToolNames().size() == Tests::c_GenericMcpTools.size() + c_FakeCoreTools);

		client.GetServer().Tick();
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 1); // Unchanged: no repeat

		editor.Stop();
		client.GetServer().Tick();
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 2);
		CHECK(client.ListToolNames() == Tests::c_GenericMcpTools);
	}

	TEST_CASE("Launching validates its arguments and the editor path")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("McpLaunch");
		McpServerSpecification specification = Tests::MakeMcpSpecification(directory);
		specification.EditorPath = FileSystem::ToUTF8(directory / "NoSuchEditor.exe");
		Tests::McpTestClient client(specification);
		client.Initialize();

		nlohmann::json badProject = client.CallTool("strata_launch_editor", nlohmann::json { { "project", 42 } });
		CHECK(badProject["isError"] == true);
		CHECK(GetText(badProject).find("project") != std::string::npos);

		nlohmann::json badHeadless = client.CallTool("strata_launch_editor", nlohmann::json { { "project", FileSystem::ToUTF8(directory) }, { "headless", "yes" } });
		CHECK(badHeadless["isError"] == true);
		nlohmann::json badNoGpu = client.CallTool("strata_launch_editor", nlohmann::json { { "noGpu", 1 } });
		CHECK(badNoGpu["isError"] == true);
		CHECK(GetText(badNoGpu).find("noGpu") != std::string::npos);

		// Without a project, an editor is started without one (here the editor executable is missing).
		nlohmann::json withoutProject = client.CallTool("strata_launch_editor");
		CHECK(withoutProject["isError"] == true);
		CHECK(GetText(withoutProject).find("not found") != std::string::npos);

		nlohmann::json missingEditor = client.CallTool("strata_launch_editor", nlohmann::json { { "project", FileSystem::ToUTF8(directory) }, { "headless", true } });
		CHECK(missingEditor["isError"] == true);
		CHECK(GetText(missingEditor).find("not found") != std::string::npos);
	}

	TEST_CASE("Launching reuses an editor that already has the project open")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("McpLaunchReuse") / "Sessions";
		const std::filesystem::path projectDirectory = Tests::CreateTemporaryDirectory("McpLaunchReuseProject");
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory));
		Tests::LiveProcess owner;
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));
		// Published like the real editor: the per-user session and the project pointer.
		REQUIRE(EditorSession::WriteSessionFiles(Tests::MakeFakeSession(owner.GetProcessId(), editor.GetPort(), EditorSession::GetCurrentTimestamp(), FileSystem::ToUTF8(projectDirectory))));

		McpServerSpecification specification = Tests::MakeMcpSpecification(sessionDirectory);
		specification.EditorPath = FileSystem::ToUTF8(sessionDirectory / "NoSuchEditor.exe");
		Tests::McpTestClient client(specification);
		client.Initialize();

		nlohmann::json result = client.CallTool("strata_launch_editor", nlohmann::json { { "project", FileSystem::ToUTF8(projectDirectory) } });
		CHECK(result["isError"] == false);
		CHECK(result["structuredContent"]["launched"] == false);
		CHECK(result["structuredContent"]["session"]["ProcessId"] == owner.GetProcessId());
		CHECK_FALSE(result["structuredContent"]["session"].contains("Token"));
		CHECK(client.CallTool("entity_create", nlohmann::json { { "Name", "Reused" } })["isError"] == false);
	}

	TEST_CASE("Launching starts an editor and exposes its methods as tools")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("McpLaunch") / "Sessions";
		const std::filesystem::path projectDirectory = Tests::CreateTemporaryDirectory("McpLaunchProject");
		Tests::ScopedEnvironmentVariable fakeEditor("STRATA_TEST_FAKE_EDITOR", "1");
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory));

		McpServerSpecification specification = Tests::MakeMcpSpecification(sessionDirectory);
		specification.EditorPath = FileSystem::ToUTF8(Tests::GetTestExecutablePath());
		specification.LaunchTimeout = std::chrono::milliseconds(20000);
		// The fake editor's methods (editor.info, editor.quit) are in the editor toolset.
		specification.Toolsets = { "core", "editor" };
		Tests::McpTestClient client(specification);
		client.Initialize();
		CHECK(client.ListToolNames() == Tests::c_GenericMcpTools);

		nlohmann::json launched = client.CallTool("strata_launch_editor", nlohmann::json { { "project", FileSystem::ToUTF8(projectDirectory) }, { "headless", true } });
		REQUIRE_MESSAGE(launched["isError"] == false, GetText(launched));
		CHECK(launched["structuredContent"]["launched"] == true);
		CHECK(launched["structuredContent"]["session"]["Headless"] == true);
		CHECK_FALSE(launched["structuredContent"]["session"].contains("Token"));
		CHECK(launched["structuredContent"]["editorTools"] == 3); // editor.info, editor.status, editor.quit
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 1);

		const std::vector<std::string> tools = client.ListToolNames();
		CHECK(std::find(tools.begin(), tools.end(), "editor_info") != tools.end());
		nlohmann::json info = client.CallTool("editor_info");
		CHECK(info["isError"] == false);
		CHECK(EditorSession::IsSameProject(info["structuredContent"]["Project"].get<std::string>(), projectDirectory));

		CHECK(client.CallTool("editor_quit")["isError"] == false);
		// The editor exits and removes its session; the server notices on its next tick and drops the tools.
		CHECK(Tests::WaitUntil([&]()
		{
			client.GetServer().Tick();
			return client.CountNotifications("notifications/tools/list_changed") == 2;
		}, std::chrono::milliseconds(10000)));
		CHECK(client.ListToolNames() == Tests::c_GenericMcpTools);
	}

	TEST_CASE("Launching answers once the project's assets are scanned, with progress notifications when asked for")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("McpLaunchScan") / "Sessions";
		const std::filesystem::path projectDirectory = Tests::CreateTemporaryDirectory("McpLaunchScanProject");
		Tests::ScopedEnvironmentVariable fakeEditor("STRATA_TEST_FAKE_EDITOR", "1");
		Tests::ScopedEnvironmentVariable fakeScan("STRATA_TEST_FAKE_SCAN", "2500");
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory));

		McpServerSpecification specification = Tests::MakeMcpSpecification(sessionDirectory);
		specification.EditorPath = FileSystem::ToUTF8(Tests::GetTestExecutablePath());
		specification.LaunchTimeout = std::chrono::milliseconds(20000);
		// The fake editor's editor.quit is in the editor toolset.
		specification.Toolsets = { "core", "editor" };
		Tests::McpTestClient client(specification);
		client.Initialize();
		nlohmann::json launched = client.CallTool("strata_launch_editor", nlohmann::json { { "project", FileSystem::ToUTF8(projectDirectory) }, { "noGpu", true } },
			"launch-1");
		REQUIRE_MESSAGE(launched["isError"] == false, GetText(launched));
		// The scan was reported as it went: one token, progress that only grows, below 100 (the scan is the first part).
		double previous = -1.0;
		size_t notifications = 0;
		for (const std::string& line : client.GetOutput())
		{
			const std::optional<nlohmann::json> message = JsonRpc::Parse(line);
			if (!message || message->value("method", "") != "notifications/progress")
				continue;
			notifications++;
			const nlohmann::json& params = (*message)["params"];
			CHECK(params["progressToken"] == "launch-1");
			CHECK(params["total"] == 100);
			CHECK(params["progress"].get<double>() > previous);
			CHECK(params["message"].get<std::string>().find("Scanning the project's assets") != std::string::npos);
			previous = params["progress"].get<double>();
		}
		CHECK(notifications >= 2);
		CHECK(client.CallTool("editor_quit")["isError"] == false);
	}

	TEST_CASE("Launching without a project starts an editor without one")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("McpLaunchEmpty") / "Sessions";
		Tests::ScopedEnvironmentVariable fakeEditor("STRATA_TEST_FAKE_EDITOR", "1");
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory));

		McpServerSpecification specification = Tests::MakeMcpSpecification(sessionDirectory);
		specification.EditorPath = FileSystem::ToUTF8(Tests::GetTestExecutablePath());
		specification.LaunchTimeout = std::chrono::milliseconds(20000);
		Tests::McpTestClient client(specification);
		client.Initialize();

		nlohmann::json launched = client.CallTool("strata_launch_editor", nlohmann::json { { "noGpu", true } });
		REQUIRE_MESSAGE(launched["isError"] == false, GetText(launched));
		CHECK(launched["structuredContent"]["launched"] == true);
		CHECK(launched["structuredContent"]["session"]["ProjectPath"] == "");
		const uint32_t firstProcess = launched["structuredContent"]["session"]["ProcessId"].get<uint32_t>();
		nlohmann::json info = client.CallTool("editor_info");
		REQUIRE(info["isError"] == false);
		CHECK(info["structuredContent"]["NoGpu"] == true);
		CHECK(info["structuredContent"]["Project"] == "");
		// Editors the server starts close themselves once it is gone (default: ten minutes without a client).
		CHECK(info["structuredContent"]["IdleTimeout"] == "600");

		// Asking again reuses that editor instead of leaving it running unused; other options start another one.
		nlohmann::json again = client.CallTool("strata_launch_editor", nlohmann::json { { "noGpu", true } });
		REQUIRE_MESSAGE(again["isError"] == false, GetText(again));
		CHECK(again["structuredContent"]["launched"] == false);
		CHECK(again["structuredContent"]["session"]["ProcessId"] == firstProcess);
		nlohmann::json other = client.CallTool("strata_launch_editor", nlohmann::json { { "headless", true } });
		REQUIRE_MESSAGE(other["isError"] == false, GetText(other));
		CHECK(other["structuredContent"]["launched"] == true);
		CHECK(other["structuredContent"]["session"]["ProcessId"] != firstProcess);
		CHECK(client.CallTool("editor_quit")["isError"] == false);

		// The first editor is still running; quit it through its session.
		const std::optional<EditorSessionInfo> firstSession = EditorSession::ReadSessionFile(EditorSession::GetSessionFilePath(sessionDirectory, firstProcess));
		REQUIRE(firstSession.has_value());
		RpcClient first;
		REQUIRE(first.Connect(firstSession->Address, firstSession->Port, firstSession->Token, std::chrono::milliseconds(3000)));
		CHECK(first.Call("editor.quit", nlohmann::json::object(), std::chrono::milliseconds(5000)).IsSuccess());
	}

	TEST_CASE("Launching stops an editor that never becomes reachable")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("McpLaunchSilent") / "Sessions";
		Tests::ScopedEnvironmentVariable fakeEditor("STRATA_TEST_FAKE_EDITOR", "silent");
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory));

		McpServerSpecification specification = Tests::MakeMcpSpecification(sessionDirectory);
		specification.EditorPath = FileSystem::ToUTF8(Tests::GetTestExecutablePath());
		specification.LaunchTimeout = std::chrono::milliseconds(1000);
		Tests::McpTestClient client(specification);
		client.Initialize();

		nlohmann::json launched = client.CallTool("strata_launch_editor", nlohmann::json { { "noGpu", true } });
		CHECK(launched["isError"] == true);
		CHECK(GetText(launched).find("the editor was stopped") != std::string::npos);
	}

	TEST_CASE("Another project's editor does not replace a disconnected one")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("McpPinned");
		const std::filesystem::path firstProject = Tests::CreateTemporaryDirectory("McpPinnedFirst");
		const std::filesystem::path secondProject = Tests::CreateTemporaryDirectory("McpPinnedSecond");
		Tests::McpTestClient client(Tests::MakeMcpSpecification(sessionDirectory));
		client.Initialize();

		auto firstOwner = CreateScope<Tests::LiveProcess>();
		auto firstEditor = CreateScope<Tests::PumpedRpcServer>();
		REQUIRE(Tests::StartFakeEditor(*firstEditor));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(firstOwner->GetProcessId(), firstEditor->GetPort(), "2026-01-01T00:00:00Z", FileSystem::ToUTF8(firstProject))));
		CHECK(client.ListToolNames().size() == Tests::c_GenericMcpTools.size() + c_FakeCoreTools);

		// The first editor exits; an editor for another project is running.
		firstEditor.reset();
		firstOwner.reset();
		Tests::LiveProcess secondOwner;
		Tests::PumpedRpcServer secondEditor;
		REQUIRE(Tests::StartFakeEditor(secondEditor));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(secondOwner.GetProcessId(), secondEditor.GetPort(), "2026-02-01T00:00:00Z", FileSystem::ToUTF8(secondProject))));

		client.GetServer().Tick();
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 1);
		CHECK(client.ListToolNames() == Tests::c_GenericMcpTools);

		nlohmann::json status = client.CallTool("strata_status");
		CHECK(status["structuredContent"]["connected"] == false);
		CHECK(status["structuredContent"]["error"].get<std::string>().find("Disconnected") != std::string::npos);
		CHECK(status["structuredContent"].contains("pinnedEditor"));

		nlohmann::json call = client.CallTool("strata_call", nlohmann::json { { "method", "rpc.ping" } });
		CHECK(call["isError"] == true);
		CHECK(GetText(call).find("Disconnected") != std::string::npos);
	}

	TEST_CASE("The stream runner serves until the input ends")
	{
		std::istringstream input(
			R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})" "\r\n"
			R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n"
			"\n"
			R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})" "\n");

		std::vector<std::string> output;
		McpServer server(Tests::MakeMcpSpecification(Tests::CreateTemporaryDirectory("McpStream")), [&output](const std::string& line) { output.push_back(line); });
		RunMcpServer(server, input, std::chrono::milliseconds(20));

		REQUIRE(output.size() == 2);
		CHECK(JsonRpc::Parse(output[0]).value()["result"]["protocolVersion"] == "2025-03-26");
		CHECK(JsonRpc::Parse(output[1]).value()["result"]["tools"].size() == Tests::c_GenericMcpTools.size());
		CHECK(server.IsInitialized());
	}

	TEST_CASE("Tool helpers")
	{
		CHECK(MakeToolName("entity.create") == "entity_create");
		CHECK(MakeToolName("scene/load file") == "scene_load_file");
		CHECK(MakeToolName("Already_Valid-Name9") == "Already_Valid-Name9");
		CHECK(MakeToolName(std::string(100, 'a')).size() == 64);

		CHECK(MakeToolInputSchema(nullptr) == nlohmann::json { { "type", "object" }, { "properties", nlohmann::json::object() } });
		CHECK(MakeToolInputSchema(nlohmann::json { { "type", "array" } })["type"] == "object");
		nlohmann::json untyped = MakeToolInputSchema(nlohmann::json { { "properties", { { "x", { { "type", "number" } } } } } });
		CHECK(untyped["type"] == "object");
		CHECK(untyped["properties"]["x"]["type"] == "number");

		nlohmann::json error = MakeToolError("boom");
		CHECK(error["isError"] == true);
		CHECK(error["content"][0]["text"] == "boom");

		// A malformed Image field is left in the text instead of becoming image content.
		nlohmann::json malformedImage = MakeToolResult(RpcResult::Success(nlohmann::json { { "Image", { { "Data", 5 } } } }));
		CHECK(malformedImage["content"].size() == 1);
		CHECK(malformedImage["structuredContent"].contains("Image"));
	}
}

TEST_SUITE("CLI.McpToolsets")
{
	TEST_CASE("Commands map to toolsets by exact name, then by group")
	{
		CHECK(McpToolsets::GetCoreCommands().size() == 27);
		CHECK(McpToolsets::IsCore("entity.create"));
		CHECK_FALSE(McpToolsets::IsCore("entity.rename"));
		CHECK(McpToolsets::GetToolsets("entity.create") == std::vector<std::string> { "core", "scene" });
		CHECK(McpToolsets::GetToolsets("entity.rename") == std::vector<std::string> { "scene" });
		CHECK(McpToolsets::GetToolsets("asset.stats") == std::vector<std::string> { "streaming" });
		CHECK(McpToolsets::GetToolsets("asset.info") == std::vector<std::string> { "core", "assets" });
		CHECK(McpToolsets::GetToolsets("editor.perf") == std::vector<std::string> { "perf" });
		CHECK(McpToolsets::GetToolsets("editor.status") == std::vector<std::string> { "core", "editor" });
		CHECK(McpToolsets::GetToolsets("input.click") == std::vector<std::string> { "play" });
		CHECK(McpToolsets::GetToolsets("physics.stats") == std::vector<std::string> { "world" });
		// The project's settings have a toolset of their own; the project's other commands stay with the assets.
		CHECK(McpToolsets::GetToolsets("project.setSettings") == std::vector<std::string> { "settings" });
		CHECK(McpToolsets::GetToolsets("project.save") == std::vector<std::string> { "settings" });
		CHECK(McpToolsets::GetToolsets("project.setCollisionLayers") == std::vector<std::string> { "settings" });
		CHECK(McpToolsets::GetToolsets("project.export") == std::vector<std::string> { "assets" });
		CHECK(McpToolsets::GetToolsets("preferences.set") == std::vector<std::string> { "settings" });
		// Groups the table does not know get a toolset named after them.
		CHECK(McpToolsets::GetToolsets("weather.set") == std::vector<std::string> { "weather" });
		CHECK(McpToolsets::GetToolsets("nodot") == std::vector<std::string> { "nodot" });
	}

	TEST_CASE("Compact definitions keep the first sentence and the schema's structure")
	{
		bool cut = false;
		CHECK(McpToolsets::FirstSentence("Creates an entity. Returns its ID.", 160, &cut) == "Creates an entity.");
		CHECK(cut);
		CHECK(McpToolsets::FirstSentence("Lists assets, e.g. textures. More.", 160, &cut) == "Lists assets, e.g. textures.");
		CHECK(McpToolsets::FirstSentence("One sentence only", 160, &cut) == "One sentence only");
		CHECK_FALSE(cut);
		CHECK(McpToolsets::FirstSentence("A rather long sentence", 12, &cut) == "A rather\xE2\x80\xA6");
		CHECK(cut);
		CHECK(McpToolsets::FirstSentence("Unbroken", 4, &cut) == "Unb\xE2\x80\xA6");
		// Never inside a UTF-8 sequence.
		CHECK(McpToolsets::FirstSentence("\xC3\xA9\xC3\xA9\xC3\xA9", 4, &cut) == "\xC3\xA9\xE2\x80\xA6");
		CHECK(McpToolsets::FirstSentence("Anything", 0, &cut).empty());

		const std::string longDescription = "Moves the entities of the selection by the given offset in world space, keeping their children where they are "
			"relative to them and recording one undo step. Also more.";
		const nlohmann::json schema = {
			{ "type", "object" },
			{ "properties", {
				{ "mode", { { "type", "string" }, { "enum", { "a", "b" } }, { "description", "How to move. The rest is detail." } } },
				{ "box", { { "type", "object" }, { "properties", { { "min", { { "type", "array" }, { "description", "Lower corner. Detail." } } } } } } },
				{ "names", { { "type", "array" }, { "items", { { "type", "string" }, { "description", "A name. Detail." } } } } } } },
			{ "required", { "mode" } } };
		const nlohmann::json tool = McpToolsets::MakeCompactToolDefinition("entity_move", longDescription, schema);
		CHECK(tool["name"] == "entity_move");
		const std::string description = tool["description"].get<std::string>();
		CHECK(description.ends_with(" (details: strata_describe)"));
		CHECK(description.size() <= 160 + 2 + std::string_view(" (details: strata_describe)").size());
		CHECK(tool["inputSchema"]["required"] == nlohmann::json::array({ "mode" }));
		CHECK(tool["inputSchema"]["properties"]["mode"]["enum"] == nlohmann::json::array({ "a", "b" }));
		CHECK(tool["inputSchema"]["properties"]["mode"]["description"] == "How to move.");
		CHECK(tool["inputSchema"]["properties"]["box"]["properties"]["min"]["description"] == "Lower corner.");
		CHECK(tool["inputSchema"]["properties"]["names"]["items"]["description"] == "A name.");
		// Short descriptions stay as they are.
		CHECK(McpToolsets::MakeCompactToolDefinition("x", "Short.", schema)["description"] == "Short.");
	}
}

TEST_SUITE("CLI.McpPresence")
{
	TEST_CASE("The MCP client's name reaches the editor in the handshake")
	{
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));
		Tests::McpTestClient client(Tests::MakeMcpSpecification(Tests::CreateTemporaryDirectory("McpPresence"), editor.GetPort()));
		client.Initialize(); // clientInfo.name: StrataTests
		client.ListToolNames();
		REQUIRE(Tests::WaitUntil([&]() { return editor.GetServer().GetClients().size() == 1; }));
		CHECK(editor.GetServer().GetClients()[0].Name == "StrataTests");
	}
}

#endif
