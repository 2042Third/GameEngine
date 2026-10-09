#if defined(STRATA_TESTS_HAVE_CLI)

#include <doctest/doctest.h>

#include "CLI/FakeEditor.h"
#include "CLI/McpServer.h"
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
	McpServerSpecification MakeSpecification(const std::filesystem::path& sessionDirectory, std::optional<uint16_t> port = std::nullopt)
	{
		McpServerSpecification specification;
		specification.Connection.SessionDirectory = sessionDirectory;
		specification.Connection.Port = port;
		specification.Connection.Token = port ? Tests::c_FakeEditorToken : "";
		specification.Connection.ConnectTimeout = std::chrono::milliseconds(3000);
		specification.CallTimeout = std::chrono::milliseconds(5000);
		specification.LaunchTimeout = std::chrono::milliseconds(2000);
		return specification;
	}

	// Drives an McpServer like an MCP client and records everything it writes.
	class McpTestClient
	{
	public:
		explicit McpTestClient(McpServerSpecification specification)
			: m_Server(std::move(specification), [this](const std::string& line) { m_Output.push_back(line); })
		{
		}

		void SendLine(std::string_view line)
		{
			m_Server.HandleLine(line);
		}

		// Sends a request and returns its response (null if none was written).
		nlohmann::json Request(const std::string& method, nlohmann::json params = nlohmann::json::object())
		{
			const int64_t id = m_NextId++;
			const size_t firstNew = m_Output.size();
			m_Server.HandleLine(JsonRpc::Serialize(JsonRpc::MakeRequest(id, method, std::move(params))));
			for (size_t index = firstNew; index < m_Output.size(); index++)
			{
				std::optional<nlohmann::json> message = JsonRpc::Parse(m_Output[index]);
				if (message && message->is_object() && message->contains("id") && (*message)["id"] == id)
					return *message;
			}
			return nullptr;
		}

		void Notify(const std::string& method)
		{
			m_Server.HandleLine(JsonRpc::Serialize(JsonRpc::MakeNotification(method, nullptr)));
		}

		void Initialize()
		{
			nlohmann::json response = Request("initialize", nlohmann::json {
				{ "protocolVersion", "2025-06-18" },
				{ "capabilities", nlohmann::json::object() },
				{ "clientInfo", { { "name", "StrataTests" }, { "version", "1.0" } } } });
			REQUIRE(response.contains("result"));
			Notify("notifications/initialized");
		}

		nlohmann::json CallTool(const std::string& name, nlohmann::json arguments = nlohmann::json::object())
		{
			nlohmann::json response = Request("tools/call", nlohmann::json { { "name", name }, { "arguments", std::move(arguments) } });
			REQUIRE(response.contains("result"));
			return response["result"];
		}

		std::vector<std::string> ListToolNames()
		{
			nlohmann::json response = Request("tools/list");
			REQUIRE(response.contains("result"));
			std::vector<std::string> names;
			for (const nlohmann::json& tool : response["result"]["tools"])
				names.push_back(tool["name"].get<std::string>());
			return names;
		}

		size_t CountNotifications(const std::string& method) const
		{
			return static_cast<size_t>(std::count_if(m_Output.begin(), m_Output.end(), [&](const std::string& line)
			{
				std::optional<nlohmann::json> message = JsonRpc::Parse(line);
				return message && message->is_object() && !message->contains("id") && message->value("method", "") == method;
			}));
		}

		McpServer& GetServer() { return m_Server; }
		const std::vector<std::string>& GetOutput() const { return m_Output; }
	private:
		std::vector<std::string> m_Output;
		McpServer m_Server;
		int64_t m_NextId = 1;
	};

	const std::vector<std::string> c_GenericTools = { "strata_status", "strata_launch_editor", "strata_list_methods", "strata_call" };

	std::string GetText(const nlohmann::json& toolResult, size_t index = 0)
	{
		return toolResult.at("content").at(index).at("text").get<std::string>();
	}
}

