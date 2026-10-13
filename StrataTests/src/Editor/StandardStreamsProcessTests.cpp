#include <doctest/doctest.h>

#include "TestHelpers.h"

#include <Strata/Core/Application.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/PlatformDetection.h>
#include <Strata/Core/Process.h>
#include <Strata/Core/Version.h>
#include <Strata/Renderer/GraphicsDevice.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(ST_PLATFORM_WINDOWS)
	#include <csignal>
	#include <sys/types.h>
#endif

// What the editor and the runtime print, and the exit codes they end with, reach the processes that start them: on Windows
// they are GUI-subsystem programs, so that no console window opens with them, and keep the pipes their callers pass
// (Platform::ConnectStandardStreams). Console runs (scripted, headless, --version) print into the terminal that started
// them and quit in order on its Ctrl+C; Interactive runs (a person's window) leave the terminal alone and show fatal
// errors nobody would read in a dialog, whose text tests read from STRATA_ERROR_DIALOG_FILE instead. CTest runs this suite
// with the editor automation tests (StrataEditor.Automation), passing the executables' paths in STRATA_TEST_EDITOR_PATH,
// STRATA_TEST_RUNTIME_PATH and STRATA_TEST_CLI_PATH; run directly, the tests look next to the test executable.

using namespace Strata;

namespace
{

	constexpr std::chrono::milliseconds c_ProcessTimeout = std::chrono::milliseconds(60000);
	// What a headless editor without automation or --frames logs before its first frame (it then runs until it is stopped).
	constexpr const char* c_RunningMessage = "Running headless without automation or --frames";

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

	// Runs a program with its output sent to the null device, as a launcher that reads none of it does.
	int RunDiscarded(const std::filesystem::path& program, const std::vector<std::string>& arguments)
	{
		REQUIRE_MESSAGE(FileSystem::IsRegularFile(program), FileSystem::ToUTF8(program), " is not built");
		ProcessSpecification specification;
		specification.Executable = program;
		specification.Arguments = arguments;
		specification.Output = ProcessOutputMode::Discard;
		// Process::Run captures the output: this starts the process itself.
		Process process;
		REQUIRE_MESSAGE(process.Start(specification), process.GetLastError());
		const std::optional<int> exitCode = process.Wait(c_ProcessTimeout);
		if (!exitCode)
			process.Terminate();
		REQUIRE(exitCode);
		return *exitCode;
	}

	// What the error dialog of a run said (STRATA_ERROR_DIALOG_FILE): its title, a line, then its message; nothing without one.
	std::optional<std::string> ReadDialog(const std::filesystem::path& file)
	{
		return FileSystem::IsRegularFile(file) ? FileSystem::ReadText(file) : std::nullopt;
	}

	bool Contains(const std::string& text, const std::string& part)
	{
		return text.find(part) != std::string::npos;
	}

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

