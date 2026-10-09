#if defined(STRATA_TESTS_HAVE_CLI)

#include <doctest/doctest.h>

#include "CLI/CliApplication.h"
#include "CLI/FakeEditor.h"
#include "Strata/Core/Version.h"
#include "Strata/Network/JsonRpc.h"
#include "Strata/Network/Socket.h"
#include "TestHelpers.h"

#include <sstream>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::CLI;

namespace
{
	struct CliRun
	{
		int ExitCode = -1;
		std::string Output;
		std::string ErrorOutput;
	};

	CliRun Run(const std::vector<std::string>& arguments, const std::string& input = {})
	{
		std::istringstream inputStream(input);
		std::ostringstream output;
		std::ostringstream errorOutput;
		CliRun run;
		run.ExitCode = RunCli(arguments, inputStream, output, errorOutput);
		run.Output = output.str();
		run.ErrorOutput = errorOutput.str();
		return run;
	}

	// Isolates a test from the developer's environment: no explicit editor endpoint, an empty session directory.
	struct IsolatedEnvironment
	{
		explicit IsolatedEnvironment(const std::string& name)
			: SessionDirectory(Tests::CreateTemporaryDirectory(name)), SessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(SessionDirectory)),
			PortOverride("STRATA_EDITOR_PORT", ""), TokenOverride("STRATA_EDITOR_TOKEN", ""), EditorOverride("STRATA_EDITOR_PATH", "")
		{
		}

		std::filesystem::path SessionDirectory;
		Tests::ScopedEnvironmentVariable SessionOverride;
		Tests::ScopedEnvironmentVariable PortOverride;
		Tests::ScopedEnvironmentVariable TokenOverride;
		Tests::ScopedEnvironmentVariable EditorOverride;
	};

	std::vector<std::string> ExplicitEndpoint(const Tests::PumpedRpcServer& editor)
	{
		return { "--port", std::to_string(editor.GetPort()), "--token", Tests::c_FakeEditorToken };
	}

	std::vector<std::string> Concat(std::vector<std::string> first, const std::vector<std::string>& second)
	{
		first.insert(first.end(), second.begin(), second.end());
		return first;
	}
}

