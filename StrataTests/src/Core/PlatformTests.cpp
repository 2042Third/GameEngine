#include <doctest/doctest.h>

#include "Strata/Core/CrashGuard.h"
#include "Strata/Core/DynamicLibrary.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/PlatformDetection.h"
#include "Strata/Core/Process.h"
#include "TestHelpers.h"

#include <chrono>
#include <climits>
#include <stdexcept>
#include <string>

#if defined(ST_PLATFORM_POSIX)
	#include <sys/stat.h>
	#include <unistd.h>
#endif

using namespace Strata;

namespace
{
	ProcessSpecification HelperProcess(std::vector<std::string> arguments)
	{
		ProcessSpecification specification;
		specification.Executable = Tests::GetTestExecutablePath();
		specification.Arguments = std::move(arguments);
		return specification;
	}

	void WriteToNull(void*)
	{
		volatile int* pointer = nullptr;
		*pointer = 42;
	}

	void DivideByZero(void* userData)
	{
		volatile int divisor = *static_cast<int*>(userData);
		volatile int result = 100 / divisor;
		(void)result;
	}

	int Recurse(int depth)
	{
		// The base case is unreachable in practice; it only keeps compilers from flagging infinite recursion.
		// The addition after the call prevents tail-call optimization from turning this into a loop.
		volatile int currentDepth = depth;
		volatile char padding[1024];
		padding[0] = static_cast<char>(currentDepth);
		if (currentDepth == INT_MAX)
			return 0;
		return Recurse(currentDepth + 1) + padding[0];
	}

	void OverflowStack(void*)
	{
		volatile int result = Recurse(0);
		(void)result;
	}

	void SetFlag(void* userData)
	{
		*static_cast<bool*>(userData) = true;
	}

	void NestedGuard(void* userData)
	{
		CrashInfo innerInfo;
		const bool innerSucceeded = CrashGuard::Invoke(WriteToNull, nullptr, &innerInfo);
		*static_cast<bool*>(userData) = !innerSucceeded;
	}

	void ThrowException(void*)
	{
		throw std::runtime_error("Thrown on purpose");
	}

	void NestedThrow(void* userData)
	{
		CrashInfo innerInfo;
		const bool innerSucceeded = CrashGuard::Invoke(ThrowException, nullptr, &innerInfo);
		*static_cast<bool*>(userData) = !innerSucceeded;
	}
}

