#if defined(STRATA_TESTS_HAVE_CLI)

#include <doctest/doctest.h>

#include "CLI/EditorLauncher.h"
#include "Network/NetworkTestHelpers.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Process.h>
#include <Strata/Network/EditorSession.h>
#include <Strata/Network/JsonRpc.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// End-to-end tests of editor automation: the real StrataEditor (headless, without a GPU) driven by the real StrataCLI and
// its MCP server, as an AI agent would. CTest runs them as StrataEditor.Automation and passes the executables' paths in
// STRATA_TEST_EDITOR_PATH and STRATA_TEST_CLI_PATH; run directly, the tests look next to the test executable.

using namespace Strata;

namespace
{

	constexpr std::chrono::milliseconds c_ProcessTimeout = std::chrono::milliseconds(60000);
	constexpr std::chrono::milliseconds c_EditorStartTimeout = std::chrono::milliseconds(60000);

	std::filesystem::path FindBuiltExecutable(const char* variable, const std::string& name)
	{
		if (const std::optional<std::string> path = Platform::GetEnvVar(variable); path && !path->empty())
			return FileSystem::FromUTF8(*path);
#if defined(ST_PLATFORM_WINDOWS)
		return Platform::GetExecutableDirectory() / FileSystem::FromUTF8(name + ".exe");
#else
		return Platform::GetExecutableDirectory() / FileSystem::FromUTF8(name);
#endif
	}

	std::filesystem::path GetEditorPath()
	{
		return FindBuiltExecutable("STRATA_TEST_EDITOR_PATH", "StrataEditor");
	}

	std::filesystem::path GetCliPath()
	{
		return FindBuiltExecutable("STRATA_TEST_CLI_PATH", "StrataCLI");
	}

	// A private session directory for the editors and clients of one test, isolated from real editors.
	struct EndToEndEnvironment
	{
		explicit EndToEndEnvironment(const std::string& name)
			: Root(Tests::CreateTemporaryDirectory(name)), SessionDirectory(Root / "Sessions"),
			SessionOverride("STRATA_SESSION_DIR", FileSystem::ToUTF8(SessionDirectory)), PortOverride("STRATA_EDITOR_PORT", ""),
			TokenOverride("STRATA_EDITOR_TOKEN", ""), EditorOverride("STRATA_EDITOR_PATH", "")
		{
		}

		std::filesystem::path Root;
		std::filesystem::path SessionDirectory;
		Tests::ScopedEnvironmentVariable SessionOverride;
		Tests::ScopedEnvironmentVariable PortOverride;
		Tests::ScopedEnvironmentVariable TokenOverride;
		Tests::ScopedEnvironmentVariable EditorOverride;
	};

	bool HasBuiltExecutables()
	{
		const bool found = FileSystem::IsRegularFile(GetEditorPath()) && FileSystem::IsRegularFile(GetCliPath());
		INFO("StrataEditor: ", FileSystem::ToUTF8(GetEditorPath()), ", StrataCLI: ", FileSystem::ToUTF8(GetCliPath()));
		CHECK_MESSAGE(found, "The end-to-end tests need the built StrataEditor and StrataCLI executables");
		return found;
	}

	// A running editor process. A test that fails before the editor quits terminates it, so nothing is left behind.
	class EditorProcess
	{
	public:
		EditorProcess() = default;
		~EditorProcess()
		{
			if (m_Process.IsRunning())
				m_Process.Terminate();
		}

		EditorProcess(const EditorProcess&) = delete;
		EditorProcess& operator=(const EditorProcess&) = delete;

		bool Start(const std::vector<std::string>& arguments)
		{
			ProcessSpecification specification;
			specification.Executable = GetEditorPath();
			specification.Arguments = arguments;
			specification.Output = ProcessOutputMode::CaptureSeparate;
			const bool started = m_Process.Start(specification);
			CHECK_MESSAGE(started, m_Process.GetLastError());
			return started;
		}

