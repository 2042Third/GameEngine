#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "Network/FakeEditorProcess.h"
#include "Renderer/GPUTestUtils.h"
#include "Strata/Core/Log.h"
#include "TestHelpers.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <thread>

// When launched with --strata-test-helper=<mode>, the test executable acts as a child process for the
// Process tests instead of running the test suites. This keeps those tests free of external programs.
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
	if (mode == "cwd")
	{
		const std::u8string currentDirectory = std::filesystem::current_path().generic_u8string();
		std::printf("%s\n", reinterpret_cast<const char*>(currentDirectory.c_str()));
		std::fflush(stdout);
		return 0;
	}
	return 99;
}

int main(int argc, char** argv)
{
	constexpr std::string_view helperPrefix = "--strata-test-helper=";
	if (argc > 1 && std::string_view(argv[1]).substr(0, helperPrefix.size()) == helperPrefix)
		return RunHelperMode(std::string_view(argv[1]).substr(helperPrefix.size()), argc, argv);

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
