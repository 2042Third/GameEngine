#include "CLI/CliApplication.h"

#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
	// stdout/stdin carry machine-readable output and the MCP protocol: no newline translation. Input lines may then
	// end in "\r", which the MCP server tolerates.
	Strata::Platform::SetBinaryStandardStreams();

	std::vector<std::string> arguments;
	for (int index = 1; index < argc; index++)
		arguments.emplace_back(argv[index] ? argv[index] : "");

	const bool verbose = std::find(arguments.begin(), arguments.end(), "--verbose") != arguments.end();
	Strata::CLI::InitializeCliLogging(verbose);

	const int exitCode = Strata::CLI::RunCli(arguments, std::cin, std::cout, std::cerr);
	std::cout.flush();
	Strata::Log::Shutdown();
	return exitCode;
}
