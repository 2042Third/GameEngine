#include <doctest/doctest.h>

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Process.h"
#include "TestHelpers.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#if defined(ST_PLATFORM_POSIX)
	#include <sys/stat.h>
#elif defined(ST_PLATFORM_WINDOWS)
	#include <Windows.h>
	#include <aclapi.h>
	#include <sddl.h>
#endif

using namespace Strata;

namespace
{
#if defined(ST_PLATFORM_WINDOWS)
	// Replaces the DACL of path with one that lets Everyone modify it (the owner and SYSTEM keep full access, so the
	// test can still clean up).
	bool GrantEveryoneModify(const std::filesystem::path& path)
	{
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)(A;;FA;;;SY)(A;;0x1301bf;;;WD)", SDDL_REVISION_1, &descriptor, nullptr))
			return false;

		BOOL present = FALSE;
		BOOL defaulted = FALSE;
		PACL dacl = nullptr;
		const BOOL found = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted);
		const DWORD result = found && present ? SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr) : ERROR_INVALID_DATA;
		LocalFree(descriptor);
		return result == ERROR_SUCCESS;
	}
#endif

	ProcessSpecification HelperProcess(std::vector<std::string> arguments)
	{
		ProcessSpecification specification;
		specification.Executable = Tests::GetTestExecutablePath();
		specification.Arguments = std::move(arguments);
		specification.Output = ProcessOutputMode::Discard;
		return specification;
	}

	size_t CountEntries(const std::filesystem::path& directory)
	{
		size_t count = 0;
		std::error_code error;
		for (std::filesystem::directory_iterator it(directory, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
			count++;
		return count;
	}
}

TEST_SUITE("Core.Platform")
{
	TEST_CASE("Secure random bytes come from the system generator")
	{
		std::array<uint8_t, 32> first = {};
		std::array<uint8_t, 32> second = {};
		REQUIRE(Platform::GenerateSecureRandom(first));
		REQUIRE(Platform::GenerateSecureRandom(second));
		CHECK(first != second);
		CHECK(first != std::array<uint8_t, 32> {});
		CHECK(Platform::GenerateSecureRandom(std::span<uint8_t>()));
	}

	TEST_CASE("Process liveness and start time follow the process lifetime")
	{
		CHECK(Platform::IsProcessAlive(Platform::GetProcessID()));
		CHECK_FALSE(Platform::IsProcessAlive(0));
		CHECK_FALSE(Platform::GetProcessStartTime(0).has_value());

		const std::optional<uint64_t> ownStart = Platform::GetProcessStartTime(Platform::GetProcessID());
		REQUIRE(ownStart.has_value());
		CHECK(Platform::GetProcessStartTime(Platform::GetProcessID()) == ownStart);

		Process sleeper;
		REQUIRE(sleeper.Start(HelperProcess({ "--strata-test-helper=sleep", "10000" })));
		const uint32_t sleeperId = sleeper.GetProcessID();
		CHECK(Platform::IsProcessAlive(sleeperId));
		const std::optional<uint64_t> sleeperStart = Platform::GetProcessStartTime(sleeperId);
		REQUIRE(sleeperStart.has_value());
		CHECK(sleeperStart != ownStart);
		CHECK(Platform::GetProcessStartTime(sleeperId) == sleeperStart);
		REQUIRE(sleeper.Terminate());

		// Once a process has exited, its instance is gone. On Windows the Process object's handle keeps the id from
		// being reused; on POSIX waiting for the process reaped it and freed the id, which a new process may take, so
		// the start time is what identifies the instance that exited.
		auto instanceGone = [](uint32_t processId, std::optional<uint64_t> startTime)
		{
			return !Platform::IsProcessAlive(processId) || Platform::GetProcessStartTime(processId) != startTime;
		};
		CHECK(instanceGone(sleeperId, sleeperStart));

		Process exited;
		REQUIRE(exited.Start(HelperProcess({ "--strata-test-helper=exit-code", "0" })));
		// Queried before waiting: until it is waited for, an exited child (or a Windows process with an open handle)
		// still has its start time.
		const std::optional<uint64_t> exitedStart = Platform::GetProcessStartTime(exited.GetProcessID());
		REQUIRE(exited.Wait(std::chrono::milliseconds(10000)).has_value());
		CHECK(exitedStart.has_value());
		CHECK(instanceGone(exited.GetProcessID(), exitedStart));
	}

	TEST_CASE("Small files are read through a single handle")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ReadRegularFile");
		REQUIRE(FileSystem::WriteText(directory / "small.json", "{\"a\":1}"));

		std::string error;
		const std::optional<std::string> contents = Platform::ReadRegularFile(directory / "small.json", 64, &error);
		REQUIRE_MESSAGE(contents.has_value(), error);
		CHECK(*contents == "{\"a\":1}");
		CHECK(Platform::ReadRegularFile(directory / "small.json", 7).value() == "{\"a\":1}");

		CHECK_FALSE(Platform::ReadRegularFile(directory / "small.json", 6, &error).has_value());
		CHECK(error.find("larger") != std::string::npos);
		CHECK_FALSE(Platform::ReadRegularFile(directory / "missing.json", 64).has_value());
		CHECK_FALSE(Platform::ReadRegularFile(directory, 64).has_value());

		REQUIRE(FileSystem::WriteText(directory / "empty.json", ""));
		CHECK(Platform::ReadRegularFile(directory / "empty.json", 64).value().empty());

#if defined(ST_PLATFORM_POSIX)
		// A link to a regular file and a FIFO are both refused, the FIFO without blocking.
		std::error_code linkError;
		std::filesystem::create_symlink(directory / "small.json", directory / "link.json", linkError);
		REQUIRE_FALSE(linkError);
		CHECK_FALSE(Platform::ReadRegularFile(directory / "link.json", 64).has_value());

		const std::filesystem::path fifo = directory / "fifo.json";
		REQUIRE(mkfifo(fifo.c_str(), 0600) == 0);
		CHECK_FALSE(Platform::ReadRegularFile(fifo, 64, &error).has_value());
		CHECK(error.find("not a regular file") != std::string::npos);
#endif
	}

	TEST_CASE("Private files are replaced atomically and trusted")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("PrivateFiles");
		const std::filesystem::path path = directory / "nested" / "secret.json";

		std::string error;
		REQUIRE_MESSAGE(Platform::WritePrivateFile(path, "first", &error), error);
		CHECK(FileSystem::ReadText(path).value() == "first");
		REQUIRE_MESSAGE(Platform::WritePrivateFile(path, "second contents", &error), error);
		CHECK(FileSystem::ReadText(path).value() == "second contents");
		REQUIRE(Platform::WritePrivateFile(path, "", &error));
		CHECK(FileSystem::ReadText(path).value().empty());

		// No temporary files are left behind.
		CHECK(CountEntries(directory / "nested") == 1);
		CHECK(Platform::IsTrustedFile(path, &error));

#if defined(ST_PLATFORM_POSIX)
		std::error_code statusError;
		const std::filesystem::perms permissions = std::filesystem::status(path, statusError).permissions();
		CHECK((permissions & std::filesystem::perms::all) == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
#endif

		CHECK_FALSE(Platform::WritePrivateFile(directory / "nested", "a directory cannot be replaced", &error));
		CHECK_FALSE(error.empty());
	}

	TEST_CASE("Untrusted files are rejected")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("TrustedFiles");
		std::string error;
		CHECK_FALSE(Platform::IsTrustedFile(directory / "missing.json", &error));
		CHECK_FALSE(error.empty());
		CHECK_FALSE(Platform::IsTrustedFile(directory, &error));

#if defined(ST_PLATFORM_POSIX)
		const std::filesystem::path shared = directory / "shared.json";
		REQUIRE(Platform::WritePrivateFile(shared, "{}"));
		std::filesystem::permissions(shared, std::filesystem::perms::group_write, std::filesystem::perm_options::add);
		CHECK_FALSE(Platform::IsTrustedFile(shared, &error));

		const std::filesystem::path link = directory / "link.json";
		std::error_code linkError;
		std::filesystem::create_symlink(shared, link, linkError);
		REQUIRE_FALSE(linkError);
		CHECK_FALSE(Platform::IsTrustedFile(link, &error));
#endif

#if defined(ST_PLATFORM_WINDOWS)
		const std::filesystem::path shared = directory / "shared.json";
		REQUIRE(Platform::WritePrivateFile(shared, "{}"));
		REQUIRE_MESSAGE(Platform::IsTrustedFile(shared, &error), error);
		REQUIRE(GrantEveryoneModify(shared));
		CHECK_FALSE(Platform::IsTrustedFile(shared, &error));
		CHECK(error.find("another account") != std::string::npos);
#endif
	}

	TEST_CASE("Private directories are created owner-only and verified")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("PrivateDirectories");
		const std::filesystem::path directory = root / "a" / "Sessions";

		std::string error;
		REQUIRE_MESSAGE(Platform::EnsurePrivateDirectory(directory, &error), error);
		CHECK(FileSystem::IsDirectory(directory));
		CHECK(Platform::EnsurePrivateDirectory(directory, &error)); // Idempotent

		REQUIRE(FileSystem::WriteText(root / "file.txt", "x"));
		CHECK_FALSE(Platform::EnsurePrivateDirectory(root / "file.txt", &error));
		CHECK_FALSE(error.empty());

