#include "CLI/CliApplication.h"

#include "Strata/Core/Log.h"
#include "Strata/Core/PlatformDetection.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#if defined(ST_PLATFORM_WINDOWS)
	#include <fcntl.h>
	#include <io.h>
#endif

int main(int argc, char** argv)
{
#if defined(ST_PLATFORM_WINDOWS)
	// stdout/stdin carry machine-readable output and the MCP protocol: no "\r\n" translation (the CRT's text
	// mode). Input lines may then end in "\r", which the MCP server tolerates.
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stdin), _O_BINARY);
#endif

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
