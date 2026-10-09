#if defined(STRATA_TESTS_HAVE_CLI)

#include <doctest/doctest.h>

#include "CLI/CliApplication.h"
#include "CLI/FakeEditor.h"
#include "CLI/ImageOutput.h"
#include "Strata/Core/Version.h"
#include "Strata/Network/JsonRpc.h"
#include "Strata/Network/Socket.h"
#include "TestHelpers.h"

#include <mutex>
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

		// "-" (params from standard input) is a positional, not an option.
		std::optional<CliArguments> fromInput = ParseCliArguments({ "call", "viewport.capture", "-", "--save-image", "shot.png", "--no-gpu" }, error);
		REQUIRE_MESSAGE(fromInput.has_value(), error);
		CHECK(fromInput->Positionals == std::vector<std::string> { "viewport.capture", "-" });
		CHECK(fromInput->SaveImage.value() == "shot.png");
		CHECK(fromInput->NoGpu);

		std::optional<CliArguments> idle = ParseCliArguments({ "launch", "--idle-timeout", "300" }, error);
		REQUIRE_MESSAGE(idle.has_value(), error);
		CHECK(idle->IdleTimeoutSeconds.value() == 300);
		CHECK(ParseCliArguments({ "mcp", "--idle-timeout", "0" }, error)->IdleTimeoutSeconds.value() == 0);
		CHECK_FALSE(ParseCliArguments({ "launch", "--idle-timeout", "-1" }, error).has_value());
		CHECK(error.find("--idle-timeout") != std::string::npos);
		CHECK_FALSE(ParseCliArguments({ "launch", "--idle-timeout", "soon" }, error).has_value());

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
		CHECK(Run({ "launch", "extra" }).ExitCode == ExitCode::UsageError);

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

	TEST_CASE("A call without an answer in time has its own exit code")
	{
		IsolatedEnvironment environment("CliTimeout");
		Tests::PumpedRpcServer editor;
		std::mutex responderMutex;
		Ref<RpcResponder> heldResponder;
		RpcMethodInfo hold;
		hold.Name = "test.hold";
		REQUIRE(editor.GetServer().RegisterMethod(hold, [&](const nlohmann::json&, const Ref<RpcResponder>& responder)
		{
			std::scoped_lock<std::mutex> lock(responderMutex);
			heldResponder = responder; // Never answered while the call waits, like a command that takes long
		}));
		REQUIRE(Tests::StartFakeEditor(editor));

		// Not a lost editor (exit code 2): the command may still be running.
		const CliRun timedOut = Run(Concat({ "call", "test.hold", "--timeout", "300" }, ExplicitEndpoint(editor)));
		CHECK(timedOut.ExitCode == ExitCode::Timeout);
		CHECK(timedOut.Output.empty());
		CHECK(JsonRpc::Parse(timedOut.ErrorOutput).value()["code"] == JsonRpc::ErrorCode::Timeout);
		editor.Stop();
	}

	TEST_CASE("call reads its params from standard input or a file")
	{
		IsolatedEnvironment environment("CliParams");
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));

		const CliRun fromInput = Run(Concat({ "call", "math.add", "-" }, ExplicitEndpoint(editor)), "{\"a\": 2,\n \"b\": 3}\n");
		CHECK(fromInput.ExitCode == ExitCode::Success);
		CHECK(JsonRpc::Parse(fromInput.Output).value() == 5.0);

		const std::filesystem::path file = Tests::CreateTemporaryDirectory("CliParamsFile") / "params \xC3\xA9.json";
		REQUIRE(FileSystem::WriteText(file, "{\"a\": 1, \"b\": 1}"));
		const CliRun fromFile = Run(Concat({ "call", "math.add", "@" + FileSystem::ToUTF8(file) }, ExplicitEndpoint(editor)));
		CHECK(fromFile.ExitCode == ExitCode::Success);
		CHECK(JsonRpc::Parse(fromFile.Output).value() == 2.0);

		const CliRun missingFile = Run(Concat({ "call", "math.add", "@" + FileSystem::ToUTF8(file.parent_path() / "missing.json") }, ExplicitEndpoint(editor)));
		CHECK(missingFile.ExitCode == ExitCode::UsageError);
		CHECK(missingFile.ErrorOutput.find("cannot read the params file") != std::string::npos);

		const CliRun invalidInput = Run(Concat({ "call", "math.add", "-" }, ExplicitEndpoint(editor)), "not json");
		CHECK(invalidInput.ExitCode == ExitCode::UsageError);
		CHECK(invalidInput.ErrorOutput.find("standard input") != std::string::npos);

		// A UTF-8 byte order mark (as Windows tools write) is skipped; UTF-16 is refused with a hint.
		const std::filesystem::path withBom = file.parent_path() / "bom.json";
		REQUIRE(FileSystem::WriteText(withBom, "\xEF\xBB\xBF{\"a\": 4, \"b\": 4}"));
		const CliRun fromBomFile = Run(Concat({ "call", "math.add", "@" + FileSystem::ToUTF8(withBom) }, ExplicitEndpoint(editor)));
		CHECK(fromBomFile.ExitCode == ExitCode::Success);
		CHECK(JsonRpc::Parse(fromBomFile.Output).value() == 8.0);
		const CliRun utf16 = Run(Concat({ "call", "math.add", "-" }, ExplicitEndpoint(editor)), std::string("\xFF\xFE{\0}\0", 6));
		CHECK(utf16.ExitCode == ExitCode::UsageError);
		CHECK(utf16.ErrorOutput.find("UTF-16") != std::string::npos);
	}

	TEST_CASE("call saves image results to a file")
	{
		IsolatedEnvironment environment("CliImage");
		Tests::PumpedRpcServer editor;
		REQUIRE(Tests::StartFakeEditor(editor));
		const std::filesystem::path image = Tests::CreateTemporaryDirectory("CliImageOutput") / "Shots" / "capture.png";

		const CliRun saved = Run(Concat({ "call", "viewport.capture", "--save-image", FileSystem::ToUTF8(image) }, ExplicitEndpoint(editor)));
		REQUIRE_MESSAGE(saved.ExitCode == ExitCode::Success, saved.ErrorOutput);
		const nlohmann::json printed = JsonRpc::Parse(saved.Output).value();
		CHECK(printed["Width"] == 2);
		CHECK(printed["Image"]["MimeType"] == "image/png");
		CHECK(printed["Image"]["Size"] == 8);
		CHECK_FALSE(printed["Image"].contains("Data"));
		CHECK(std::filesystem::equivalent(FileSystem::FromUTF8(printed["Image"]["File"].get<std::string>()), image));
		const std::vector<uint8_t> expected = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
		CHECK(FileSystem::ReadBytes(image).value_or(std::vector<uint8_t>()) == expected);

		// A result without an image is reported instead of silently printed.
		const CliRun noImage = Run(Concat({ "call", "math.add", "{\"a\":1,\"b\":2}", "--save-image", FileSystem::ToUTF8(image) }, ExplicitEndpoint(editor)));
		CHECK(noImage.ExitCode == ExitCode::OutputError);
		CHECK(noImage.ErrorOutput.find("no image") != std::string::npos);
	}

	TEST_CASE("Base64 is decoded strictly")
	{
		auto decode = [](std::string_view text) -> std::optional<std::string>
		{
			const std::optional<std::vector<uint8_t>> bytes = DecodeBase64(text);
			if (!bytes)
				return std::nullopt;
			return std::string(bytes->begin(), bytes->end());
		};
		// RFC 4648 test vectors, with and without padding.
		CHECK(decode("") == std::string());
		CHECK(decode("Zg==") == "f");
		CHECK(decode("Zm8=") == "fo");
		CHECK(decode("Zm9v") == "foo");
		CHECK(decode("Zm9vYg==") == "foob");
		CHECK(decode("Zm9vYmE=") == "fooba");
		CHECK(decode("Zm9vYmFy") == "foobar");
		CHECK(decode("Zm9vYg") == "foob");
		CHECK(decode("Zm9v\r\nYmFy") == "foobar");
		CHECK(decode("+/+/") == std::string("\xFB\xFF\xBF", 3));

		CHECK_FALSE(decode("Z").has_value());
		CHECK_FALSE(decode("Zg=").has_value());
		CHECK_FALSE(decode("Zg===").has_value());
		CHECK_FALSE(decode("Zg==Zg==").has_value());
		CHECK_FALSE(decode("Zm9v!").has_value());
		CHECK_FALSE(decode("Zm9v-_").has_value()); // The URL-safe alphabet is not accepted

		nlohmann::json noImage = { { "Width", 1 } };
		std::string error;
		CHECK_FALSE(SaveResultImage(noImage, Tests::CreateTemporaryDirectory("CliNoImage") / "x.png", error));
		CHECK(noImage == nlohmann::json { { "Width", 1 } });
		nlohmann::json badData = { { "Image", { { "MimeType", "image/png" }, { "Data", "***" } } } };
		CHECK_FALSE(SaveResultImage(badData, Tests::CreateTemporaryDirectory("CliBadImage") / "x.png", error));
		CHECK(error.find("base64") != std::string::npos);
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

	TEST_CASE("launch starts an editor without a project and without a GPU")
	{
		IsolatedEnvironment environment("CliLaunchEmpty");
		Tests::ScopedEnvironmentVariable fakeEditor("STRATA_TEST_FAKE_EDITOR", "1");

		const CliRun run = Run({ "launch", "--no-gpu", "--editor", FileSystem::ToUTF8(Tests::GetTestExecutablePath()), "--wait-timeout", "20000" });
		REQUIRE_MESSAGE(run.ExitCode == ExitCode::Success, run.ErrorOutput);
		const nlohmann::json printed = JsonRpc::Parse(run.Output).value();
		CHECK(printed["ProjectPath"] == "");
		CHECK(printed["Headless"] == true);

		const CliRun info = Run({ "call", "editor.info" });
		REQUIRE_MESSAGE(info.ExitCode == ExitCode::Success, info.ErrorOutput);
		CHECK(JsonRpc::Parse(info.Output).value()["NoGpu"] == true);

		const CliRun quit = Run({ "call", "editor.quit" });
		CHECK(quit.ExitCode == ExitCode::Success);
		const std::filesystem::path sessionFile = EditorSession::GetSessionFilePath(environment.SessionDirectory, printed["ProcessId"].get<uint32_t>());
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
