#include <doctest/doctest.h>

#include "Strata/Core/CrashGuard.h"
#include "Strata/Core/DynamicLibrary.h"
#include "Strata/Core/FileLock.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/PlatformDetection.h"
#include "Strata/Core/Process.h"
#include "TestHelpers.h"

#include <chrono>
#include <climits>
#include <cstdlib>
#include <stdexcept>
#include <string>

#if defined(ST_PLATFORM_POSIX)
	#include <csignal>
	#include <pthread.h>
	#include <sys/stat.h>
	#include <unistd.h>
#elif defined(ST_PLATFORM_WINDOWS)
	#include <Windows.h>
	#include <aclapi.h>
	#include <sddl.h>
	#include <winioctl.h>

	#include <cstring>
	#include <vector>
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

#if !defined(__aarch64__) && !defined(_M_ARM64)
	// Integer division by zero does not trap on ARM64 (it returns 0), so the tests that use this only exist elsewhere.
	void DivideByZero(void* userData)
	{
		volatile int divisor = *static_cast<int*>(userData);
		volatile int result = 100 / divisor;
		(void)result;
	}
#endif

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

#if defined(ST_PLATFORM_WINDOWS)
	// Points STRATA_RUNTIME_DIR somewhere else for the scope.
	class ScopedRuntimeDirectory
	{
	public:
		explicit ScopedRuntimeDirectory(const std::filesystem::path& directory)
			: m_Previous(Platform::GetEnvVar("STRATA_RUNTIME_DIR").value_or(""))
		{
			Platform::SetEnvVar("STRATA_RUNTIME_DIR", FileSystem::ToUTF8(directory));
		}

		~ScopedRuntimeDirectory()
		{
			Platform::SetEnvVar("STRATA_RUNTIME_DIR", m_Previous);
		}

		ScopedRuntimeDirectory(const ScopedRuntimeDirectory&) = delete;
		ScopedRuntimeDirectory& operator=(const ScopedRuntimeDirectory&) = delete;
	private:
		std::string m_Previous;
	};

	std::vector<uint8_t> SidBytes(PSID sid)
	{
		const uint8_t* bytes = static_cast<const uint8_t*>(sid);
		return std::vector<uint8_t>(bytes, bytes + GetLengthSid(sid));
	}

	std::vector<uint8_t> GetCurrentUserSid()
	{
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
			return {};
		DWORD size = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::vector<uint8_t> tokenUser(size);
		const bool queried = size > 0 && GetTokenInformation(token, TokenUser, tokenUser.data(), size, &size);
		CloseHandle(token);
		if (!queried)
			return {};
		return SidBytes(reinterpret_cast<const TOKEN_USER*>(tokenUser.data())->User.Sid);
	}

	struct AccessEntry
	{
		BYTE Type = 0;
		BYTE Flags = 0;
		ACCESS_MASK Mask = 0;
		std::vector<uint8_t> Sid; // Of allow and deny entries
	};

	struct FileSecurity
	{
		bool Read = false;
		bool Protected = false;
		std::vector<AccessEntry> Entries;
	};

	FileSecurity ReadFileSecurity(const std::filesystem::path& path)
	{
		FileSecurity security;
		PACL dacl = nullptr;
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS)
			return security;

		SECURITY_DESCRIPTOR_CONTROL control = 0;
		DWORD revision = 0;
		security.Read = dacl && GetSecurityDescriptorControl(descriptor, &control, &revision);
		security.Protected = (control & SE_DACL_PROTECTED) != 0;
		for (DWORD index = 0; security.Read && index < dacl->AceCount; index++)
		{
			void* ace = nullptr;
			if (!GetAce(dacl, index, &ace))
			{
				security.Read = false;
				break;
			}
			const ACE_HEADER* header = static_cast<const ACE_HEADER*>(ace);
			AccessEntry entry;
			entry.Type = header->AceType;
			entry.Flags = header->AceFlags;
			if (header->AceType == ACCESS_ALLOWED_ACE_TYPE || header->AceType == ACCESS_DENIED_ACE_TYPE)
			{
				// Both kinds have this layout.
				const ACCESS_ALLOWED_ACE* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(ace);
				entry.Mask = allowed->Mask;
				entry.Sid = SidBytes(const_cast<DWORD*>(&allowed->SidStart));
			}
			security.Entries.push_back(std::move(entry));
		}
		LocalFree(descriptor);
		return security;
	}

	// Only the current user has access: a protected DACL whose one entry grants the user full access and is inherited
	// by everything created inside.
	void CheckOwnerOnlyDirectory(const std::filesystem::path& directory)
	{
		INFO("Directory: ", FileSystem::ToUTF8(directory));
		const FileSecurity security = ReadFileSecurity(directory);
		REQUIRE(security.Read);
		CHECK(security.Protected);
		REQUIRE(security.Entries.size() == 1);
		const AccessEntry& entry = security.Entries[0];
		CHECK(entry.Type == ACCESS_ALLOWED_ACE_TYPE);
		CHECK(entry.Flags == (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE));
		CHECK(entry.Mask == FILE_ALL_ACCESS);
		CHECK(entry.Sid == GetCurrentUserSid());
	}

	// A directory everyone may modify.
	bool CreateSharedDirectory(const std::filesystem::path& path)
	{
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;OICI;FA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr))
			return false;
		SECURITY_ATTRIBUTES attributes = { sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE };
		const bool created = CreateDirectoryW(path.c_str(), &attributes) != FALSE;
		LocalFree(descriptor);
		return created;
	}

	// Creates `link` as a junction to the directory `target` (unlike symbolic links, junctions need no privilege).
	bool CreateJunction(const std::filesystem::path& link, const std::filesystem::path& target)
	{
		if (!CreateDirectoryW(link.c_str(), nullptr))
			return false;
		HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			return false;

		// The mount point form of REPARSE_DATA_BUFFER (declared in the driver kit only), followed by both names.
		struct MountPointHeader
		{
			DWORD ReparseTag;
			WORD ReparseDataLength;
			WORD Reserved;
			WORD SubstituteNameOffset;
			WORD SubstituteNameLength;
			WORD PrintNameOffset;
			WORD PrintNameLength;
		};
		const std::wstring printName = std::filesystem::path(target).make_preferred().wstring();
		const std::wstring substituteName = L"\\??\\" + printName;
		const size_t substituteSize = (substituteName.size() + 1) * sizeof(wchar_t);
		const size_t printSize = (printName.size() + 1) * sizeof(wchar_t);

		MountPointHeader header = {};
		header.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
		header.ReparseDataLength = static_cast<WORD>(4 * sizeof(WORD) + substituteSize + printSize);
		header.SubstituteNameOffset = 0;
		header.SubstituteNameLength = static_cast<WORD>(substituteName.size() * sizeof(wchar_t));
		header.PrintNameOffset = static_cast<WORD>(substituteSize);
		header.PrintNameLength = static_cast<WORD>(printName.size() * sizeof(wchar_t));
		std::vector<uint8_t> buffer(sizeof(header) + substituteSize + printSize);
		std::memcpy(buffer.data(), &header, sizeof(header));
		std::memcpy(buffer.data() + sizeof(header), substituteName.c_str(), substituteSize);
		std::memcpy(buffer.data() + sizeof(header) + substituteSize, printName.c_str(), printSize);

		DWORD returned = 0;
		const bool set = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr) != FALSE;
		CloseHandle(handle);
		return set;
	}
