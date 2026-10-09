#include "CLI/CliApplication.h"

#include "CLI/EditorLauncher.h"
#include "CLI/McpServer.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Version.h"
#include "Strata/Network/JsonRpc.h"
#include "Strata/Network/Socket.h"

#include <spdlog/sinks/stdout_sinks.h>

#include <algorithm>
#include <charconv>
#include <istream>
#include <mutex>
#include <ostream>
#include <string_view>

namespace Strata::CLI
{

	namespace
	{

		constexpr std::chrono::milliseconds c_DefaultCallTimeout = std::chrono::milliseconds(30000);
		constexpr const char* c_EditorPortVariable = "STRATA_EDITOR_PORT";
		constexpr const char* c_EditorTokenVariable = "STRATA_EDITOR_TOKEN";

		struct OptionDefinition
		{
			std::string_view Name;
			bool TakesValue = false;
		};

		constexpr OptionDefinition c_Options[] = {
			{ "--project", true },
			{ "--editor", true },
			{ "--host", true },
			{ "--port", true },
			{ "--token", true },
			{ "--timeout", true },
			{ "--wait-timeout", true },
			{ "--headless", false },
			{ "--json", false },
			{ "--verbose", false },
			{ "--help", false },
			{ "-h", false },
			{ "--version", false }
		};

		std::optional<int64_t> ParseInteger(std::string_view text)
		{
			int64_t value = 0;
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
			if (text.empty() || error != std::errc() || end != text.data() + text.size())
				return std::nullopt;
			return value;
		}

		std::optional<uint16_t> ParsePort(std::string_view text)
		{
			const std::optional<int64_t> value = ParseInteger(text);
			if (!value || *value < 1 || *value > UINT16_MAX)
				return std::nullopt;
			return static_cast<uint16_t>(*value);
		}

		std::filesystem::path ToAbsolutePath(const std::string& utf8Path)
		{
			const std::filesystem::path path = FileSystem::FromUTF8(utf8Path);
			std::error_code error;
			std::filesystem::path absolute = std::filesystem::absolute(path, error);
			return (error ? path : absolute).lexically_normal();
		}

		std::string PrettyPrint(const nlohmann::json& value)
		{
			return value.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
		}

		int ReportCallFailure(const RpcResult& result, std::ostream& errorOutput)
		{
			const RpcError& error = result.GetError();
			nlohmann::json description = nlohmann::json { { "code", error.Code }, { "message", error.Message } };
			if (!error.Data.is_null())
				description["data"] = error.Data;
			errorOutput << PrettyPrint(description) << "\n";

			const bool transportFailure = error.Code == JsonRpc::ErrorCode::ConnectionClosed || error.Code == JsonRpc::ErrorCode::Timeout;
			return transportFailure ? ExitCode::ConnectionFailure : ExitCode::RpcError;
		}

		int ReportUsageError(const std::string& message, std::ostream& errorOutput)
		{
			errorOutput << "error: " << message << "\n\nRun 'StrataCLI --help' for usage.\n";
			return ExitCode::UsageError;
		}

		std::chrono::milliseconds GetCallTimeout(const CliArguments& arguments)
		{
			return arguments.TimeoutMilliseconds ? std::chrono::milliseconds(*arguments.TimeoutMilliseconds) : c_DefaultCallTimeout;
		}

		// Connects for a one-shot command, printing the reason on failure.
		bool ConnectForCommand(EditorConnection& connection, std::ostream& errorOutput)
		{
			if (connection.EnsureConnected())
				return true;
			errorOutput << "error: " << connection.GetLastError() << "\n";
			return false;
		}