		// Waits until the editor's session accepts authenticated connections.
		std::optional<EditorSessionInfo> WaitForSession(const std::filesystem::path& sessionDirectory)
		{
			std::string error;
			std::optional<EditorSessionInfo> session = CLI::WaitForEditorSession(m_Process.GetProcessID(), sessionDirectory, c_EditorStartTimeout,
				[this]() { return m_Process.IsRunning(); }, &error);
			CHECK_MESSAGE(session.has_value(), error, "\nEditor output:\n", TakeOutput());
			return session;
		}

		std::optional<int> Wait(std::chrono::milliseconds timeout = c_ProcessTimeout)
		{
			return m_Process.Wait(timeout);
		}

		// Ends the editor at once, as a crash would (TerminateProcess, SIGKILL): nothing of it runs any more.
		void Kill()
		{
			CHECK_MESSAGE(m_Process.Terminate(), m_Process.GetLastError());
		}

		// Everything the editor printed so far (its log), for failure messages.
		std::string TakeOutput()
		{
			m_Output += m_Process.TakeOutput();
			m_Output += m_Process.TakeErrorOutput();
			return m_Output;
		}

		// Whether the editor printed text. The output readers may still be appending the last lines after the process
		// has exited, so this waits a little for them.
		bool OutputContains(const std::string& text)
		{
			return Tests::WaitUntil([&]() { return TakeOutput().find(text) != std::string::npos; }, std::chrono::milliseconds(5000));
		}

		uint32_t GetProcessId() const { return m_Process.GetProcessID(); }
	private:
		Process m_Process;
		std::string m_Output;
	};

	struct CliResult
	{
		int ExitCode = -1;
		std::string Output;
		std::string ErrorOutput;
		nlohmann::json Json;  // The parsed output (null unless it is JSON)
		nlohmann::json Error; // The parsed error output (null unless it is JSON)
	};

	CliResult RunCli(const std::vector<std::string>& arguments)
	{
		ProcessSpecification specification;
		specification.Executable = GetCliPath();
		specification.Arguments = arguments;
		specification.Output = ProcessOutputMode::CaptureSeparate;
		const Process::RunResult run = Process::Run(specification, c_ProcessTimeout);
		CHECK_MESSAGE(run.Started, run.Error);
		CHECK_FALSE(run.TimedOut);

		CliResult result;
		result.ExitCode = run.ExitCode;
		result.Output = run.Output;
		result.ErrorOutput = run.ErrorOutput;
		result.Json = JsonRpc::Parse(run.Output).value_or(nlohmann::json());
		result.Error = JsonRpc::Parse(run.ErrorOutput).value_or(nlohmann::json());
		return result;
	}

	// `StrataCLI call <method> <params>`; the call must succeed. Returns the result.
	nlohmann::json Call(const std::string& method, const nlohmann::json& params = nlohmann::json::object())
	{
		const CliResult result = RunCli({ "call", method, params.dump() });
		INFO(method, " ", params.dump(), "\nstdout: ", result.Output, "\nstderr: ", result.ErrorOutput);
		CHECK(result.ExitCode == 0);
		return result.Json;
	}

	// `StrataCLI call <method> <params>`; the editor must answer with an error. Returns the error object.
	nlohmann::json CallError(const std::string& method, const nlohmann::json& params = nlohmann::json::object())
	{
		const CliResult result = RunCli({ "call", method, params.dump() });
		INFO(method, " ", params.dump(), "\nstdout: ", result.Output, "\nstderr: ", result.ErrorOutput);
		CHECK(result.ExitCode == 1); // The editor answered with an error
		CHECK(result.Output.empty());
		return result.Error;
	}

	bool ContainsLine(const std::string& text, const std::string& start)
	{
		size_t position = 0;
		while (position < text.size())
		{
			const size_t end = text.find('\n', position);
			if (text.compare(position, start.size(), start) == 0)
				return true;
			if (end == std::string::npos)
				break;
			position = end + 1;
		}
		return false;
	}