#endif
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

	TEST_CASE("The runtime directory is the user's, or the one STRATA_RUNTIME_DIR names")
	{
		// The tests run with STRATA_RUNTIME_DIR set to a private temporary directory (TestMain.cpp).
		const std::optional<std::string> configured = Platform::GetEnvVar("STRATA_RUNTIME_DIR");
		REQUIRE(configured.has_value());
		const std::filesystem::path runtime = Platform::GetUserRuntimeDirectory("StrataTests");
		CHECK(runtime == FileSystem::FromUTF8(*configured) / "StrataTests");
		CHECK(FileSystem::IsDirectory(runtime));
		CHECK(Platform::GetUserRuntimeDirectory("StrataTests") == runtime);

		// Without it, every user has a location (with a temporary-directory fallback on POSIX). Removed again afterwards.
		REQUIRE(Platform::SetEnvVar("STRATA_RUNTIME_DIR", ""));
		const std::filesystem::path user = Platform::GetUserRuntimeDirectory("StrataTestsUser");
		REQUIRE(Platform::SetEnvVar("STRATA_RUNTIME_DIR", *configured));
		REQUIRE_FALSE(user.empty());
		CHECK(FileSystem::IsDirectory(user));
		CHECK(user != runtime);
		// Windows: <local application data>/StrataTestsUser/Runtime.
		CHECK(FileSystem::Remove(user.filename() == "Runtime" ? user.parent_path() : user));
	}

	TEST_CASE("Private directories are unique and only the user can modify them")
	{
		const std::filesystem::path runtime = Platform::GetUserRuntimeDirectory("StrataTests");
		REQUIRE_FALSE(runtime.empty());
		CHECK(FileSystem::IsDirectory(runtime));

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

#if defined(ST_PLATFORM_WINDOWS)
	TEST_CASE("Runtime and private directories grant only the current user access")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("RuntimeSecurity");
		const ScopedRuntimeDirectory scopedRuntime(root);

		// New directories get an owner-only DACL instead of inheriting the parent's entries.
		const std::filesystem::path runtime = Platform::GetUserRuntimeDirectory("StrataSecurity");
		REQUIRE(runtime == root / "StrataSecurity");
		CheckOwnerOnlyDirectory(runtime);
		const std::filesystem::path directory = Platform::CreatePrivateDirectory(runtime, "Private-");
		REQUIRE_FALSE(directory.empty());
		CheckOwnerOnlyDirectory(directory);

		// What is created inside inherits it.
		const std::filesystem::path file = directory / "File.txt";
		REQUIRE(FileSystem::WriteText(file, "Private"));
		const FileSecurity fileSecurity = ReadFileSecurity(file);
		REQUIRE(fileSecurity.Read);
		REQUIRE(fileSecurity.Entries.size() == 1);
		CHECK(fileSecurity.Entries[0].Sid == GetCurrentUserSid());
		CHECK((fileSecurity.Entries[0].Flags & INHERITED_ACE) != 0);

		// An existing private directory is used as it is.
		CHECK(Platform::GetUserRuntimeDirectory("StrataSecurity") == runtime);
	}

	TEST_CASE("Runtime directories that others can modify, and links, are not used")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("RuntimeRefused");
		{
			// The directory containing it.
			const std::filesystem::path shared = root / "Shared";
			REQUIRE(CreateSharedDirectory(shared));
			const ScopedRuntimeDirectory scopedRuntime(shared);
			CHECK(Platform::GetUserRuntimeDirectory("StrataRefused").empty());
		}

		const ScopedRuntimeDirectory scopedRuntime(root);
		REQUIRE(CreateSharedDirectory(root / "StrataShared"));
		CHECK(Platform::GetUserRuntimeDirectory("StrataShared").empty());

		// A junction to a private directory: the directory itself must be the user's.
		const std::filesystem::path target = Platform::GetUserRuntimeDirectory("StrataTarget");
		REQUIRE_FALSE(target.empty());
		const std::filesystem::path junction = root / "StrataJunction";
		REQUIRE(CreateJunction(junction, target));
		CHECK(Platform::GetUserRuntimeDirectory("StrataJunction").empty());
		CHECK(RemoveDirectoryW(junction.c_str()));
		CHECK(FileSystem::IsDirectory(target));
	}