TEST_SUITE("CLI.Mcp")
{
	TEST_CASE("Initialize negotiates the protocol version")
	{
		McpTestClient client(MakeSpecification(Tests::CreateTemporaryDirectory("McpInitialize")));

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
		CHECK(result["instructions"].is_string());

		CHECK_FALSE(client.GetServer().IsInitialized());
		const size_t outputBefore = client.GetOutput().size();
		client.Notify("notifications/initialized");
		CHECK(client.GetServer().IsInitialized());
		CHECK(client.GetOutput().size() == outputBefore); // Notifications are never answered
	}

	TEST_CASE("Protocol-level errors and utilities")
	{
		McpTestClient client(MakeSpecification(Tests::CreateTemporaryDirectory("McpProtocol")));

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
		McpTestClient client(MakeSpecification(Tests::CreateTemporaryDirectory("McpNoEditor")));
		client.Initialize();

		nlohmann::json response = client.Request("tools/list");
		nlohmann::json& tools = response["result"]["tools"];
		REQUIRE(tools.size() == c_GenericTools.size());
		for (size_t index = 0; index < tools.size(); index++)
		{
			CHECK(tools[index]["name"] == c_GenericTools[index]);
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
	}

	TEST_CASE("Editor methods are exposed as tools")
	{
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));
		McpTestClient client(MakeSpecification(Tests::CreateTemporaryDirectory("McpTools"), editor.GetPort()));
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

		for (const std::string& generic : c_GenericTools)
			CHECK(std::find(names.begin(), names.end(), generic) != names.end());
		for (const char* expected : { "entity_create", "scene_fail", "viewport_capture", "math_add", "list_sum" })
			CHECK(std::find(names.begin(), names.end(), expected) != names.end());
		for (const std::string& name : names)
			CHECK(name.rfind("rpc_", 0) == std::string::npos);
		CHECK(names.size() == c_GenericTools.size() + 5);

		const auto create = std::find_if(tools.begin(), tools.end(), [](const nlohmann::json& tool) { return tool.value("name", "") == "entity_create"; });
		REQUIRE(create != tools.end());
		CHECK((*create)["description"] == "Creates an entity");
		CHECK((*create)["inputSchema"]["required"] == nlohmann::json::array({ "Name" }));
		CHECK((*create)["inputSchema"]["properties"]["Name"]["type"] == "string");

		// Joining after the initial listing, the client was never told about a change.
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 0);
	}

	TEST_CASE("Tool calls are routed to the editor and results are mapped")
	{
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));
		McpTestClient client(MakeSpecification(Tests::CreateTemporaryDirectory("McpCalls"), editor.GetPort()));
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
			CHECK(status["structuredContent"]["editorTools"] == 5);
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
		McpTestClient client(MakeSpecification(sessionDirectory));
		client.Initialize();
		CHECK(client.ListToolNames() == c_GenericTools);

		client.GetServer().Tick();
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 0);

		Tests::LiveProcess owner;
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(owner.GetProcessId(), editor.GetPort(), EditorSession::GetCurrentTimestamp())));

		client.GetServer().Tick();
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 1);
		CHECK(client.ListToolNames().size() == c_GenericTools.size() + 5);

		client.GetServer().Tick();
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 1); // Unchanged: no repeat

		editor.Stop();
		client.GetServer().Tick();
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 2);
		CHECK(client.ListToolNames() == c_GenericTools);
	}

	TEST_CASE("Launching validates its arguments and the editor path")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("McpLaunch");
		McpServerSpecification specification = MakeSpecification(directory);
		specification.EditorPath = FileSystem::ToUTF8(directory / "NoSuchEditor.exe");
		McpTestClient client(specification);
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

		McpServerSpecification specification = MakeSpecification(sessionDirectory);
		specification.EditorPath = FileSystem::ToUTF8(sessionDirectory / "NoSuchEditor.exe");
		McpTestClient client(specification);
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

		McpServerSpecification specification = MakeSpecification(sessionDirectory);
		specification.EditorPath = FileSystem::ToUTF8(Tests::GetTestExecutablePath());
		specification.LaunchTimeout = std::chrono::milliseconds(20000);
		McpTestClient client(specification);
		client.Initialize();
		CHECK(client.ListToolNames() == c_GenericTools);

		nlohmann::json launched = client.CallTool("strata_launch_editor", nlohmann::json { { "project", FileSystem::ToUTF8(projectDirectory) }, { "headless", true } });
		REQUIRE_MESSAGE(launched["isError"] == false, GetText(launched));
		CHECK(launched["structuredContent"]["launched"] == true);
		CHECK(launched["structuredContent"]["session"]["Headless"] == true);
		CHECK_FALSE(launched["structuredContent"]["session"].contains("Token"));
		CHECK(launched["structuredContent"]["editorTools"] == 2);
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
		CHECK(client.ListToolNames() == c_GenericTools);
	}

	TEST_CASE("Launching without a project starts an editor without one")
	{
		const std::filesystem::path sessionDirectory = Tests::CreateTemporaryDirectory("McpLaunchEmpty") / "Sessions";
		Tests::ScopedEnvironmentVariable fakeEditor("STRATA_TEST_FAKE_EDITOR", "1");
		Tests::ScopedEnvironmentVariable sessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(sessionDirectory));

		McpServerSpecification specification = MakeSpecification(sessionDirectory);
		specification.EditorPath = FileSystem::ToUTF8(Tests::GetTestExecutablePath());
		specification.LaunchTimeout = std::chrono::milliseconds(20000);
		McpTestClient client(specification);
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

		McpServerSpecification specification = MakeSpecification(sessionDirectory);
		specification.EditorPath = FileSystem::ToUTF8(Tests::GetTestExecutablePath());
		specification.LaunchTimeout = std::chrono::milliseconds(1000);
		McpTestClient client(specification);
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
		McpTestClient client(MakeSpecification(sessionDirectory));
		client.Initialize();

		auto firstOwner = CreateScope<Tests::LiveProcess>();
		auto firstEditor = CreateScope<Tests::PumpedRpcServer>();
		REQUIRE(Tests::StartFakeEditor(*firstEditor));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(firstOwner->GetProcessId(), firstEditor->GetPort(), "2026-01-01T00:00:00Z", FileSystem::ToUTF8(firstProject))));
		CHECK(client.ListToolNames().size() == c_GenericTools.size() + 5);

		// The first editor exits; an editor for another project is running.
		firstEditor.reset();
		firstOwner.reset();
		Tests::LiveProcess secondOwner;
		Tests::PumpedRpcServer secondEditor;
		REQUIRE(Tests::StartFakeEditor(secondEditor));
		REQUIRE(Tests::WriteFakeSessionFile(sessionDirectory, Tests::MakeFakeSession(secondOwner.GetProcessId(), secondEditor.GetPort(), "2026-02-01T00:00:00Z", FileSystem::ToUTF8(secondProject))));

		client.GetServer().Tick();
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 1);
		CHECK(client.ListToolNames() == c_GenericTools);

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
		McpServer server(MakeSpecification(Tests::CreateTemporaryDirectory("McpStream")), [&output](const std::string& line) { output.push_back(line); });
		RunMcpServer(server, input, std::chrono::milliseconds(20));

		REQUIRE(output.size() == 2);
		CHECK(JsonRpc::Parse(output[0]).value()["result"]["protocolVersion"] == "2025-03-26");
		CHECK(JsonRpc::Parse(output[1]).value()["result"]["tools"].size() == c_GenericTools.size());
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

#endif
