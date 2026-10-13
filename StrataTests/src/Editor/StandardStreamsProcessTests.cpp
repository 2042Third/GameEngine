#include <doctest/doctest.h>

#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/PlatformDetection.h>
#include <Strata/Core/Process.h>
#include <Strata/Core/Version.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// What the editor and the runtime print, and the exit codes they end with, reach the processes that start them: on Windows
// they are GUI-subsystem programs, so that no console window opens with them, and keep the pipes their callers pass
// (Platform::ConnectStandardStreams). CTest runs this suite with the editor automation tests (StrataEditor.Automation),
// passing the executables' paths in STRATA_TEST_EDITOR_PATH, STRATA_TEST_RUNTIME_PATH and STRATA_TEST_CLI_PATH; run
// directly, the tests look next to the test executable.

using namespace Strata;

namespace
{

	constexpr std::chrono::milliseconds c_ProcessTimeout = std::chrono::milliseconds(60000);

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

	Process::RunResult RunCaptured(const std::filesystem::path& program, const std::vector<std::string>& arguments)
	{
		REQUIRE_MESSAGE(FileSystem::IsRegularFile(program), FileSystem::ToUTF8(program), " is not built");
		ProcessSpecification specification;
		specification.Executable = program;
		specification.Arguments = arguments;
		specification.Output = ProcessOutputMode::CaptureSeparate;
		const Process::RunResult result = Process::Run(specification, c_ProcessTimeout);
		REQUIRE_MESSAGE(result.Started, result.Error);
		REQUIRE_FALSE(result.TimedOut);
		return result;
	}

#if defined(ST_PLATFORM_WINDOWS)
	// The subsystem field of a Windows executable's optional header (2: GUI, 3: console), nothing for a file that is not one.
	std::optional<uint16_t> ReadPESubsystem(const std::filesystem::path& file)
	{
		const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytes(file);
		if (!bytes || bytes->size() < 0x40 || (*bytes)[0] != 'M' || (*bytes)[1] != 'Z')
			return std::nullopt;
		const auto read32 = [&bytes](size_t offset)
		{
			return static_cast<uint32_t>((*bytes)[offset]) | static_cast<uint32_t>((*bytes)[offset + 1]) << 8 |
				static_cast<uint32_t>((*bytes)[offset + 2]) << 16 | static_cast<uint32_t>((*bytes)[offset + 3]) << 24;
		};
		// "PE\0\0", the 20-byte file header, then the optional header, whose subsystem is at offset 68 in PE32 and PE32+.
		const size_t header = read32(0x3C);
		const size_t subsystem = header + 4 + 20 + 68;
		if (subsystem + 2 > bytes->size() || read32(header) != 0x00004550)
			return std::nullopt;
		return static_cast<uint16_t>((*bytes)[subsystem] | (*bytes)[subsystem + 1] << 8);
	}
#endif

}

TEST_SUITE("EndToEnd.StandardStreams")
{
	TEST_CASE("The editor prints to the pipes it is given and ends with its exit code")
	{
		const std::filesystem::path editor = FindBuiltExecutable("STRATA_TEST_EDITOR_PATH", "StrataEditor");
		const Process::RunResult version = RunCaptured(editor, { "--version" });
		CHECK(version.ExitCode == 0);
		CHECK(version.Output.rfind(std::string("Strata Editor ") + c_EngineVersion, 0) == 0);

		// A command line it does not understand: the usage on stderr, exit code 3.
		const Process::RunResult usage = RunCaptured(editor, { "--frames", "abc" });
		CHECK(usage.ExitCode == 3);
		CHECK(usage.ErrorOutput.find("Usage: StrataEditor [options]") != std::string::npos);
		CHECK(usage.Output.empty());
	}

#if defined(STRATA_TESTS_HAVE_RUNTIME)
	TEST_CASE("The runtime logs to the pipes it is given and ends with its exit code")
	{
		const std::filesystem::path runtime = FindBuiltExecutable("STRATA_TEST_RUNTIME_PATH", "StrataRuntime");
		const std::filesystem::path missing = Tests::CreateTemporaryDirectory("StandardStreams") / "Missing.stgame";
		const Process::RunResult result = RunCaptured(runtime, { "--game", FileSystem::ToUTF8(missing), "--headless" });
		CHECK(result.ExitCode == 1);
		CHECK((result.Output + result.ErrorOutput).find("No game to run") != std::string::npos);
		CHECK((result.Output + result.ErrorOutput).find(std::string("Strata ") + c_EngineVersion) != std::string::npos);
	}
#endif

#if defined(ST_PLATFORM_WINDOWS)
	TEST_CASE("On Windows the editor and the runtime are windowed programs, the CLI a console program")
	{
		// A console program opens a console window when started from the desktop, as every exported game (a renamed runtime)
		// would.
		CHECK(ReadPESubsystem(FindBuiltExecutable("STRATA_TEST_EDITOR_PATH", "StrataEditor")) == uint16_t(2));
#if defined(STRATA_TESTS_HAVE_RUNTIME)
		CHECK(ReadPESubsystem(FindBuiltExecutable("STRATA_TEST_RUNTIME_PATH", "StrataRuntime")) == uint16_t(2));
#endif
		CHECK(ReadPESubsystem(FindBuiltExecutable("STRATA_TEST_CLI_PATH", "StrataCLI")) == uint16_t(3));
		CHECK(ReadPESubsystem(Platform::GetExecutablePath()) == uint16_t(3));
		CHECK_FALSE(ReadPESubsystem(Platform::GetExecutableDirectory() / "no-such-file.exe"));
	}

	TEST_CASE("On Windows the editor prints into the terminal it was started from")
	{
		// The test executable as a helper with a console of its own (hidden: Process starts it without a window) starts the
		// editor as a terminal does and reports what the console shows.
		const std::filesystem::path editor = FindBuiltExecutable("STRATA_TEST_EDITOR_PATH", "StrataEditor");
		const Process::RunResult version = RunCaptured(Tests::GetTestExecutablePath(), { "--strata-test-helper=console-output",
			FileSystem::ToUTF8(editor), "--version" });
		INFO(version.ErrorOutput);
		CHECK(version.ExitCode == 0);
		CHECK(version.Output.find(std::string("Strata Editor ") + c_EngineVersion) != std::string::npos);

		const Process::RunResult usage = RunCaptured(Tests::GetTestExecutablePath(), { "--strata-test-helper=console-output",
			FileSystem::ToUTF8(editor), "--frames", "abc" });
		INFO(usage.ErrorOutput);
		CHECK(usage.ExitCode == 3);
		CHECK(usage.Output.find("Usage: StrataEditor [options]") != std::string::npos);
	}
#endif
}