#endif

#if defined(ST_PLATFORM_LINUX)
	TEST_CASE("Without a private per-user location the runtime directory falls back to the temporary directory")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("RuntimeFallback");
		const std::filesystem::path runtime = root / "Runtime";
		const std::filesystem::path cache = root / "Cache";
		const std::filesystem::path temporary = root / "Temp";
		const std::filesystem::path shared = root / "Shared";
		using std::filesystem::perms;
		std::error_code error;
		for (const std::filesystem::path& directory : { runtime, cache, temporary, shared })
			REQUIRE(FileSystem::CreateDirectories(directory));
		// Group-writable runtime and cache directories do not qualify (others could replace what is in them).
		std::filesystem::permissions(runtime, perms::owner_all | perms::group_all, std::filesystem::perm_options::replace, error);
		std::filesystem::permissions(cache, perms::owner_all | perms::group_all, std::filesystem::perm_options::replace, error);
		std::filesystem::permissions(temporary, perms::owner_all, std::filesystem::perm_options::replace, error);
		// A shared temporary directory without the sticky bit lets others rename entries: refused too.
		std::filesystem::permissions(shared, perms::all, std::filesystem::perm_options::replace, error);
		REQUIRE_FALSE(error);

		auto find = [&](const std::filesystem::path& temporaryDirectory)
		{
			const Process::RunResult result = Process::Run(HelperProcess({ "--strata-test-helper=runtime-directory", "StrataFallback", FileSystem::ToUTF8(runtime),
				FileSystem::ToUTF8(cache), FileSystem::ToUTF8(temporaryDirectory) }), std::chrono::milliseconds(30000));
			REQUIRE(result.ExitCode == 0);
			const size_t begin = result.Output.find('[');
			const size_t end = result.Output.rfind(']');
			REQUIRE((begin != std::string::npos && end != std::string::npos && end > begin));
			return result.Output.substr(begin + 1, end - begin - 1);
		};

		const std::filesystem::path expected = temporary / ("StrataFallback-" + std::to_string(geteuid()));
		CHECK(find(temporary) == FileSystem::ToUTF8(expected));
		struct stat info = {};
		REQUIRE(lstat(expected.c_str(), &info) == 0);
		CHECK(S_ISDIR(info.st_mode));
		CHECK((info.st_mode & 0777) == 0700);
		CHECK(find(shared).empty());
	}