#if defined(ST_PLATFORM_POSIX)
		std::error_code statusError;
		CHECK((std::filesystem::status(directory, statusError).permissions() & std::filesystem::perms::all) == std::filesystem::perms::owner_all);

		// An owned directory that others can write to is tightened.
		const std::filesystem::path shared = root / "Shared";
		REQUIRE(FileSystem::CreateDirectories(shared));
		std::filesystem::permissions(shared, std::filesystem::perms::all, std::filesystem::perm_options::replace);
		REQUIRE(Platform::EnsurePrivateDirectory(shared, &error));
		CHECK((std::filesystem::status(shared, statusError).permissions() & (std::filesystem::perms::group_write | std::filesystem::perms::others_write)) == std::filesystem::perms::none);

		// A symbolic link is never trusted, even to a private directory.
		const std::filesystem::path link = root / "Link";
		std::error_code linkError;
		std::filesystem::create_directory_symlink(directory, link, linkError);
		REQUIRE_FALSE(linkError);
		CHECK_FALSE(Platform::EnsurePrivateDirectory(link, &error));
#endif

#if defined(ST_PLATFORM_WINDOWS)
		// A directory other accounts may modify is refused.
		const std::filesystem::path shared = root / "Shared";
		REQUIRE(Platform::EnsurePrivateDirectory(shared, &error));
		REQUIRE(GrantEveryoneModify(shared));
		CHECK_FALSE(Platform::EnsurePrivateDirectory(shared, &error));
		CHECK(error.find("another account") != std::string::npos);
#endif
	}

	TEST_CASE("Renaming without replacing")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("RenameNoReplace");
		REQUIRE(FileSystem::WriteText(directory / "a.txt", "a"));
		REQUIRE(FileSystem::WriteText(directory / "b.txt", "b"));

		CHECK_FALSE(Platform::RenameNoReplace(directory / "a.txt", directory / "b.txt"));
		CHECK(FileSystem::ReadText(directory / "a.txt").value() == "a");
		CHECK(FileSystem::ReadText(directory / "b.txt").value() == "b");

		CHECK(Platform::RenameNoReplace(directory / "a.txt", directory / "c.txt"));
		CHECK_FALSE(FileSystem::Exists(directory / "a.txt"));
		CHECK(FileSystem::ReadText(directory / "c.txt").value() == "a");
	}

	TEST_CASE("The per-user data directory has no shared fallback")
	{
		const std::optional<std::filesystem::path> directory = Platform::FindUserDataDirectory("StrataTests");
		REQUIRE(directory.has_value());
		CHECK(FileSystem::IsDirectory(*directory));
		CHECK(*directory == Platform::GetUserDataDirectory("StrataTests"));
	}
}
