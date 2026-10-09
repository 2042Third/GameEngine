#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include "Renderer/GPUTestUtils.h"
#include "Strata/Core/FileLock.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "TestHelpers.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

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
	if (mode == "cwd")
	{
		const std::u8string currentDirectory = std::filesystem::current_path().generic_u8string();
		std::printf("%s\n", reinterpret_cast<const char*>(currentDirectory.c_str()));
		std::fflush(stdout);
		return 0;
	}
	if (mode == "hold-file-lock")
	{
		// <path>: locks the existing file, reports "locked" and holds the lock until the process is ended (at most a
		// minute).
		if (argc < 3)
			return 2;
		const Strata::Scope<Strata::FileLock> lock = Strata::FileLock::TryAcquire(Strata::FileSystem::FromUTF8(argv[2]));
		if (!lock)
			return 1;
		std::printf("locked\n");
		std::fflush(stdout);
		std::this_thread::sleep_for(std::chrono::seconds(60));
		return 0;
	}
	if (mode == "script-module-reloads")
	{
		// <module path> <count>: loads the module from a private copy (as with hot reload) and reloads it <count> times.
		if (argc < 4)
			return 2;
		Strata::ScriptEngine engine;
		engine.SetHotReloadEnabled(true);
		std::string error;
		if (!engine.LoadModule(Strata::FileSystem::FromUTF8(argv[2]), &error))
		{
			std::fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		const int count = std::atoi(argv[3]);
		for (int index = 0; index < count; index++)
		{
			if (!engine.Reload(&error) || engine.GetClasses().empty())
			{
				std::fprintf(stderr, "Reload %d failed: %s\n", index, error.c_str());
				return 1;
			}
		}
		return 0;
	}
	if (mode == "script-module-lifecycle")
	{
		// <module> <healthy module>: loads and unloads the first module (whatever happens), then shows that the engine
		// still works by loading the second. Engine messages go to the output too. The process ends without exit
		// handlers: a library abandoned after a crash in its static destructors must not run them again.
		if (argc < 4)
			return 2;
		Strata::LogSpecification logSpecification;
		logSpecification.Level = Strata::LogLevel::Warn;
		Strata::Log::Init(logSpecification);
		int result = 0;
		{
			Strata::ScriptEngine engine;
			std::string error;
			const bool loaded = engine.LoadModule(Strata::FileSystem::FromUTF8(argv[2]), &error);
			std::printf("first module: %s\n", loaded ? "loaded" : error.c_str());
			std::fflush(stdout);
			engine.UnloadModule();
			std::printf("first module: unloaded\n");
			std::fflush(stdout);
			if (engine.LoadModule(Strata::FileSystem::FromUTF8(argv[3]), &error) && !engine.GetClasses().empty())
			{
				std::printf("second module: loaded\n");
			}
			else
			{
				std::printf("second module: %s\n", error.c_str());
				result = 1;
			}
		}
		Strata::Log::Shutdown();
		std::fflush(stdout);
		std::fflush(stderr);
		std::_Exit(result);
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
	return 99;
}

int main(int argc, char** argv)
{
	constexpr std::string_view helperPrefix = "--strata-test-helper=";
	if (argc > 1 && std::string_view(argv[1]).substr(0, helperPrefix.size()) == helperPrefix)
		return RunHelperMode(std::string_view(argv[1]).substr(helperPrefix.size()), argc, argv);

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
