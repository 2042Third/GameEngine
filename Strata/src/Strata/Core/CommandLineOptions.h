#pragma once

#include "Strata/Core/CommandLine.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Strata
{

	// An option a program understands: its name, what follows it ("--frames 120" or "--frames=120"; null for flags) and what
	// it does, for the program's usage text and for finding the arguments it does not understand.
	struct CommandLineOption
	{
		const char* Name;        // "--frames"
		const char* Value;       // "<count>"; null for flags
		const char* Description;
	};

	namespace CommandLineOptions
	{

		// The exit code of a command line a program does not understand (the editor's, the runtime's; StrataCLI's usage
		// errors use it too).
		constexpr int c_UsageExitCode = 3;

		// What is wrong with the command line: an argument the table lacks, a flag given a value, or an option without its value;
		// nullopt when it has nothing wrong.
		std::optional<std::string> FindUnknownArgument(const CommandLine& commandLine, std::span<const CommandLineOption> options);
		// "Usage: <program> [options]", then each option with its value and description, one per line.
		std::string FormatUsage(std::string_view program, std::span<const CommandLineOption> options);

	}

}