TEST_SUITE("CLI.Commands")
{
	TEST_CASE("Arguments are parsed into commands, positionals and options")
	{
		std::string error;
		std::optional<CliArguments> parsed = ParseCliArguments({ "call", "--port", "4000", "entity.create", "--token=abc", "{\"Name\":\"A\"}", "--timeout", "250", "--headless" }, error);
		REQUIRE_MESSAGE(parsed.has_value(), error);
		CHECK(parsed->Command == "call");
		CHECK(parsed->Positionals == std::vector<std::string> { "entity.create", "{\"Name\":\"A\"}" });
		CHECK(parsed->Port.value() == 4000);
		CHECK(parsed->Token.value() == "abc");
		CHECK(parsed->TimeoutMilliseconds.value() == 250);
		CHECK(parsed->Headless);
		CHECK_FALSE(parsed->Json);

		// Huge timeouts are clamped to the longest wait sockets accept.
		std::optional<CliArguments> unbounded = ParseCliArguments({ "mcp", "--wait-timeout", "9223372036854775807", "--timeout", "100000000000" }, error);
		REQUIRE_MESSAGE(unbounded.has_value(), error);
		CHECK(unbounded->WaitTimeoutMilliseconds.value() == c_MaxSocketTimeout.count());
		CHECK(unbounded->TimeoutMilliseconds.value() == c_MaxSocketTimeout.count());

		std::optional<CliArguments> separated = ParseCliArguments({ "call", "--", "--weird-method" }, error);
		REQUIRE(separated.has_value());
		CHECK(separated->Positionals == std::vector<std::string> { "--weird-method" });

		CHECK_FALSE(ParseCliArguments({ "call", "--port", "70000" }, error).has_value());
		CHECK(error.find("70000") != std::string::npos);
		CHECK_FALSE(ParseCliArguments({ "call", "--port", "abc" }, error).has_value());
		CHECK_FALSE(ParseCliArguments({ "call", "--bogus" }, error).has_value());
		CHECK(error.find("--bogus") != std::string::npos);
		CHECK_FALSE(ParseCliArguments({ "call", "--project" }, error).has_value());
		CHECK_FALSE(ParseCliArguments({ "call", "--timeout", "0" }, error).has_value());
		CHECK_FALSE(ParseCliArguments({ "call", "--headless=yes" }, error).has_value());

		std::optional<CliArguments> empty = ParseCliArguments({}, error);
		REQUIRE(empty.has_value());
		CHECK(empty->Command.empty());
	}

	TEST_CASE("Connection options follow the discovery precedence")
	{
		IsolatedEnvironment environment("CliOptions");
		std::string error;

		CliArguments explicitArguments;
		explicitArguments.Port = 4100;
		explicitArguments.Token = "explicit";
		explicitArguments.Project = "relative/project";
		std::optional<EditorConnectionOptions> options = BuildConnectionOptions(explicitArguments, error);
		REQUIRE(options.has_value());
		CHECK(options->Port.value() == 4100);
		CHECK(options->Token == "explicit");
		CHECK(options->ProjectDirectory.is_absolute());

		{
			Tests::ScopedEnvironmentVariable port("STRATA_EDITOR_PORT", "4200");
			Tests::ScopedEnvironmentVariable token("STRATA_EDITOR_TOKEN", "from-environment");
			std::optional<EditorConnectionOptions> fromEnvironment = BuildConnectionOptions(CliArguments(), error);
			REQUIRE(fromEnvironment.has_value());
			CHECK(fromEnvironment->Port.value() == 4200);
			CHECK(fromEnvironment->Token == "from-environment");

			// An explicit --port still wins over the environment.
			CHECK(BuildConnectionOptions(explicitArguments, error)->Port.value() == 4100);
		}

		{
			Tests::ScopedEnvironmentVariable port("STRATA_EDITOR_PORT", "not-a-port");
			CHECK_FALSE(BuildConnectionOptions(CliArguments(), error).has_value());
			CHECK(error.find("STRATA_EDITOR_PORT") != std::string::npos);
		}

		CliArguments tokenOnly;
		tokenOnly.Token = "x";
		CHECK_FALSE(BuildConnectionOptions(tokenOnly, error).has_value());

		// Discovered editors are always on loopback, so a host only makes sense for an explicit endpoint.
		CliArguments hostOnly;
		hostOnly.Host = "192.168.1.10";
		CHECK_FALSE(BuildConnectionOptions(hostOnly, error).has_value());
		CHECK(error.find("--host requires --port") != std::string::npos);
		CliArguments hostAndPort = hostOnly;
		hostAndPort.Port = 4300;
		hostAndPort.Token = "remote";
		std::optional<EditorConnectionOptions> remote = BuildConnectionOptions(hostAndPort, error);
		REQUIRE(remote.has_value());
		CHECK(remote->Host == "192.168.1.10");

		// An explicit port takes its token from --token or the environment, and is refused without one.
		CliArguments portOnly;
		portOnly.Port = 4400;
		CHECK_FALSE(BuildConnectionOptions(portOnly, error).has_value());
		CHECK(error.find("session token") != std::string::npos);
		{
			Tests::ScopedEnvironmentVariable token("STRATA_EDITOR_TOKEN", "from-environment");
			std::optional<EditorConnectionOptions> withEnvironmentToken = BuildConnectionOptions(portOnly, error);
			REQUIRE_MESSAGE(withEnvironmentToken.has_value(), error);
			CHECK(withEnvironmentToken->Token == "from-environment");
		}
		{
			Tests::ScopedEnvironmentVariable port("STRATA_EDITOR_PORT", "4200");
			CHECK_FALSE(BuildConnectionOptions(CliArguments(), error).has_value());
			CHECK(error.find("session token") != std::string::npos);
		}

		std::optional<EditorConnectionOptions> discovery = BuildConnectionOptions(CliArguments(), error);
		REQUIRE(discovery.has_value());
		CHECK_FALSE(discovery->Port.has_value());
	}

	TEST_CASE("Help, version and usage errors")
	{
		IsolatedEnvironment environment("CliUsage");

		const CliRun version = Run({ "--version" });
		CHECK(version.ExitCode == ExitCode::Success);
		CHECK(version.Output == std::string("StrataCLI ") + c_EngineVersion + "\n");

		const CliRun help = Run({ "--help" });
		CHECK(help.ExitCode == ExitCode::Success);
		CHECK(help.Output.find("Usage:") != std::string::npos);
		CHECK(help.Output.find("STRATA_SESSION_DIR") != std::string::npos);

		CHECK(Run({}).ExitCode == ExitCode::UsageError);
		CHECK(Run({ "frobnicate" }).ExitCode == ExitCode::UsageError);
		CHECK(Run({ "call" }).ExitCode == ExitCode::UsageError);
		CHECK(Run({ "call", "a.b", "[1]", "extra" }).ExitCode == ExitCode::UsageError);
		CHECK(Run({ "call", "a.b", "{not json" }).ExitCode == ExitCode::UsageError);
		CHECK(Run({ "call", "a.b", "42" }).ExitCode == ExitCode::UsageError);
		CHECK(Run({ "list", "extra" }).ExitCode == ExitCode::UsageError);
		CHECK(Run({ "launch" }).ExitCode == ExitCode::UsageError);

		const CliRun badOption = Run({ "status", "--nope" });
		CHECK(badOption.ExitCode == ExitCode::UsageError);
		CHECK(badOption.ErrorOutput.find("--nope") != std::string::npos);
		CHECK(badOption.Output.empty());
	}

	TEST_CASE("call prints results and maps failures to exit codes")
	{
		IsolatedEnvironment environment("CliCall");
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));

		const CliRun success = Run(Concat({ "call", "entity.create", "{\"Name\":\"Hero\"}" }, ExplicitEndpoint(editor)));
		CHECK(success.ExitCode == ExitCode::Success);
		CHECK(success.ErrorOutput.empty());
		nlohmann::json result = JsonRpc::Parse(success.Output).value();
		CHECK(result["Entity"] == 42);
		CHECK(result["Name"] == "Hero");

		const CliRun rpcError = Run(Concat({ "call", "scene.fail" }, ExplicitEndpoint(editor)));
		CHECK(rpcError.ExitCode == ExitCode::RpcError);
		CHECK(rpcError.Output.empty());
		nlohmann::json error = JsonRpc::Parse(rpcError.ErrorOutput).value();
		CHECK(error["code"] == JsonRpc::ErrorCode::InvalidParams);
		CHECK(error["message"] == "Scene 'x' not found");
		CHECK(error["data"]["Scene"] == "x");

		const CliRun unauthorized = Run({ "call", "rpc.ping", "--port", std::to_string(editor.GetPort()), "--token", "wrong" });
		CHECK(unauthorized.ExitCode == ExitCode::ConnectionFailure);

		Tests::RefusingPort refusingPort;
		const CliRun unreachable = Run({ "call", "rpc.ping", "--port", std::to_string(refusingPort.GetPort()), "--token", "x" });
		CHECK(unreachable.ExitCode == ExitCode::ConnectionFailure);
		CHECK(unreachable.ErrorOutput.find("error:") != std::string::npos);

		// An explicit endpoint without a token could never authenticate.
		const CliRun withoutToken = Run({ "call", "rpc.ping", "--port", std::to_string(editor.GetPort()) });
		CHECK(withoutToken.ExitCode == ExitCode::UsageError);
		CHECK(withoutToken.ErrorOutput.find("STRATA_EDITOR_TOKEN") != std::string::npos);

		const CliRun noSession = Run({ "call", "rpc.ping" });
		CHECK(noSession.ExitCode == ExitCode::ConnectionFailure);
		CHECK(noSession.ErrorOutput.find("No running Strata editor") != std::string::npos);
	}

	TEST_CASE("call discovers the editor through session files")
	{
		IsolatedEnvironment environment("CliDiscovery");
		Tests::LiveProcess owner;
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));
		REQUIRE(Tests::WriteFakeSessionFile(environment.SessionDirectory, Tests::MakeFakeSession(owner.GetProcessId(), editor.GetPort(), "2026-01-01T00:00:00Z")));

		const CliRun run = Run({ "call", "math.add", "{\"a\":1,\"b\":2}" });
		CHECK(run.ExitCode == ExitCode::Success);
		CHECK(JsonRpc::Parse(run.Output).value() == 3.0);

		// The editor can also be selected through STRATA_EDITOR_PORT / STRATA_EDITOR_TOKEN.
		Tests::ScopedEnvironmentVariable port("STRATA_EDITOR_PORT", std::to_string(editor.GetPort()));
		Tests::ScopedEnvironmentVariable token("STRATA_EDITOR_TOKEN", Tests::c_FakeEditorToken);
		CHECK(Run({ "call", "rpc.ping" }).ExitCode == ExitCode::Success);
	}

	TEST_CASE("list and status describe the editor")
	{
		IsolatedEnvironment environment("CliList");
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));

		const CliRun list = Run(Concat({ "list" }, ExplicitEndpoint(editor)));
		CHECK(list.ExitCode == ExitCode::Success);
		CHECK(list.Output.find("entity.create") != std::string::npos);
		CHECK(list.Output.find("Creates an entity") != std::string::npos);
		CHECK(list.Output.find("rpc.ping") != std::string::npos);

		const CliRun listJson = Run(Concat({ "list", "--json" }, ExplicitEndpoint(editor)));
		CHECK(listJson.ExitCode == ExitCode::Success);
		CHECK(JsonRpc::Parse(listJson.Output).value()["methods"].size() == 9); // 4 built-ins + 5 editor methods

		const CliRun status = Run(Concat({ "status" }, ExplicitEndpoint(editor)));
		CHECK(status.ExitCode == ExitCode::Success);
		nlohmann::json statusJson = JsonRpc::Parse(status.Output).value();
		CHECK(statusJson["connected"] == true);
		CHECK(statusJson["endpoint"]["port"] == editor.GetPort());

		const CliRun disconnected = Run({ "status" });
		CHECK(disconnected.ExitCode == ExitCode::ConnectionFailure);
		CHECK(JsonRpc::Parse(disconnected.Output).value()["connected"] == false);
	}

	TEST_CASE("launch reports a missing editor executable")
	{
		IsolatedEnvironment environment("CliLaunch");
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("CliLaunchProject");

		const CliRun run = Run({ "launch", "--project", FileSystem::ToUTF8(project), "--editor", FileSystem::ToUTF8(project / "NoSuchEditor.exe"), "--wait-timeout", "500" });
		CHECK(run.ExitCode == ExitCode::ConnectionFailure);
		CHECK(run.ErrorOutput.find("not found") != std::string::npos);
		CHECK(run.Output.empty());
	}

	TEST_CASE("launch starts the editor and prints its session without the token")
	{
		IsolatedEnvironment environment("CliLaunchEditor");
		const std::filesystem::path project = Tests::CreateTemporaryDirectory("CliLaunchEditorProject");
		Tests::ScopedEnvironmentVariable fakeEditor("STRATA_TEST_FAKE_EDITOR", "1");

		const CliRun run = Run({ "launch", "--project", FileSystem::ToUTF8(project), "--editor", FileSystem::ToUTF8(Tests::GetTestExecutablePath()), "--headless", "--wait-timeout", "20000" });
		REQUIRE_MESSAGE(run.ExitCode == ExitCode::Success, run.ErrorOutput);
		nlohmann::json printed = JsonRpc::Parse(run.Output).value();
		CHECK_FALSE(printed.contains("Token"));
		CHECK(printed["Headless"] == true);
		REQUIRE(printed["Port"].is_number_unsigned());
		REQUIRE(printed["ProcessId"].is_number_unsigned());

		// The token is only in the private session file.
		const std::vector<EditorSessionInfo> sessions = EditorSession::FindSessions(environment.SessionDirectory);
		REQUIRE(sessions.size() == 1);
		CHECK(sessions[0].ProcessId == printed["ProcessId"].get<uint32_t>());
		CHECK(run.Output.find(sessions[0].Token) == std::string::npos);

		// The editor removes its session files when it exits. (Its process is not waited for here: the launch command
		// leaves it running detached, and on POSIX an exited child of this test process stays a zombie.)
		const CliRun quit = Run({ "call", "editor.quit", "--project", FileSystem::ToUTF8(project) });
		CHECK(quit.ExitCode == ExitCode::Success);
		const std::filesystem::path sessionFile = EditorSession::GetSessionFilePath(environment.SessionDirectory, sessions[0].ProcessId);
		CHECK(Tests::WaitUntil([&]() { return !FileSystem::Exists(sessionFile); }, std::chrono::milliseconds(10000)));
	}

	TEST_CASE("mcp serves the protocol on the given streams")
	{
		IsolatedEnvironment environment("CliMcp");
		const CliRun run = Run({ "mcp" },
			R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05"}})" "\n"
			R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"strata_status","arguments":{}}})" "\n");
		CHECK(run.ExitCode == ExitCode::Success);

		std::istringstream lines(run.Output);
		std::vector<nlohmann::json> messages;
		std::string line;
		while (std::getline(lines, line))
			messages.push_back(JsonRpc::Parse(line).value());
		REQUIRE(messages.size() == 2);
		CHECK(messages[0]["result"]["protocolVersion"] == "2024-11-05");
		CHECK(messages[1]["result"]["structuredContent"]["connected"] == false);
	}
}

#endif