#endif

	TEST_CASE("File locks are exclusive")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("FileLock");
		const std::filesystem::path path = directory / "Test.lock";
		Scope<FileLock> lock = FileLock::Create(path);
		REQUIRE(lock);
		CHECK(FileSystem::Exists(path));
		CHECK_FALSE(FileLock::Create(path));     // It exists already
		CHECK_FALSE(FileLock::TryAcquire(path)); // Held, also from within this process

		lock.reset();
		const Scope<FileLock> again = FileLock::TryAcquire(path);
		CHECK(again);
		CHECK_FALSE(FileLock::TryAcquire(directory / "Missing.lock"));

		// Create never replaces an existing file, locked or not.
		const std::filesystem::path existing = directory / "Existing.lock";
		REQUIRE(FileSystem::WriteText(existing, "Kept"));
		CHECK_FALSE(FileLock::Create(existing));
		CHECK(FileSystem::ReadText(existing) == std::optional<std::string>("Kept"));
		CHECK(FileLock::TryAcquire(existing));
		// No temporary files are left behind.
		size_t entries = 0;
		std::error_code error;
		for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error))
			entries++;
		CHECK_FALSE(error);
		CHECK(entries == 2);
	}

	TEST_CASE("File locks are released when their process ends")
	{
		const std::filesystem::path path = Tests::CreateTemporaryDirectory("FileLockProcess") / "Owner.lock";
		REQUIRE(FileLock::Create(path)); // Released right away; the child takes it

		Process holder;
		REQUIRE(holder.Start(HelperProcess({ "--strata-test-helper=hold-file-lock", FileSystem::ToUTF8(path) })));
		std::string output;
		REQUIRE(Tests::WaitUntil([&]()
		{
			output += holder.TakeOutput();
			return output.find("locked") != std::string::npos || !holder.IsRunning();
		}, std::chrono::milliseconds(30000)));
		REQUIRE(output.find("locked") != std::string::npos);
		CHECK_FALSE(FileLock::TryAcquire(path));

		// The holder ends without releasing anything (as in a crash): the system releases the lock.
		CHECK(holder.Terminate());
		CHECK(Tests::WaitUntil([&]() { return FileLock::TryAcquire(path) != nullptr; }, std::chrono::milliseconds(10000)));
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

	TEST_CASE("Process captures stdout and stderr separately on request")
	{
		ProcessSpecification specification = HelperProcess({ "--strata-test-helper=split-output" });
		specification.Output = ProcessOutputMode::CaptureSeparate;
		Process::RunResult separate = Process::Run(specification);
		REQUIRE_MESSAGE(separate.Started, separate.Error);
		CHECK(separate.ExitCode == 0);
		CHECK(separate.Output.find("to-stdout") != std::string::npos);
		CHECK(separate.Output.find("to-stderr") == std::string::npos);
		CHECK(separate.ErrorOutput.find("to-stderr") != std::string::npos);
		CHECK(separate.ErrorOutput.find("to-stdout") == std::string::npos);

		// Merged by default.
		Process::RunResult merged = Process::Run(HelperProcess({ "--strata-test-helper=split-output" }));
		CHECK(merged.Output.find("to-stdout") != std::string::npos);
		CHECK(merged.Output.find("to-stderr") != std::string::npos);
		CHECK(merged.ErrorOutput.empty());
	}

	TEST_CASE("Process pipes input to the child")
	{
		ProcessSpecification specification = HelperProcess({ "--strata-test-helper=cat" });
		specification.PipeInput = true;
		specification.Output = ProcessOutputMode::CaptureSeparate;
		Process process;
		REQUIRE_MESSAGE(process.Start(specification), process.GetLastError());

		// A conversation: the child answers each line while its input stays open.
		std::string output;
		auto waitForOutput = [&](size_t size)
		{
			return Tests::WaitUntil([&]()
			{
				output += process.TakeOutput();
				return output.size() >= size;
			}, std::chrono::milliseconds(10000));
		};
		REQUIRE(process.WriteInput("first line\n"));
		REQUIRE(waitForOutput(11));
		CHECK(output == "first line\n");

		// More than a pipe buffer holds: writing blocks until the child has read enough.
		const std::string large = std::string(1024 * 1024, 'x') + "\n";
		REQUIRE(process.WriteInput(large));
		process.CloseInput();
		CHECK(process.Wait(std::chrono::milliseconds(10000)) == 0);
		REQUIRE(waitForOutput(11 + large.size()));
		CHECK(output.size() == 11 + large.size());
		CHECK(output.substr(11) == large);
		CHECK_FALSE(process.WriteInput("after the input was closed\n"));
	}

	TEST_CASE("Writing to a child that has exited fails without ending this process")
	{
		ProcessSpecification specification = HelperProcess({ "--strata-test-helper=exit-code", "0" });
		specification.PipeInput = true;
		specification.Output = ProcessOutputMode::Discard;
		Process process;
		REQUIRE_MESSAGE(process.Start(specification), process.GetLastError());
		REQUIRE(process.Wait(std::chrono::milliseconds(10000)) == 0);
		// The child never read its input; with its end closed, the write fails (on POSIX without a fatal SIGPIPE).
		CHECK_FALSE(process.WriteInput(std::string(256 * 1024, 'x')));
		CHECK_FALSE(process.WriteInput("again"));
	}

	TEST_CASE("Piped input needs captured or discarded output")
	{
		ProcessSpecification specification = HelperProcess({ "--strata-test-helper=exit-code", "0" });
		specification.PipeInput = true;
		specification.Output = ProcessOutputMode::Inherit;
		Process process;
		CHECK_FALSE(process.Start(specification));
		CHECK(process.GetLastError().find("Piped input") != std::string::npos);

		// Without piped input, there is nothing to write to.
		Process plain;
		REQUIRE(plain.Start(HelperProcess({ "--strata-test-helper=exit-code", "0" })));
		CHECK_FALSE(plain.WriteInput("x"));
		plain.CloseInput();
		CHECK(plain.Wait(std::chrono::milliseconds(10000)) == 0);
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

#if defined(ST_PLATFORM_POSIX)
	TEST_CASE("CrashGuard leaves the signal mask as it was")
	{
		// The guard does not save the mask on every call; after a fault the delivered signal must be unblocked again.
		sigset_t before;
		REQUIRE(pthread_sigmask(SIG_BLOCK, nullptr, &before) == 0);
		for (int attempt = 0; attempt < 2; attempt++)
		{
			CrashInfo info;
			CHECK_FALSE(CrashGuard::Invoke(WriteToNull, nullptr, &info));
#if !defined(__aarch64__) && !defined(_M_ARM64)
			int divisor = 0;
			CHECK_FALSE(CrashGuard::Invoke(DivideByZero, &divisor, &info));
#endif
			sigset_t after;
			REQUIRE(pthread_sigmask(SIG_BLOCK, nullptr, &after) == 0);
			for (int signal = 1; signal < NSIG; signal++)
			{
				INFO("Signal ", signal);
				CHECK(sigismember(&after, signal) == sigismember(&before, signal));
			}
		}
	}

	TEST_CASE("Faults outside guarded calls go to the handler installed before the guard, every time")
	{
		const Process::RunResult result = Process::Run(HelperProcess({ "--strata-test-helper=signal-chaining" }), std::chrono::milliseconds(60000));
		INFO("Output: ", result.Output);
		REQUIRE(result.Started);
		CHECK_FALSE(result.TimedOut);
		CHECK(result.ExitCode == 0);
	}

	TEST_CASE("Signals another process sends during a guarded call are not contained")
	{
		const Process::RunResult result = Process::Run(HelperProcess({ "--strata-test-helper=external-signal" }), std::chrono::milliseconds(60000));
		INFO("Output: ", result.Output);
		REQUIRE(result.Started);
		CHECK_FALSE(result.TimedOut);
		CHECK(result.ExitCode == 128 + SIGFPE);
		CHECK(result.Output.find("contained") == std::string::npos);
	}

	TEST_CASE("abort() in guarded code is reported and ends the process")
	{
		// It cannot be contained safely (the C library may hold allocator locks), so it must neither be swallowed nor hang.
		const Process::RunResult result = Process::Run(HelperProcess({ "--strata-test-helper=guarded-abort" }), std::chrono::milliseconds(60000));
		INFO("Output: ", result.Output);
		REQUIRE(result.Started);
		CHECK_FALSE(result.TimedOut);
		CHECK(result.ExitCode == 128 + SIGABRT);
		CHECK(result.Output.find("called abort()") != std::string::npos);
		CHECK(result.Output.find("contained") == std::string::npos);
	}
#endif

#if defined(ST_PLATFORM_LINUX)
	TEST_CASE("Thread cancellation passes through guarded calls")
	{
		// glibc ends a thread (pthread_exit, pthread_cancel) by unwinding it with an exception that must not be swallowed.
		const Process::RunResult result = Process::Run(HelperProcess({ "--strata-test-helper=guarded-thread-exit" }), std::chrono::milliseconds(60000));
		INFO("Output: ", result.Output);
		REQUIRE(result.Started);
		CHECK(result.ExitCode == 0);
		CHECK(result.Output.find("exited") != std::string::npos);
	}
#endif

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