	// Talks to `StrataCLI mcp` over its standard input and output, as an MCP client does.
	class McpProcess
	{
	public:
		~McpProcess()
		{
			if (m_Process.IsRunning())
				m_Process.Terminate();
		}

		bool Start()
		{
			ProcessSpecification specification;
			specification.Executable = GetCliPath();
			specification.Arguments = { "mcp" };
			specification.Output = ProcessOutputMode::CaptureSeparate;
			specification.PipeInput = true;
			const bool started = m_Process.Start(specification);
			CHECK_MESSAGE(started, m_Process.GetLastError());
			return started;
		}

		bool Send(const nlohmann::json& message)
		{
			return m_Process.WriteInput(JsonRpc::Serialize(message) + "\n");
		}

		// Sends a request and waits for its response (null if none arrives in time).
		nlohmann::json Request(const std::string& method, const nlohmann::json& params = nlohmann::json::object())
		{
			const int64_t id = m_NextId++;
			if (!Send(JsonRpc::MakeRequest(id, method, params)))
				return nullptr;

			const auto deadline = std::chrono::steady_clock::now() + c_ProcessTimeout;
			while (std::chrono::steady_clock::now() < deadline)
			{
				m_Buffer += m_Process.TakeOutput();
				size_t end = 0;
				while ((end = m_Buffer.find('\n')) != std::string::npos)
				{
					const std::string line = m_Buffer.substr(0, end);
					m_Buffer.erase(0, end + 1);
					std::optional<nlohmann::json> message = JsonRpc::Parse(line);
					if (message && message->is_object() && message->contains("id") && (*message)["id"] == id)
						return *message;
				}
				if (!m_Process.IsRunning())
					break;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return nullptr;
		}

		nlohmann::json CallTool(const std::string& name, const nlohmann::json& arguments = nlohmann::json::object())
		{
			const nlohmann::json response = Request("tools/call", { { "name", name }, { "arguments", arguments } });
			INFO(name, " -> ", response.dump(), "\nstderr: ", m_Process.TakeErrorOutput());
			CHECK(response.contains("result"));
			return response.contains("result") ? response["result"] : nlohmann::json();
		}

		// Ends the input; the server exits once it has handled everything.
		std::optional<int> Finish()
		{
			m_Process.CloseInput();
			return m_Process.Wait(c_ProcessTimeout);
		}
	private:
		Process m_Process;
		std::string m_Buffer;
		int64_t m_NextId = 1;
	};

