#pragma once

#include "Strata/Core/Base.h"
#include "CLI/EditorConnection.h"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace Strata::CLI
{

	// Process exit codes of StrataCLI.
	struct ExitCode
	{
		enum : int
		{
			Success = 0,
			RpcError = 1,          // The editor answered with an error
			ConnectionFailure = 2, // No editor reachable, connection lost, or the editor failed to launch
			UsageError = 3         // Invalid command line
		};
	};

	struct CliArguments
	{
		std::string Command;
		std::vector<std::string> Positionals; // Arguments after the command that are not options
		std::optional<std::string> Project;
		std::optional<std::string> Editor;
		std::optional<std::string> Host;
		std::optional<uint16_t> Port;
		std::optional<std::string> Token;
		std::optional<int64_t> TimeoutMilliseconds;
		std::optional<int64_t> WaitTimeoutMilliseconds;
		std::optional<std::string> SaveImage; // call: where to write an image result
		bool Headless = false;
		bool NoGpu = false;
		bool Json = false;
		bool Verbose = false;
		bool Help = false;
		bool Version = false;
	};

	// Parses arguments (without the program name). Options take the forms "--name value" and "--name=value".
	// Returns nullopt with a message in error for invalid input.
	std::optional<CliArguments> ParseCliArguments(const std::vector<std::string>& arguments, std::string& error);

	// Connection options from the parsed arguments, falling back to STRATA_EDITOR_PORT / STRATA_EDITOR_TOKEN
	// when no --port is given. Returns nullopt with a message in error for invalid input.
	std::optional<EditorConnectionOptions> BuildConnectionOptions(const CliArguments& arguments, std::string& error);

	std::string GetUsageText();

	// Routes logging to stderr only (stdout carries command results and MCP protocol messages).
	void InitializeCliLogging(bool verbose);

	// Runs StrataCLI with arguments (without the program name) and returns the process exit code. Results go to
	// output, diagnostics to errorOutput; input feeds the MCP server and the params of `call -`.
	int RunCli(const std::vector<std::string>& arguments, std::istream& input, std::ostream& output, std::ostream& errorOutput);

}