		int RunCallCommand(const CliArguments& arguments, std::ostream& output, std::ostream& errorOutput)
		{
			if (arguments.Positionals.empty() || arguments.Positionals.size() > 2)
				return ReportUsageError("'call' expects <method> [params-json]", errorOutput);

			nlohmann::json params = nlohmann::json::object();
			if (arguments.Positionals.size() == 2)
			{
				std::optional<nlohmann::json> parsed = JsonRpc::Parse(arguments.Positionals[1]);
				if (!parsed || (!parsed->is_object() && !parsed->is_array()))
					return ReportUsageError("params must be a JSON object or array", errorOutput);
				params = std::move(*parsed);
			}

			std::string error;
			std::optional<EditorConnectionOptions> options = BuildConnectionOptions(arguments, error);
			if (!options)
				return ReportUsageError(error, errorOutput);

			EditorConnection connection(std::move(*options));
			if (!ConnectForCommand(connection, errorOutput))
				return ExitCode::ConnectionFailure;

			const RpcResult result = connection.Call(arguments.Positionals[0], params, GetCallTimeout(arguments));
			if (result.IsError())
				return ReportCallFailure(result, errorOutput);

			output << PrettyPrint(result.GetValue()) << "\n";
			return ExitCode::Success;
		}

		int RunListCommand(const CliArguments& arguments, std::ostream& output, std::ostream& errorOutput)
		{
			if (!arguments.Positionals.empty())
				return ReportUsageError("'list' takes no arguments", errorOutput);

			std::string error;
			std::optional<EditorConnectionOptions> options = BuildConnectionOptions(arguments, error);
			if (!options)
				return ReportUsageError(error, errorOutput);

			EditorConnection connection(std::move(*options));
			if (!ConnectForCommand(connection, errorOutput))
				return ExitCode::ConnectionFailure;

			const RpcResult result = connection.Call("rpc.listMethods", nlohmann::json::object(), GetCallTimeout(arguments));
			if (result.IsError())
				return ReportCallFailure(result, errorOutput);

			if (arguments.Json)
			{
				output << PrettyPrint(result.GetValue()) << "\n";
				return ExitCode::Success;
			}

			const nlohmann::json& value = result.GetValue();
			const auto methods = value.is_object() ? value.find("methods") : value.end();
			if (methods == value.end() || !methods->is_array())
			{
				errorOutput << "error: the editor returned a malformed method list\n";
				return ExitCode::RpcError;
			}

			std::vector<std::pair<std::string, std::string>> rows;
			size_t nameWidth = 0;
			for (const nlohmann::json& method : *methods)
			{
				const auto name = method.is_object() ? method.find("name") : method.end();
				if (name == method.end() || !name->is_string())
					continue;
				const auto description = method.find("description");
				rows.emplace_back(name->get<std::string>(), description != method.end() && description->is_string() ? description->get<std::string>() : std::string());
				nameWidth = std::max(nameWidth, rows.back().first.size());
			}

			for (const auto& [name, description] : rows)
				output << name << std::string(nameWidth - name.size() + 2, ' ') << description << "\n";
			return ExitCode::Success;
		}

		int RunStatusCommand(const CliArguments& arguments, std::ostream& output, std::ostream& errorOutput)
		{
			if (!arguments.Positionals.empty())
				return ReportUsageError("'status' takes no arguments", errorOutput);

			std::string error;
			std::optional<EditorConnectionOptions> options = BuildConnectionOptions(arguments, error);
			if (!options)
				return ReportUsageError(error, errorOutput);

			EditorConnection connection(std::move(*options));
			const bool connected = connection.EnsureConnected();

			nlohmann::json status = connection.DescribeStatus();
			nlohmann::json sessions = nlohmann::json::array();
			if (const std::optional<std::filesystem::path> sessionDirectory = connection.GetSessionDirectory())
			{
				for (const EditorSessionInfo& session : EditorSession::FindSessions(*sessionDirectory))
					sessions.push_back(DescribeSession(session));
			}
			status["knownSessions"] = std::move(sessions);

			output << PrettyPrint(status) << "\n";
			return connected ? ExitCode::Success : ExitCode::ConnectionFailure;
		}