	std::string GetToolText(const nlohmann::json& toolResult)
	{
		std::string text;
		if (toolResult.contains("content"))
		{
			for (const nlohmann::json& content : toolResult["content"])
			{
				if (content.value("type", "") == "text")
					text += content.value("text", "");
			}
		}
		return text;
	}

}

TEST_SUITE("EndToEnd.EditorAutomation")
{
	TEST_CASE("An agent builds, plays, saves and exports a game through StrataCLI")
	{
		if (!HasBuiltExecutables())
			return;
		EndToEndEnvironment environment("EndToEndCli");
		const std::filesystem::path projectDirectory = environment.Root / "Tetris";
		const std::filesystem::path buildDirectory = environment.Root / "Build";

		// A headless editor without a GPU, serving automation on a free port, and found through its session file.
		EditorProcess editor;
		REQUIRE(editor.Start({ "--no-gpu" }));
		const std::optional<EditorSessionInfo> session = editor.WaitForSession(environment.SessionDirectory);
		REQUIRE(session.has_value());
		CHECK(session->Headless);
		CHECK(session->ProjectPath.empty());

		// Discovery: the commands, with descriptions and parameter schemas.
		const CliResult list = RunCli({ "list" });
		REQUIRE(list.ExitCode == 0);
		for (const char* method : { "entity.create", "component.set", "editor.wait", "editor.status", "editor.quit", "project.export" })
			CHECK_MESSAGE(ContainsLine(list.Output, method), method);
		const CliResult schemas = RunCli({ "list", "--json" });
		REQUIRE(schemas.ExitCode == 0);
		const nlohmann::json& methods = schemas.Json["methods"];
		const auto entityCreate = std::find_if(methods.begin(), methods.end(), [](const nlohmann::json& method) { return method["name"] == "entity.create"; });
		REQUIRE(entityCreate != methods.end());
		CHECK((*entityCreate)["paramsSchema"]["properties"].contains("name"));
		CHECK_FALSE((*entityCreate)["description"].get<std::string>().empty());

		nlohmann::json status = Call("editor.status", { { "sections", { "automation", "editor" } } });
		CHECK(status["project"]["open"] == false);
		CHECK(status["automation"]["port"] == session->Port);
		CHECK(status["editor"]["headless"] == true);
		CHECK(status["editor"]["graphicsDevice"] == false);

		// Build a scene.
		Call("project.create", { { "directory", FileSystem::ToUTF8(projectDirectory) }, { "name", "Tetris" } });
		// The session follows the project before the answer arrives: the very next call finds the editor by it.
		const CliResult byNewProject = RunCli({ "call", "editor.status", "--project", FileSystem::ToUTF8(projectDirectory) });
		REQUIRE_MESSAGE(byNewProject.ExitCode == 0, byNewProject.ErrorOutput);
		CHECK(byNewProject.Json["project"]["name"] == "Tetris");
		const nlohmann::json board = Call("entity.create", { { "name", "Board" }, { "components", { { "Transform", { { "Translation", { 0, 1, 0 } } } } } } });
		const std::string boardId = board["id"].get<std::string>();
		Call("component.add", { { "entity", boardId }, { "component", "MeshRenderer" }, { "values", { { "Mesh", "Builtin/Cube" } } } });
		Call("component.set", { { "entity", boardId }, { "component", "Transform" }, { "values", { { "Scale", { 2, 2, 2 } } } } });
		CHECK(Call("component.get", { { "entity", boardId }, { "component", "Transform" } })["values"]["Scale"] == nlohmann::json { 2.0, 2.0, 2.0 });

		// Deferred commands answer once their frames have passed.
		CHECK(Call("editor.wait", { { "frames", 10 } })["frames"] == 10);

		// Play: changes apply to the running copy, with a warning, and play.stop discards them.
		CHECK(Call("play.start")["state"] == "Play");
		const nlohmann::json temporary = Call("entity.create", { { "name", "Temporary" } });
		CHECK(temporary.contains("warning"));
		Call("editor.wait", { { "frames", 5 } });
		CHECK(Call("play.stop")["state"] == "Edit");
		CHECK(Call("entity.find", { { "name", "Temporary" } })["entities"].empty());

		// Undo the last edit.
		const nlohmann::json undone = Call("edit.undo");
		CHECK(undone["redo"].get<std::string>().rfind("Set Scale of ", 0) == 0); // The step names what it changed
		CHECK(Call("component.get", { { "entity", boardId }, { "component", "Transform" } })["values"]["Scale"] == nlohmann::json { 1.0, 1.0, 1.0 });

		// Errors come back as JSON-RPC errors on stderr, with exit code 1.
		const nlohmann::json invalid = CallError("entity.get", { { "entity", "00000000DEADBEEF" } });
		CHECK(invalid["code"] == JsonRpc::ErrorCode::InvalidParams);
		CHECK(invalid["data"]["command"] == "entity.get");
		CHECK(invalid["data"]["parameters"]["properties"].contains("entity"));
		CHECK(CallError("no.such.command")["code"] == JsonRpc::ErrorCode::MethodNotFound);
		const nlohmann::json unsaved = CallError("editor.quit");
		CHECK(unsaved["code"] == JsonRpc::ErrorCode::OperationFailed);
		CHECK(unsaved["message"].get<std::string>().find("unsaved changes") != std::string::npos);

		// Save, then export a playable game.
		Call("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
		Call("project.setStartScene", { { "scene", "Scenes/Main.stscene" } });
		const nlohmann::json exported = Call("project.export", { { "directory", FileSystem::ToUTF8(buildDirectory) }, { "includeRuntime", false } });
		CHECK(FileSystem::IsRegularFile(FileSystem::FromUTF8(exported["manifest"].get<std::string>())));
		CHECK(FileSystem::IsRegularFile(FileSystem::FromUTF8(exported["assetPack"].get<std::string>())));
		CHECK(exported["assetCount"].get<int>() > 0);

		// The session followed the project: the editor is found by its project directory.
		const CliResult byProject = RunCli({ "call", "editor.status", "--project", FileSystem::ToUTF8(projectDirectory) });
		REQUIRE_MESSAGE(byProject.ExitCode == 0, byProject.ErrorOutput);
		CHECK(byProject.Json["project"]["name"] == "Tetris");
		CHECK(byProject.Json["scene"]["modified"] == false);
		CHECK(RunCli({ "status" }).Json["connected"] == true);

		// Quit: answered first, then the editor closes cleanly and removes its session.
		CHECK(Call("editor.quit")["quitting"] == true);
		const std::optional<int> exitCode = editor.Wait();
		REQUIRE_MESSAGE(exitCode.has_value(), editor.TakeOutput());
		CHECK_MESSAGE(*exitCode == 0, editor.TakeOutput());
		CHECK(EditorSession::FindSessions(environment.SessionDirectory).empty());
		CHECK_FALSE(FileSystem::Exists(EditorSession::GetProjectSessionFilePath(projectDirectory)));
		CHECK(editor.OutputContains("Automation: #")); // Requests are logged
	}

	TEST_CASE("The MCP server exposes the editor's commands as tools")
	{
		if (!HasBuiltExecutables())
			return;
		EndToEndEnvironment environment("EndToEndMcp");

		EditorProcess editor;
		REQUIRE(editor.Start({ "--no-gpu", "--automation-port", "0" }));
		REQUIRE(editor.WaitForSession(environment.SessionDirectory).has_value());

		McpProcess mcp;
		REQUIRE(mcp.Start());
		const nlohmann::json initialized = mcp.Request("initialize", {
			{ "protocolVersion", "2025-06-18" },
			{ "capabilities", nlohmann::json::object() },
			{ "clientInfo", { { "name", "StrataTests" }, { "version", "1.0" } } } });
		REQUIRE(initialized.contains("result"));
		CHECK(initialized["result"]["serverInfo"]["name"] == "strata");
		REQUIRE(mcp.Send(JsonRpc::MakeNotification("notifications/initialized", nullptr)));

		const nlohmann::json listing = mcp.Request("tools/list");
		REQUIRE(listing.contains("result"));
		std::vector<std::string> names;
		const nlohmann::json* entityCreate = nullptr;
		for (const nlohmann::json& tool : listing["result"]["tools"])
		{
			names.push_back(tool["name"].get<std::string>());
			if (tool["name"] == "entity_create")
				entityCreate = &tool;
		}
		// The core toolset by default: the commands a game is made and tested with; the others through strata_call, by their
		// tool names, or listed once strata_toolsets enables their toolsets.
		for (const char* name : { "strata_status", "strata_call", "strata_describe", "strata_toolsets", "entity_create", "editor_status", "play_step",
			"input_sequence" })
			CHECK_MESSAGE(std::find(names.begin(), names.end(), name) != names.end(), std::string(name));
		for (const char* name : { "editor_wait", "editor_quit", "strata_list_methods" })
			CHECK_MESSAGE(std::find(names.begin(), names.end(), name) == names.end(), std::string(name));
		CHECK(names.size() <= 32);
		REQUIRE(entityCreate != nullptr);
		CHECK((*entityCreate)["inputSchema"]["type"] == "object");
		CHECK((*entityCreate)["inputSchema"]["properties"].contains("components"));

		const nlohmann::json created = mcp.CallTool("entity_create", { { "name", "FromMcp" } });
		CHECK(created["isError"] == false);
		CHECK(created["structuredContent"]["id"].is_string());
		const nlohmann::json waited = mcp.CallTool("editor_wait", { { "frames", 3 } });
		CHECK(waited["structuredContent"]["frames"] == 3);
		const nlohmann::json found = mcp.CallTool("strata_call", { { "method", "entity.find" }, { "params", { { "name", "FromMcp" } } } });
		CHECK(found["structuredContent"]["entities"].size() == 1);

		const nlohmann::json invalid = mcp.CallTool("entity_get", { { "entity", "not an id" } });
		CHECK(invalid["isError"] == true);
		CHECK(GetToolText(invalid).find("-32602") != std::string::npos);

		const nlohmann::json quit = mcp.CallTool("editor_quit", { { "force", true } });
		CHECK(quit["isError"] == false);
		CHECK(quit["structuredContent"]["discardedChanges"] == true);

		const std::optional<int> mcpExitCode = mcp.Finish();
		REQUIRE(mcpExitCode.has_value());
		CHECK(*mcpExitCode == 0);
		const std::optional<int> editorExitCode = editor.Wait();
		REQUIRE_MESSAGE(editorExitCode.has_value(), editor.TakeOutput());
		CHECK_MESSAGE(*editorExitCode == 0, editor.TakeOutput());
		CHECK(EditorSession::FindSessions(environment.SessionDirectory).empty());
	}

	TEST_CASE("StrataCLI launch opens a project that was never imported, with its start scene")
	{
		if (!HasBuiltExecutables())
			return;
		EndToEndEnvironment environment("EndToEndLaunchFresh");
		// A copy of a sample as an agent makes one: no .strata, so every asset imports in the background as the editor opens
		// it, and the start scene uses some of them (its sounds).
		std::string error;
		const std::filesystem::path projectDirectory = Tests::CopySampleProject("Tetris", &error);
		REQUIRE_MESSAGE(!projectDirectory.empty(), error);
		REQUIRE_FALSE(FileSystem::Exists(projectDirectory / ".strata"));

		const auto start = std::chrono::steady_clock::now();
		const CliResult launched = RunCli({ "launch", "--no-gpu", "--project", FileSystem::ToUTF8(projectDirectory), "--idle-timeout", "60" });
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		REQUIRE_MESSAGE(launched.ExitCode == 0, launched.ErrorOutput);
		CHECK_MESSAGE(seconds < 30.0, "Launching took ", seconds, " s");

		const CliResult status = RunCli({ "call", "editor.status", R"({"sections": ["sceneLoad"]})", "--project", FileSystem::ToUTF8(projectDirectory) });
		REQUIRE_MESSAGE(status.ExitCode == 0, status.ErrorOutput);
		CHECK(status.Json["scene"]["scene"] == "BD75622ED18E84E4"); // Tetris.stproj's start scene
		CHECK(status.Json["scene"]["entityCount"].get<int>() > 0);
		CHECK(status.Json["sceneLoad"].is_null());

		const CliResult quit = RunCli({ "call", "editor.quit", "--project", FileSystem::ToUTF8(projectDirectory) });
		REQUIRE_MESSAGE(quit.ExitCode == 0, quit.ErrorOutput);
		CHECK(Tests::WaitUntil([&]() { return EditorSession::FindSessions(environment.SessionDirectory).empty(); }, c_ProcessTimeout));
	}

	TEST_CASE("Editor automation options are checked at start-up")
	{
		if (!HasBuiltExecutables())
			return;
		EndToEndEnvironment environment("EndToEndOptions");

		// An invalid port ends the start-up.
		{
			EditorProcess editor;
			REQUIRE(editor.Start({ "--no-gpu", "--automation-port", "70000" }));
			const std::optional<int> exitCode = editor.Wait();
			REQUIRE(exitCode.has_value());
			CHECK(*exitCode != 0);
			CHECK(editor.OutputContains("--automation-port"));
		}

		// With an idle timeout, an editor nobody connects to closes itself, cleanly.
		{
			EditorProcess editor;
			REQUIRE(editor.Start({ "--no-gpu", "--idle-timeout", "1" }));
			const std::optional<int> exitCode = editor.Wait();
			REQUIRE(exitCode.has_value());
			CHECK_MESSAGE(*exitCode == 0, editor.TakeOutput());
			CHECK(editor.OutputContains("--idle-timeout"));
			CHECK(EditorSession::FindSessions(environment.SessionDirectory).empty());

			EditorProcess invalid;
			REQUIRE(invalid.Start({ "--no-gpu", "--idle-timeout", "-5" }));
			const std::optional<int> invalidExitCode = invalid.Wait();
			REQUIRE(invalidExitCode.has_value());
			CHECK(*invalidExitCode != 0);
		}

		// Without automation, a scripted run publishes no session.
		{
			EditorProcess editor;
			REQUIRE(editor.Start({ "--no-gpu", "--no-automation", "--frames", "3" }));
			const std::optional<int> exitCode = editor.Wait();
			REQUIRE(exitCode.has_value());
			CHECK_MESSAGE(*exitCode == 0, editor.TakeOutput());
			CHECK(EditorSession::FindSessions(environment.SessionDirectory).empty());
		}

		// --help and --version answer and exit 0 without starting anything (no window, no session); a command line the editor
		// does not understand exits 3 with the usage.
		{
			EditorProcess help;
			REQUIRE(help.Start({ "--help" }));
			const std::optional<int> helpExit = help.Wait();
			REQUIRE(helpExit.has_value());
			CHECK_MESSAGE(*helpExit == 0, help.TakeOutput());
			CHECK(help.OutputContains("Usage: StrataEditor"));
			CHECK(help.OutputContains("--quit-after-commands"));
			CHECK(EditorSession::FindSessions(environment.SessionDirectory).empty());

			EditorProcess version;
			REQUIRE(version.Start({ "--version" }));
			const std::optional<int> versionExit = version.Wait();
			REQUIRE(versionExit.has_value());
			CHECK(*versionExit == 0);
			CHECK(version.OutputContains("Strata Editor "));

			// Values that are not what an option takes too: a run that would ignore them might never end.
			for (const std::vector<std::string>& arguments : { std::vector<std::string> { "--no-gpu", "--bogus" }, std::vector<std::string> { "stray" },
				std::vector<std::string> { "--no-gpu", "--frames" }, std::vector<std::string> { "--no-gpu=1" }, std::vector<std::string> { "--no-gpu", "--frames", "abc" },
				std::vector<std::string> { "--no-gpu", "--frames=0" }, std::vector<std::string> { "--no-gpu", "--ui-scale", "-1" },
				std::vector<std::string> { "--no-gpu", "--automation-port", "99999" }, std::vector<std::string> { "--no-gpu", "--idle-timeout", "x" } })
			{
				CAPTURE(arguments[arguments.size() - 1]);
				EditorProcess wrong;
				REQUIRE(wrong.Start(arguments));
				const std::optional<int> wrongExit = wrong.Wait();
				REQUIRE(wrongExit.has_value());
				CHECK(*wrongExit == 3);
				CHECK(wrong.OutputContains("Usage: StrataEditor"));
			}
			CHECK(EditorSession::FindSessions(environment.SessionDirectory).empty());
		}

		// A headless editor that would run until told to quit, but whose session cannot be published, exits with an error
		// instead of running unreachable forever.
		{
			const std::filesystem::path notADirectory = environment.Root / "NotADirectory";
			REQUIRE(FileSystem::WriteText(notADirectory, "x"));
			Tests::ScopedEnvironmentVariable unusableSessions("STRATA_SESSION_DIR", FileSystem::ToUTF8(notADirectory));
			EditorProcess editor;
			REQUIRE(editor.Start({ "--no-gpu" }));
			const std::optional<int> exitCode = editor.Wait();
			REQUIRE(exitCode.has_value());
			CHECK(*exitCode == 1);
			CHECK(editor.OutputContains("needs automation"));
		}
	}
}

TEST_SUITE("EndToEnd.Recovery")
{
	TEST_CASE("An editor killed with unsaved changes leaves them to the next editor, which recovers them")
	{
		if (!HasBuiltExecutables())
			return;
		EndToEndEnvironment environment("EndToEndRecovery");
		const std::filesystem::path projectDirectory = environment.Root / "Game";

		std::string crate;
		nlohmann::json savedEntities;
		{
			EditorProcess editor;
			REQUIRE(editor.Start({ "--no-gpu" }));
			REQUIRE(editor.WaitForSession(environment.SessionDirectory).has_value());
			Call("project.create", { { "directory", FileSystem::ToUTF8(projectDirectory) }, { "name", "Game" }, { "template", "basic3d" } });
			savedEntities = Call("scene.hierarchy")["entities"];

			// An agent edits the saved scene.
			crate = Call("entity.create", { { "name", "Crate" }, { "components", { { "Transform", { { "Translation", { 1, 2, 3 } } } } } } })["id"].get<std::string>();
			Call("component.set", { { "entity", crate }, { "component", "Transform" }, { "values", { { "Translation", { 4, 5, 6 } } } } });
			// The scene moves meanwhile (its file stays the same): the changes still belong to it.
			Call("asset.move", { { "asset", "Scenes/Main.stscene" }, { "path", "Levels/Moved.stscene" } });
			Call("entity.rename", { { "entity", crate }, { "name", "Big Crate" } });
			REQUIRE(Call("editor.status")["scene"]["modified"] == true);

			// The journal reaches the disk a few seconds after the edits stop.
			const bool journaled = Tests::WaitUntil([]()
			{
				const nlohmann::json journal = Call("editor.status", { { "sections", { "recovery" } } })["recovery"]["journal"];
				return journal["lastWrite"].get<int64_t>() > 0 && journal["pending"] == false && journal["busy"] == false;
			}, std::chrono::seconds(30));
			REQUIRE(journaled);

			// The editor dies without closing anything.
			editor.Kill();
			CHECK(editor.Wait().has_value());
		}

		EditorProcess editor;
		REQUIRE(editor.Start({ "--no-gpu", "--project", FileSystem::ToUTF8(projectDirectory) }));
		REQUIRE(editor.WaitForSession(environment.SessionDirectory).has_value());
		const nlohmann::json status = Call("scene.recoveryStatus");
		REQUIRE(status["entries"].size() == 1);
		const nlohmann::json entry = status["entries"][0];
		CHECK(entry["sceneName"] == "Main");
		CHECK(entry["scenePath"] == "Levels/Moved.stscene");
		CHECK(entry["discarded"] == false); // Left behind by a crash
		CHECK(entry["applicable"] == true);
		CHECK(Call("editor.status", { { "sections", { "recovery" } } })["recovery"]["applicable"] == 1);
		CHECK(Call("scene.hierarchy")["entities"] == savedEntities);

		const nlohmann::json recovered = Call("scene.recover", { { "scene", entry["id"] }, { "action", "apply" } });
		CHECK(recovered["recovered"] == entry["id"]);
		const nlohmann::json crateEntity = Call("entity.get", { { "entity", crate } });
		CHECK(crateEntity["name"] == "Big Crate");
		CHECK(crateEntity["components"]["Transform"]["Translation"] == nlohmann::json::array({ 4, 5, 6 }));
		CHECK(Call("editor.status")["scene"]["modified"] == true);

		// One undo step back to the saved file.
		Call("edit.undo");
		CHECK(Call("scene.hierarchy")["entities"] == savedEntities);
		CHECK(Call("editor.status")["scene"]["modified"] == false);
		Call("editor.quit", { { "force", true } });
		CHECK(editor.Wait() == 0);
	}
}

#endif
