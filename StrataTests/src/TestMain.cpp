#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "Network/FakeEditorProcess.h"
#include "Renderer/GPUTestUtils.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Process.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "TestHelpers.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#if defined(ST_PLATFORM_LINUX)
	#include <sys/prctl.h>
#endif

// Helper processes that could outlive their test stop when it says so: once the stop file exists or its directory is gone
// (temporary directories are removed when the tests end), and after a minute at the latest.
static bool KeepHelperRunning(const std::filesystem::path& stop, std::chrono::steady_clock::time_point deadline)
{
	return !Strata::FileSystem::Exists(stop) && Strata::FileSystem::IsDirectory(stop.parent_path()) && std::chrono::steady_clock::now() < deadline;
}

// When launched with --strata-test-helper=<mode>, the test executable acts as a child process for the
// Process tests (and checks build products for CTest scripts) instead of running the test suites. This keeps
// those tests free of external programs.
static int RunHelperMode(std::string_view mode, int argc, char** argv)
{
	if (mode == "echo")
	{
		for (int index = 2; index < argc; index++)
			std::printf("%s%s", index > 2 ? " " : "", argv[index]);
		std::printf("\n");
		std::fflush(stdout);
		return 0;
	}
	if (mode == "stderr")
	{
		std::fprintf(stderr, "error-output\n");
		std::fflush(stderr);
		return 0;
	}
	if (mode == "exit-code")
		return argc > 2 ? std::atoi(argv[2]) : 0;
	if (mode == "sleep")
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(argc > 2 ? std::atoi(argv[2]) : 10000));
		return 0;
	}
	if (mode == "heartbeat" && argc > 3)
	{
		// <file> <stop file>: appends a byte to the file every 10 ms (see KeepHelperRunning).
		const std::filesystem::path beats = Strata::FileSystem::FromUTF8(argv[2]);
		const std::filesystem::path stop = Strata::FileSystem::FromUTF8(argv[3]);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
		while (KeepHelperRunning(stop, deadline))
		{
			{
				std::ofstream file(beats, std::ios::binary | std::ios::app);
				file.put('.');
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return 0;
	}
	if (mode == "spawn-heartbeat" && argc > 3)
	{
		// <file> <stop file>: starts a "heartbeat" child (a grandchild of the test) and waits (see KeepHelperRunning).
		Strata::ProcessSpecification specification;
		specification.Executable = Strata::Platform::GetExecutablePath();
		specification.Arguments = { "--strata-test-helper=heartbeat", argv[2], argv[3] };
		specification.Output = Strata::ProcessOutputMode::Discard;
		Strata::Process heartbeat;
		if (!heartbeat.Start(specification))
			return 1;
		const std::filesystem::path stop = Strata::FileSystem::FromUTF8(argv[3]);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
		while (KeepHelperRunning(stop, deadline))
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return 0;
	}
	if (mode == "cwd")
	{
		const std::u8string currentDirectory = std::filesystem::current_path().generic_u8string();
		std::printf("%s\n", reinterpret_cast<const char*>(currentDirectory.c_str()));
		std::fflush(stdout);
		return 0;
	}
	if (mode == "load-script-module")
	{
		// <module path> <class name>...: succeeds if the module loads and contains every class.
		if (argc < 3)
			return 2;
		Strata::ScriptEngine engine;
		std::string error;
		if (!engine.LoadModule(Strata::FileSystem::FromUTF8(argv[2]), &error))
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		for (int index = 3; index < argc; index++)
		{
			if (!engine.FindClass(argv[index]))
			{
				std::fprintf(stderr, "The module has no script class '%s'\n", argv[index]);
				return 1;
			}
		}
		return 0;
	}

#if defined(ST_PLATFORM_LINUX)
	if (mode == "rename-when-file-exists" && argc > 2)
	{
		// Once the parent creates the file, takes a name containing ')' and a newline (which /proc/<pid>/stat shows
		// unescaped inside its parentheses), then runs until it is terminated.
		const std::filesystem::path trigger(argv[2]);
		std::error_code error;
		for (int attempt = 0; attempt < 1000 && !std::filesystem::exists(trigger, error); attempt++)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		// prctl reads its arguments as unsigned long, which is uintptr_t on Linux.
		if (prctl(PR_SET_NAME, reinterpret_cast<uintptr_t>("a)\nb"), 0UL, 0UL, 0UL) != 0)
			return 1;
		std::this_thread::sleep_for(std::chrono::seconds(30));
		return 0;
	}
#endif
	return 99;
}

// With STRATA_TEST_FAKE_CMAKE=succeed, the test executable run with CMake's arguments ("-S ..." to configure,
// "--build ..." to build) stands in for CMake in script builds (ScriptBuildSettings::CMake): it succeeds without building
// anything, so the module file stays as the test left it.
static std::optional<int> RunAsFakeCMake(int argc, char** argv)
{
	const std::optional<std::string> mode = Strata::Platform::GetEnvVar("STRATA_TEST_FAKE_CMAKE");
	if (!mode || argc < 2 || (std::string_view(argv[1]) != "-S" && std::string_view(argv[1]) != "--build"))
		return std::nullopt;
	if (*mode != "succeed")
		return std::nullopt;
	std::printf("-- Fake CMake: %s\n", argv[1]);
	std::fflush(stdout);
	return 0;
}

int main(int argc, char** argv)
{
	constexpr std::string_view helperPrefix = "--strata-test-helper=";
	if (argc > 1 && std::string_view(argv[1]).substr(0, helperPrefix.size()) == helperPrefix)
		return RunHelperMode(std::string_view(argv[1]).substr(helperPrefix.size()), argc, argv);
	if (const std::optional<int> fakeCMake = RunAsFakeCMake(argc, argv))
		return *fakeCMake;

	// Launched as an editor by the CLI launch tests (see Network/FakeEditorProcess.h).
	if (Strata::Tests::IsFakeEditorLaunch(argc, argv))
		return Strata::Tests::RunFakeEditor(argc, argv);

	Strata::LogSpecification logSpecification;
	logSpecification.Level = Strata::LogLevel::Warn;
	Strata::Log::Init(logSpecification);

	doctest::Context context(argc, argv);
	const int result = context.run();

	Strata::Tests::GPUContext::ShutdownShared();
	Strata::Log::Shutdown();
	Strata::Tests::CleanupTemporaryDirectories();
	return result;
}