TEST_SUITE("Core.Platform")
{
	TEST_CASE("Platform queries")
	{
		CHECK_FALSE(Platform::GetName().empty());
		const std::filesystem::path executable = Platform::GetExecutablePath();
		CHECK(FileSystem::Exists(executable));
		CHECK(Platform::GetExecutableDirectory() == executable.parent_path());
		CHECK(Platform::GetProcessID() != 0);
		CHECK(Platform::GetProcessMemoryUsage() > 0);

		REQUIRE(Platform::SetEnvVar("STRATA_TEST_VARIABLE", "value \xC3\xA9"));
		CHECK(Platform::GetEnvVar("STRATA_TEST_VARIABLE").value() == "value \xC3\xA9");
		CHECK_FALSE(Platform::GetEnvVar("STRATA_TEST_VARIABLE_THAT_DOES_NOT_EXIST").has_value());
	}

	TEST_CASE("User data directory is created")
	{
		const std::filesystem::path directory = Platform::GetUserDataDirectory("StrataTests");
		CHECK(FileSystem::IsDirectory(directory));
	}

	TEST_CASE("Private directories are unique and only the user can modify them")
	{
		const std::filesystem::path runtime = Platform::GetUserRuntimeDirectory("StrataTests");
		REQUIRE_FALSE(runtime.empty());
		CHECK(FileSystem::IsDirectory(runtime));
		CHECK(Platform::GetUserRuntimeDirectory("StrataTests") == runtime);

		const std::filesystem::path first = Platform::CreatePrivateDirectory(runtime, "Private-");
		const std::filesystem::path second = Platform::CreatePrivateDirectory(runtime, "Private-");
		REQUIRE_FALSE(first.empty());
		REQUIRE_FALSE(second.empty());
		CHECK(first != second);
		CHECK(first.parent_path() == runtime);
		CHECK(FileSystem::ToUTF8(first.filename()).starts_with("Private-"));
		CHECK(FileSystem::IsDirectory(first));
		CHECK(FileSystem::IsDirectory(second));
#if defined(ST_PLATFORM_POSIX)
		for (const std::filesystem::path& directory : { runtime, first })
		{
			struct stat info = {};
			REQUIRE(lstat(directory.c_str(), &info) == 0);
			CHECK(S_ISDIR(info.st_mode));
			CHECK(info.st_uid == geteuid());
			CHECK((info.st_mode & (S_IWGRP | S_IWOTH)) == 0);
		}
		struct stat created = {};
		REQUIRE(stat(first.c_str(), &created) == 0);
		CHECK((created.st_mode & 0777) == 0700);
#endif
		CHECK(Platform::CreatePrivateDirectory(runtime / "Missing", "Private-").empty());

		CHECK(FileSystem::Remove(first));
		CHECK(FileSystem::Remove(second));
	}

	TEST_CASE("Process liveness")
	{
		CHECK(Platform::IsProcessRunning(Platform::GetProcessID()));
		CHECK_FALSE(Platform::IsProcessRunning(0));

		// The Process object keeps the ended process's ID from being reused while it exists (Windows keeps the handle open).
		Process process;
		REQUIRE(process.Start(HelperProcess({ "--strata-test-helper=exit-code", "0" })));
		const uint32_t processID = process.GetProcessID();
		REQUIRE(process.Wait(std::chrono::milliseconds(30000)).has_value());
		CHECK_FALSE(Platform::IsProcessRunning(processID));
	}

	TEST_CASE("DynamicLibrary loads, resolves symbols and unloads")
	{
		const std::filesystem::path libraryPath = Platform::GetExecutableDirectory() / STRATA_TEST_LIBRARY_NAME;
		DynamicLibrary library;
		REQUIRE_MESSAGE(library.Load(libraryPath), library.GetLastError());
		CHECK(library.IsLoaded());

		using AddFunction = int (*)(int, int);
		AddFunction add = library.GetFunction<AddFunction>("StrataTestLibrary_Add");
		REQUIRE(add != nullptr);
		CHECK(add(2, 3) == 5);
		CHECK(library.GetSymbol("DoesNotExist") == nullptr);

		DynamicLibrary moved = std::move(library);
		CHECK_FALSE(library.IsLoaded());
		CHECK(moved.IsLoaded());
		moved.Unload();
		CHECK_FALSE(moved.IsLoaded());
		CHECK(moved.GetSymbol("StrataTestLibrary_Add") == nullptr);
	}

	TEST_CASE("DynamicLibrary can forget a library without unloading it")
	{
		const std::filesystem::path libraryPath = Platform::GetExecutableDirectory() / STRATA_TEST_LIBRARY_NAME;
		DynamicLibrary library;
		REQUIRE_MESSAGE(library.Load(libraryPath), library.GetLastError());
		library.Release();
		CHECK_FALSE(library.IsLoaded());
		CHECK(library.GetPath().empty());
		CHECK(library.GetSymbol("StrataTestLibrary_Add") == nullptr);

		// The released library is still loaded in the process; loading it again works.
		DynamicLibrary again;
		REQUIRE_MESSAGE(again.Load(libraryPath), again.GetLastError());
		using AddFunction = int (*)(int, int);
		AddFunction add = again.GetFunction<AddFunction>("StrataTestLibrary_Add");
		REQUIRE(add != nullptr);
		CHECK(add(20, 22) == 42);
	}

	TEST_CASE("DynamicLibrary reports errors for missing files")
	{
		DynamicLibrary library;
		CHECK_FALSE(library.Load(Platform::GetExecutableDirectory() / "DoesNotExist.library"));
		CHECK_FALSE(library.GetLastError().empty());
		CHECK(DynamicLibrary::GetPlatformFileName("Game").find("Game") != std::string::npos);
	}

	TEST_CASE("Process captures output and exit codes")
	{
		Process::RunResult echo = Process::Run(HelperProcess({ "--strata-test-helper=echo", "hello world", "\xC3\xA9t\xC3\xA9", "quote\"d" }));
		REQUIRE_MESSAGE(echo.Started, echo.Error);
		CHECK(echo.ExitCode == 0);
		CHECK(echo.Output.find("hello world \xC3\xA9t\xC3\xA9 quote\"d") != std::string::npos);

		Process::RunResult errorOutput = Process::Run(HelperProcess({ "--strata-test-helper=stderr" }));
		CHECK(errorOutput.Output.find("error-output") != std::string::npos);

		Process::RunResult exitCode = Process::Run(HelperProcess({ "--strata-test-helper=exit-code", "7" }));
		CHECK(exitCode.ExitCode == 7);
	}

	TEST_CASE("Process honors the working directory")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ProcessCwd");
		ProcessSpecification specification = HelperProcess({ "--strata-test-helper=cwd" });
		specification.WorkingDirectory = directory;
		Process::RunResult result = Process::Run(specification);
		REQUIRE(result.Started);
		const std::string expected = FileSystem::ToUTF8(std::filesystem::canonical(directory));
		const std::string actual = FileSystem::ToUTF8(std::filesystem::canonical(FileSystem::FromUTF8(result.Output.substr(0, result.Output.find_last_not_of("\r\n") + 1))));
		CHECK(actual == expected);
	}

	TEST_CASE("Process timeouts terminate the child")
	{
		Process::RunResult result = Process::Run(HelperProcess({ "--strata-test-helper=sleep", "30000" }), std::chrono::milliseconds(200));
		CHECK(result.Started);
		CHECK(result.TimedOut);
	}

	TEST_CASE("Process can be started, polled and terminated")
	{
		Process process;
		REQUIRE(process.Start(HelperProcess({ "--strata-test-helper=sleep", "30000" })));
		CHECK(process.GetProcessID() != 0);
		CHECK(process.IsRunning());
		CHECK_FALSE(process.Wait(std::chrono::milliseconds(10)).has_value());
		CHECK(process.Terminate());
		CHECK_FALSE(process.IsRunning());
		CHECK(process.GetExitCode().has_value());
	}

	TEST_CASE("Starting a missing executable fails cleanly")
	{
		ProcessSpecification specification;
		specification.Executable = Platform::GetExecutableDirectory() / "missing-executable-strata";
		Process process;
		CHECK_FALSE(process.Start(specification));
		CHECK_FALSE(process.GetLastError().empty());
	}

	TEST_CASE("CrashGuard runs normal functions")
	{
		bool flag = false;
		CHECK(CrashGuard::Invoke(SetFlag, &flag));
		CHECK(flag);
	}

	TEST_CASE("CrashGuard contains access violations")
	{
		CrashInfo info;
		CHECK_FALSE(CrashGuard::Invoke(WriteToNull, nullptr, &info));
		CHECK_FALSE(info.Description.empty());

		// The guard keeps working after a fault.
		bool flag = false;
		CHECK(CrashGuard::Invoke(SetFlag, &flag));
		CHECK(flag);
		CHECK_FALSE(CrashGuard::Invoke(WriteToNull, nullptr, &info));
	}

	TEST_CASE("CrashGuard contains integer division by zero")
	{
#if defined(__aarch64__) || defined(_M_ARM64)
		// ARM64 integer division by zero returns 0 instead of trapping.
		MESSAGE("Skipped: integer division does not trap on ARM64");
#else
		int divisor = 0;
		CrashInfo info;
		CHECK_FALSE(CrashGuard::Invoke(DivideByZero, &divisor, &info));
		CHECK_FALSE(info.Description.empty());
#endif
	}

	TEST_CASE("CrashGuard contains stack overflows repeatedly")
	{
		for (int attempt = 0; attempt < 2; attempt++)
		{
			CrashInfo info;
			CHECK_FALSE(CrashGuard::Invoke(OverflowStack, nullptr, &info));
			CHECK_FALSE(info.Description.empty());
		}
	}

	TEST_CASE("CrashGuard supports nesting")
	{
		bool innerCaught = false;
		CHECK(CrashGuard::Invoke(NestedGuard, &innerCaught));
		CHECK(innerCaught);
	}

	TEST_CASE("CrashGuard contains C++ exceptions")
	{
		CrashInfo info;
		CHECK_FALSE(CrashGuard::Invoke(ThrowException, nullptr, &info));
		CHECK(info.Description.find("C++ exception") != std::string::npos);

		bool innerCaught = false;
		CHECK(CrashGuard::Invoke(NestedThrow, &innerCaught));
		CHECK(innerCaught);

		// The guard keeps working: later faults are still contained.
		CHECK_FALSE(CrashGuard::Invoke(WriteToNull, nullptr, &info));
		bool flag = false;
		CHECK(CrashGuard::Invoke(SetFlag, &flag));
		CHECK(flag);
	}
}