	TEST_CASE("A game that cannot start shows a person why, and never stops a tool with a dialog")
	{
		const std::filesystem::path runtime = FindBuiltExecutable("STRATA_TEST_RUNTIME_PATH", "StrataRuntime");
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("StandardStreams");
		const std::filesystem::path missing = directory / "Missing.stgame";
		const std::filesystem::path dialog = directory / "Dialog.txt";
		Tests::ScopedEnvironmentVariable dialogFile("STRATA_ERROR_DIALOG_FILE", FileSystem::ToUTF8(dialog));

		// A person's run whose output nobody reads (a launcher, the desktop): the dialog says what is wrong, what to do and
		// where the log is.
		CHECK(RunDiscarded(runtime, { "--game", FileSystem::ToUTF8(missing) }) == 1);
		const std::optional<std::string> shown = ReadDialog(dialog);
		REQUIRE(shown);
		CHECK(shown->rfind("Strata Game\n", 0) == 0);
		CHECK(Contains(*shown, "No game to run: "));
		CHECK(Contains(*shown, "StrataRuntime plays games exported from Strata Editor"));
		CHECK(Contains(*shown, "The log has the details: "));
		CHECK(Contains(*shown, "Game.log"));
		REQUIRE(FileSystem::Remove(dialog));

		// Whoever reads the output gets the error there: no dialog. Nor in a headless run (a server, CI), whatever its output.
		CHECK(RunCaptured(runtime, { "--game", FileSystem::ToUTF8(missing) }).ExitCode == 1);
		CHECK_FALSE(ReadDialog(dialog));
		CHECK(RunDiscarded(runtime, { "--game", FileSystem::ToUTF8(missing), "--headless" }) == 1);
		CHECK_FALSE(ReadDialog(dialog));
	}
#endif

#if !defined(ST_PLATFORM_WINDOWS)
	TEST_CASE("SIGINT and SIGTERM end a headless editor in order")
	{
		const std::filesystem::path editor = FindBuiltExecutable("STRATA_TEST_EDITOR_PATH", "StrataEditor");
		REQUIRE_MESSAGE(FileSystem::IsRegularFile(editor), FileSystem::ToUTF8(editor), " is not built");
		for (const int stopSignal : { SIGINT, SIGTERM })
		{
			CAPTURE(stopSignal);
			ProcessSpecification specification;
			specification.Executable = editor;
			// Without automation or --frames it runs until it is stopped.
			specification.Arguments = { "--no-gpu", "--no-automation" };
			Process process;
			REQUIRE_MESSAGE(process.Start(specification), process.GetLastError());
			std::string output;
			const auto deadline = std::chrono::steady_clock::now() + c_ProcessTimeout;
			while (!Contains(output, c_RunningMessage) && process.IsRunning() && std::chrono::steady_clock::now() < deadline)
			{
				output += process.TakeOutput();
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			}
			REQUIRE_MESSAGE(Contains(output, c_RunningMessage), output);
			REQUIRE(kill(static_cast<pid_t>(process.GetProcessID()), stopSignal) == 0);
			const std::optional<int> exitCode = process.Wait(c_ProcessTimeout);
			if (!exitCode)
				process.Terminate();
			REQUIRE(exitCode);
			const auto outputDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!process.IsOutputFinished() && std::chrono::steady_clock::now() < outputDeadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			output += process.TakeOutput();
			CHECK(*exitCode == Application::c_InterruptedExitCode);
			CHECK_MESSAGE(Contains(output, "Interrupted (Ctrl+C or a request to terminate): closing"), output);
		}
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

	TEST_CASE("On Windows Ctrl+C in the terminal ends a headless editor in order")
	{
		// Without automation or --frames it runs until it is stopped; once it says so, Ctrl+C is pressed in its terminal.
		// Killed by the console, it would end with STATUS_CONTROL_C_EXIT before it shut down.
		const std::filesystem::path editor = FindBuiltExecutable("STRATA_TEST_EDITOR_PATH", "StrataEditor");
		const Process::RunResult run = RunCaptured(Tests::GetTestExecutablePath(), { "--strata-test-helper=console-output", "--interrupt-when",
			c_RunningMessage, FileSystem::ToUTF8(editor), "--no-gpu", "--no-automation" });
		INFO(run.Output);
		INFO(run.ErrorOutput);
		CHECK(run.ExitCode == Application::c_InterruptedExitCode);
		CHECK(Contains(run.Output, "Interrupted (Ctrl+C or a request to terminate): closing"));
	}

#if defined(STRATA_TESTS_HAVE_RUNTIME)
	TEST_CASE("On Windows a game played in a window leaves the terminal alone, and a headless one prints into it")
	{
		const std::filesystem::path runtime = FindBuiltExecutable("STRATA_TEST_RUNTIME_PATH", "StrataRuntime");
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("StandardStreams");
		const std::filesystem::path missing = directory / "Missing.stgame";
		const std::filesystem::path dialog = directory / "Dialog.txt";
		Tests::ScopedEnvironmentVariable dialogFile("STRATA_ERROR_DIALOG_FILE", FileSystem::ToUTF8(dialog));

		// A person's window: nothing in the terminal (whose Ctrl+C and closing would end it, and whose prompt the log would run
		// into), so the person learns in a dialog why the game cannot start.
		const Process::RunResult windowed = RunCaptured(Tests::GetTestExecutablePath(), { "--strata-test-helper=console-output",
			FileSystem::ToUTF8(runtime), "--game", FileSystem::ToUTF8(missing) });
		INFO(windowed.ErrorOutput);
		CHECK(windowed.ExitCode == 1);
		CHECK_FALSE(Contains(windowed.Output, "No game to run"));
		CHECK_FALSE(Contains(windowed.Output, std::string("Strata ") + c_EngineVersion));
		const std::optional<std::string> shown = ReadDialog(dialog);
		REQUIRE(shown);
		CHECK(Contains(*shown, "No game to run: "));
		REQUIRE(FileSystem::Remove(dialog));

		// A headless run prints into the terminal and shows no dialog.
		const Process::RunResult headless = RunCaptured(Tests::GetTestExecutablePath(), { "--strata-test-helper=console-output",
			FileSystem::ToUTF8(runtime), "--game", FileSystem::ToUTF8(missing), "--headless" });
		INFO(headless.ErrorOutput);
		CHECK(headless.ExitCode == 1);
		CHECK(Contains(headless.Output, "No game to run"));
		CHECK_FALSE(ReadDialog(dialog));
	}
#endif
#endif
}

// A desktop, not a GPU: the editor opens its window before it looks for a graphics device, and windows need a machine like
// the other GPU tests' (CI's runners skip them).
TEST_SUITE("GPU.Startup")
{
	TEST_CASE("An editor without a usable graphics device tells the person what to install")
	{
		const std::filesystem::path editor = FindBuiltExecutable("STRATA_TEST_EDITOR_PATH", "StrataEditor");
		REQUIRE_MESSAGE(FileSystem::IsRegularFile(editor), FileSystem::ToUTF8(editor), " is not built");
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("NoGraphicsDevice");
		const std::filesystem::path dialog = directory / "Dialog.txt";
		// The Vulkan loader finds no driver (VK_DRIVER_FILES, and VK_ICD_FILENAMES for older loaders), as on a machine without
		// one. Only the editor's environment says so (a helper sets it), never this process's, whose tests use the GPU.
		const std::string noDriver = FileSystem::ToUTF8(directory / "NoDriver.json");

		// A person's window (no --frames, --commands or --headless) whose output nobody reads.
		CHECK(RunDiscarded(Tests::GetTestExecutablePath(), { "--strata-test-helper=run-with-environment", "VK_DRIVER_FILES=" + noDriver,
			"VK_ICD_FILENAMES=" + noDriver, "STRATA_ERROR_DIALOG_FILE=" + FileSystem::ToUTF8(dialog), "--", FileSystem::ToUTF8(editor), "--windowed",
			"--layout", FileSystem::ToUTF8(directory / "Layout.ini") }) == 1);
		const std::optional<std::string> shown = ReadDialog(dialog);
		REQUIRE(shown);
		CHECK(shown->rfind("Strata Editor\n", 0) == 0);
		CHECK(Contains(*shown, "Strata Editor cannot start: no usable graphics device was found (it needs Vulkan 1.2 or later)."));
		CHECK(Contains(*shown, GraphicsDevice::GetDriverInstallAdvice()));
		CHECK(Contains(*shown, "The log has the details: "));
		CHECK(Contains(*shown, "StrataEditor.log"));
	}
}