		int RunLaunchCommand(const CliArguments& arguments, std::ostream& output, std::ostream& errorOutput)
		{
			if (!arguments.Positionals.empty())
				return ReportUsageError("'launch' takes no positional arguments", errorOutput);
			if (!arguments.Project)
				return ReportUsageError("'launch' requires --project <dir>", errorOutput);

			EditorLaunchSpecification specification;
			specification.EditorPath = ResolveEditorPath(arguments.Editor);
			specification.ProjectDirectory = ToAbsolutePath(*arguments.Project);
			specification.Headless = arguments.Headless;
			if (arguments.WaitTimeoutMilliseconds)
				specification.WaitTimeout = std::chrono::milliseconds(*arguments.WaitTimeoutMilliseconds);

			const EditorLaunchResult result = LaunchEditor(specification);
			if (!result.Success)
			{
				errorOutput << "error: " << result.Error << "\n";
				return ExitCode::ConnectionFailure;
			}

			// The token stays in the private session file; printing it would expose it in logs and terminals.
			output << PrettyPrint(DescribeSession(result.Session)) << "\n";
			return ExitCode::Success;
		}

		int RunMcpCommand(const CliArguments& arguments, std::istream& input, std::ostream& output, std::ostream& errorOutput)
		{
			if (!arguments.Positionals.empty())
				return ReportUsageError("'mcp' takes no positional arguments", errorOutput);

			std::string error;
			std::optional<EditorConnectionOptions> options = BuildConnectionOptions(arguments, error);
			if (!options)
				return ReportUsageError(error, errorOutput);

			McpServerSpecification specification;
			specification.Connection = std::move(*options);
			specification.EditorPath = arguments.Editor;
			if (arguments.TimeoutMilliseconds)
				specification.CallTimeout = std::chrono::milliseconds(*arguments.TimeoutMilliseconds);
			if (arguments.WaitTimeoutMilliseconds)
				specification.LaunchTimeout = std::chrono::milliseconds(*arguments.WaitTimeoutMilliseconds);

			std::mutex outputMutex;
			McpServer server(std::move(specification), [&output, &outputMutex](const std::string& message)
			{
				std::scoped_lock<std::mutex> lock(outputMutex);
				output << message << '\n';
				output.flush();
			});

			ST_INFO("StrataCLI {} serving MCP on stdin/stdout", c_EngineVersion);
			RunMcpServer(server, input);
			return ExitCode::Success;
		}

	}

	std::optional<CliArguments> ParseCliArguments(const std::vector<std::string>& arguments, std::string& error)
	{
		CliArguments result;
		bool optionsEnded = false;
		for (size_t index = 0; index < arguments.size(); index++)
		{
			const std::string& argument = arguments[index];
			const bool isOption = !optionsEnded && argument.size() > 1 && argument[0] == '-';
			if (!isOption)
			{
				if (result.Command.empty())
					result.Command = argument;
				else
					result.Positionals.push_back(argument);
				continue;
			}

			if (argument == "--")
			{
				optionsEnded = true;
				continue;
			}

			std::string name = argument;
			std::optional<std::string> inlineValue;
			if (const size_t equals = argument.find('='); equals != std::string::npos && argument.starts_with("--"))
			{
				name = argument.substr(0, equals);
				inlineValue = argument.substr(equals + 1);
			}

			const OptionDefinition* definition = nullptr;
			for (const OptionDefinition& option : c_Options)
			{
				if (option.Name == name)
					definition = &option;
			}
			if (!definition)
			{
				error = fmt::format("unknown option '{}'", name);
				return std::nullopt;
			}

			std::string value;
			if (definition->TakesValue)
			{
				if (inlineValue)
					value = *inlineValue;
				else if (index + 1 < arguments.size())
					value = arguments[++index];
				else
				{
					error = fmt::format("option '{}' requires a value", name);
					return std::nullopt;
				}
			}
			else if (inlineValue)
			{
				error = fmt::format("option '{}' does not take a value", name);
				return std::nullopt;
			}

			if (name == "--project")
				result.Project = value;
			else if (name == "--editor")
				result.Editor = value;
			else if (name == "--host")
				result.Host = value;
			else if (name == "--token")
				result.Token = value;
			else if (name == "--port")
			{
				result.Port = ParsePort(value);
				if (!result.Port)
				{
					error = fmt::format("'{}' is not a valid port (1-65535)", value);
					return std::nullopt;
				}
			}
			else if (name == "--timeout" || name == "--wait-timeout")
			{
				std::optional<int64_t> milliseconds = ParseInteger(value);
				if (!milliseconds || *milliseconds <= 0)
				{
					error = fmt::format("{} expects a positive number of milliseconds, got '{}'", name, value);
					return std::nullopt;
				}
				// Longer waits are clamped like every socket timeout, which keeps deadline arithmetic from overflowing.
				milliseconds = std::min(*milliseconds, static_cast<int64_t>(c_MaxSocketTimeout.count()));
				if (name == "--timeout")
					result.TimeoutMilliseconds = milliseconds;
				else
					result.WaitTimeoutMilliseconds = milliseconds;
			}
			else if (name == "--headless")
				result.Headless = true;
			else if (name == "--json")
				result.Json = true;
			else if (name == "--verbose")
				result.Verbose = true;
			else if (name == "--help" || name == "-h")
				result.Help = true;
			else if (name == "--version")
				result.Version = true;
		}
		return result;
	}

	std::optional<EditorConnectionOptions> BuildConnectionOptions(const CliArguments& arguments, std::string& error)
	{
		EditorConnectionOptions options;
		if (arguments.Host)
			options.Host = *arguments.Host;

		if (arguments.Port)
		{
			options.Port = arguments.Port;
			options.Token = arguments.Token ? *arguments.Token : Platform::GetEnvVar(c_EditorTokenVariable).value_or(std::string());
		}
		else if (const std::optional<std::string> environmentPort = Platform::GetEnvVar(c_EditorPortVariable); environmentPort && !environmentPort->empty())
		{
			options.Port = ParsePort(*environmentPort);
			if (!options.Port)
			{
				error = fmt::format("{} ('{}') is not a valid port", c_EditorPortVariable, *environmentPort);
				return std::nullopt;
			}
			options.Token = arguments.Token ? *arguments.Token : Platform::GetEnvVar(c_EditorTokenVariable).value_or(std::string());
		}
		else if (arguments.Token)
		{
			error = "--token requires --port (discovered editor sessions provide their own token)";
			return std::nullopt;
		}
		else if (arguments.Host)
		{
			error = "--host requires --port (discovered editor sessions are reached at the loopback address they record)";
			return std::nullopt;
		}

		// An explicit endpoint has no session file to take the token from, and the editor accepts no connection
		// without one.
		if (options.Port && options.Token.empty())
		{
			error = fmt::format("An explicit editor endpoint needs its session token: set {} (or pass --token)", c_EditorTokenVariable);
			return std::nullopt;
		}

		if (arguments.Project)
			options.ProjectDirectory = ToAbsolutePath(*arguments.Project);
		return options;
	}

	std::string GetUsageText()
	{
		return fmt::format(
			"StrataCLI {} - command-line client for the Strata editor automation API\n"
			"\n"
			"Usage:\n"
			"  StrataCLI call <method> [params-json] [connection options] [--timeout <ms>]\n"
			"  StrataCLI list [connection options] [--json]\n"
			"  StrataCLI status [connection options]\n"
			"  StrataCLI launch --project <dir> [--headless] [--editor <path>] [--wait-timeout <ms>]\n"
			"  StrataCLI mcp [connection options] [--editor <path>] [--timeout <ms>]\n"
			"  StrataCLI --help | --version\n"
			"\n"
			"Commands:\n"
			"  call     Call an editor method and print its JSON result\n"
			"  list     List the editor's methods and their descriptions\n"
			"  status   Show whether an editor is reachable, and the known editor sessions\n"
			"  launch   Start the editor for a project and wait until it accepts connections\n"
			"  mcp      Serve the Model Context Protocol on stdin/stdout (for AI agents)\n"
			"\n"
			"Connection options, in discovery order:\n"
			"  --port <n>                Explicit endpoint (else STRATA_EDITOR_PORT), authenticated with the\n"
			"                            STRATA_EDITOR_TOKEN environment variable (preferred) or --token <t>\n"
			"  --project <dir>           The editor that has this project open (<dir>/.strata/EditorSession.json)\n"
			"  (none)                    The newest running editor session that accepts a connection\n"
			"  --host <address>          Host of the explicit endpoint (default 127.0.0.1; requires --port)\n"
			"Once connected, reconnects only reach the same project's editor (e.g. after it restarts).\n"
			"Avoid --token where possible: command lines are visible to other processes.\n"
			"\n"
			"Other options:\n"
			"  --verbose                 Log diagnostics to stderr\n"
			"\n"
			"Environment:\n"
			"  STRATA_EDITOR_PORT, STRATA_EDITOR_TOKEN  Explicit editor endpoint\n"
			"  STRATA_EDITOR_PATH  Editor executable for launch/mcp (default: StrataEditor next to StrataCLI)\n"
			"  STRATA_SESSION_DIR  Private directory of editor session files (default: <user data>/Strata/Sessions)\n"
			"\n"
			"Exit codes: 0 success, 1 the editor returned an error, 2 no editor reachable or connection lost,\n"
			"            3 invalid command line\n",
			c_EngineVersion);
	}

	void InitializeCliLogging(bool verbose)
	{
		LogSpecification specification;
		specification.ConsoleOutput = false; // The console sink writes to stdout
		specification.Level = verbose ? LogLevel::Trace : LogLevel::Warn;
		Log::Init(specification);

		auto stderrSink = std::make_shared<spdlog::sinks::stderr_sink_mt>();
		stderrSink->set_pattern("[%T] [%l] %n: %v");
		Log::GetCoreLogger()->sinks().push_back(stderrSink);
		Log::GetClientLogger()->sinks().push_back(stderrSink);
		Log::GetScriptLogger()->sinks().push_back(stderrSink);
	}

	int RunCli(const std::vector<std::string>& arguments, std::istream& input, std::ostream& output, std::ostream& errorOutput)
	{
		std::string error;
		const std::optional<CliArguments> parsed = ParseCliArguments(arguments, error);
		if (!parsed)
			return ReportUsageError(error, errorOutput);

		const CliArguments& cliArguments = *parsed;
		if (cliArguments.Version)
		{
			output << "StrataCLI " << c_EngineVersion << "\n";
			return ExitCode::Success;
		}
		if (cliArguments.Help || cliArguments.Command == "help")
		{
			output << GetUsageText();
			return ExitCode::Success;
		}
		if (cliArguments.Command.empty())
		{
			errorOutput << GetUsageText();
			return ExitCode::UsageError;
		}

		if (cliArguments.Command == "call")
			return RunCallCommand(cliArguments, output, errorOutput);
		if (cliArguments.Command == "list")
			return RunListCommand(cliArguments, output, errorOutput);
		if (cliArguments.Command == "status")
			return RunStatusCommand(cliArguments, output, errorOutput);
		if (cliArguments.Command == "launch")
			return RunLaunchCommand(cliArguments, output, errorOutput);
		if (cliArguments.Command == "mcp")
			return RunMcpCommand(cliArguments, input, output, errorOutput);
		return ReportUsageError(fmt::format("unknown command '{}'", cliArguments.Command), errorOutput);
	}

}
