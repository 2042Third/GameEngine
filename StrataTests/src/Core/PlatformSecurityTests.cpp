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
	#include "Platform/Windows/WindowsFileSecurity.h"

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

	std::vector<uint8_t> ParseSid(const wchar_t* text)
	{
		PSID sid = nullptr;
		if (!ConvertStringSidToSidW(text, &sid))
			return {};
		const uint8_t* bytes = static_cast<const uint8_t*>(sid);
		std::vector<uint8_t> result(bytes, bytes + GetLengthSid(sid));
		LocalFree(sid);
		return result;
	}

	struct AllowedEntry
	{
		std::vector<uint8_t>* Sid;
		DWORD Mask;
	};

	// An ACL with one access-allowed entry per element.
	std::vector<uint8_t> MakeAcl(const std::vector<AllowedEntry>& entries)
	{
		DWORD size = sizeof(ACL);
		for (const AllowedEntry& entry : entries)
			size += static_cast<DWORD>(sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(entry.Sid->data()));

		std::vector<uint8_t> acl(size);
		PACL pointer = reinterpret_cast<PACL>(acl.data());
		if (!InitializeAcl(pointer, size, ACL_REVISION))
			return {};
		for (const AllowedEntry& entry : entries)
		{
			if (!AddAccessAllowedAce(pointer, ACL_REVISION, entry.Mask, entry.Sid->data()))
				return {};
		}
		return acl;
	}

	std::vector<uint8_t> GetCurrentUserSid()
	{
		std::string error;
		return WindowsFileSecurity::GetCurrentUserSid(error);
	}

	struct AccessEntry
	{
		BYTE Type = 0;
		BYTE Flags = 0;
		ACCESS_MASK Mask = 0;
		std::vector<uint8_t> Sid; // Of allow and deny entries
	};

	struct ObjectDacl
	{
		bool Read = false;
		bool Protected = false;
		std::vector<AccessEntry> Entries;
	};

	ObjectDacl ReadDacl(const std::filesystem::path& path)
	{
		ObjectDacl result;
		PACL dacl = nullptr;
		PSECURITY_DESCRIPTOR descriptor = nullptr;
		if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS)
			return result;

		SECURITY_DESCRIPTOR_CONTROL control = 0;
		DWORD revision = 0;
		result.Read = dacl && GetSecurityDescriptorControl(descriptor, &control, &revision);
		result.Protected = (control & SE_DACL_PROTECTED) != 0;
		for (DWORD index = 0; result.Read && index < dacl->AceCount; index++)
		{
			void* ace = nullptr;
			if (!GetAce(dacl, index, &ace))
			{
				result.Read = false;
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
				PSID sid = const_cast<DWORD*>(&allowed->SidStart);
				const uint8_t* bytes = static_cast<const uint8_t*>(sid);
				entry.Sid.assign(bytes, bytes + GetLengthSid(sid));
			}
			result.Entries.push_back(std::move(entry));
		}
		LocalFree(descriptor);
		return result;
	}

	// The inheritance flags of an owner-only directory's entry: everything created inside inherits it.
	constexpr BYTE c_InheritedByContents = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;

	// Only the current user has access: a protected DACL whose one entry grants the user full access, with the given
	// inheritance flags.
	void CheckOwnerOnlyDacl(const std::filesystem::path& path, BYTE inheritance)
	{
		INFO("Object: ", FileSystem::ToUTF8(path));
		const ObjectDacl dacl = ReadDacl(path);
		REQUIRE(dacl.Read);
		CHECK(dacl.Protected);
		REQUIRE(dacl.Entries.size() == 1);
		const AccessEntry& entry = dacl.Entries[0];
		CHECK(entry.Type == ACCESS_ALLOWED_ACE_TYPE);
		CHECK(entry.Flags == inheritance);
		CHECK(entry.Mask == FILE_ALL_ACCESS);
		CHECK(entry.Sid == GetCurrentUserSid());
	}

	// What an object created inside an owner-only directory gets: the directory's entry, and nothing else.
	void CheckInheritedOwnerOnlyDacl(const std::filesystem::path& path)
	{
		INFO("Object: ", FileSystem::ToUTF8(path));
		const ObjectDacl dacl = ReadDacl(path);
		REQUIRE(dacl.Read);
		REQUIRE(dacl.Entries.size() == 1);
		CHECK(dacl.Entries[0].Type == ACCESS_ALLOWED_ACE_TYPE);
		CHECK(dacl.Entries[0].Sid == GetCurrentUserSid());
		CHECK((dacl.Entries[0].Flags & INHERITED_ACE) != 0);
	}

	// A directory that other accounts may modify.
	bool CreateSharedDirectory(const std::filesystem::path& path)
	{
		return FileSystem::CreateDirectories(path) && GrantEveryoneModify(path);
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

	TEST_CASE("Trusted files are checked and read through one handle")
	{
		const std::filesystem::path directory = Tests::CreateTemporaryDirectory("ReadTrustedFile");
		const std::filesystem::path path = directory / "session.json";
		REQUIRE(Platform::WritePrivateFile(path, "{\"b\":2}"));

		std::string error;
		const std::optional<std::string> contents = Platform::ReadTrustedFile(path, 64, &error);
		REQUIRE_MESSAGE(contents.has_value(), error);
		CHECK(*contents == "{\"b\":2}");
		CHECK_FALSE(Platform::ReadTrustedFile(path, 3, &error).has_value());
		CHECK(error.find("larger") != std::string::npos);
		CHECK_FALSE(Platform::ReadTrustedFile(directory / "missing.json", 64).has_value());
		CHECK_FALSE(Platform::ReadTrustedFile(directory, 64).has_value());

		// A file others may modify can still be read as plain data, but it is not trusted.
#if defined(ST_PLATFORM_POSIX)
		std::filesystem::permissions(path, std::filesystem::perms::others_write, std::filesystem::perm_options::add);
		CHECK_FALSE(Platform::ReadTrustedFile(path, 64, &error).has_value());
		CHECK(error.find("writable by other users") != std::string::npos);
#elif defined(ST_PLATFORM_WINDOWS)
		REQUIRE(GrantEveryoneModify(path));
		CHECK_FALSE(Platform::ReadTrustedFile(path, 64, &error).has_value());
		CHECK(error.find("another account") != std::string::npos);
#endif
		CHECK(Platform::ReadRegularFile(path, 64).value() == "{\"b\":2}");
	}

#if defined(ST_PLATFORM_LINUX)
	TEST_CASE("Files that report no size are read to their end")
	{
		// /proc files report a size of 0 but have contents.
		std::string error;
		const std::optional<std::string> stat = Platform::ReadRegularFile("/proc/self/stat", 64 * 1024, &error);
		REQUIRE_MESSAGE(stat.has_value(), error);
		CHECK(stat->find(')') != std::string::npos);
		CHECK(stat->size() > 50);
	}

	TEST_CASE("The start time is read whatever the process is called")
	{
		// /proc/<pid>/stat shows the process name in parentheses, unescaped, so the name may add a ')' or even a
		// newline to the line. The helper takes such a name only when told to, so its start time is first read
		// while the name is still plain.
		const std::filesystem::path trigger = Tests::CreateTemporaryDirectory("ProcessName") / "rename";
		Process child;
		REQUIRE(child.Start(HelperProcess({ "--strata-test-helper=rename-when-file-exists", FileSystem::ToUTF8(trigger) })));
		struct TerminateOnExit
		{
			Process& Child;
			~TerminateOnExit() { Child.Terminate(); }
		} terminateOnExit { child };

		const uint32_t childId = child.GetProcessID();
		const std::optional<uint64_t> plainNameStart = Platform::GetProcessStartTime(childId);
		REQUIRE(plainNameStart.has_value());

		REQUIRE(FileSystem::WriteText(trigger, "rename"));
		const std::filesystem::path nameFile = "/proc/" + std::to_string(childId) + "/comm";
		REQUIRE(Tests::WaitUntil([&]() { return Platform::ReadRegularFile(nameFile, 4096).value_or(std::string()) == "a)\nb\n"; }));

		CHECK(Platform::GetProcessStartTime(childId) == plainNameStart);
	}
#endif

#if defined(ST_PLATFORM_WINDOWS)
	TEST_CASE("Objects owned by the user, Administrators or SYSTEM are trusted whatever the elevation")
	{
		std::string error;
		std::vector<uint8_t> user = WindowsFileSecurity::GetCurrentUserSid(error);
		REQUIRE_MESSAGE(!user.empty(), error);
		std::vector<uint8_t> administrators = WindowsFileSecurity::MakeWellKnownSid(WinBuiltinAdministratorsSid);
		std::vector<uint8_t> system = WindowsFileSecurity::MakeWellKnownSid(WinLocalSystemSid);
		std::vector<uint8_t> everyone = WindowsFileSecurity::MakeWellKnownSid(WinWorldSid);
		std::vector<uint8_t> otherUser = ParseSid(L"S-1-5-21-1111111111-2222222222-3333333333-1001");
		REQUIRE_FALSE(administrators.empty());
		REQUIRE_FALSE(system.empty());
		REQUIRE_FALSE(everyone.empty());
		REQUIRE_FALSE(otherUser.empty());

		std::vector<uint8_t> privateAcl = MakeAcl({ { &user, FILE_ALL_ACCESS }, { &system, FILE_ALL_ACCESS }, { &administrators, FILE_ALL_ACCESS } });
		REQUIRE_FALSE(privateAcl.empty());
		PACL acl = reinterpret_cast<PACL>(privateAcl.data());

		// What an elevated editor or CLI creates is owned by Administrators; a later run without elevation (this
		// test usually runs without it) must still accept it. SYSTEM can take any file anyway.
		for (std::vector<uint8_t>* owner : { &user, &administrators, &system })
			CHECK_MESSAGE(WindowsFileSecurity::CheckOwnerAndAccess(owner->data(), acl, user.data(), "object", &error), error);
		CHECK_FALSE(WindowsFileSecurity::CheckOwnerAndAccess(otherUser.data(), acl, user.data(), "object", &error));
		CHECK(error.find("owned by another account") != std::string::npos);
		CHECK_FALSE(WindowsFileSecurity::CheckOwnerAndAccess(nullptr, acl, user.data(), "object", &error));

		// Others may read, but not modify.
		std::vector<uint8_t> readable = MakeAcl({ { &user, FILE_ALL_ACCESS }, { &everyone, FILE_GENERIC_READ | FILE_GENERIC_EXECUTE } });
		REQUIRE_FALSE(readable.empty());
		CHECK(WindowsFileSecurity::CheckOwnerAndAccess(administrators.data(), reinterpret_cast<PACL>(readable.data()), user.data(), "object", &error));
		for (const DWORD mask : { DWORD(FILE_WRITE_DATA), DWORD(FILE_APPEND_DATA), DWORD(DELETE), DWORD(WRITE_DAC), DWORD(WRITE_OWNER), DWORD(GENERIC_WRITE), DWORD(GENERIC_ALL) })
		{
			CAPTURE(mask);
			std::vector<uint8_t> writable = MakeAcl({ { &user, FILE_ALL_ACCESS }, { &otherUser, mask } });
			REQUIRE_FALSE(writable.empty());
			CHECK_FALSE(WindowsFileSecurity::CheckOwnerAndAccess(user.data(), reinterpret_cast<PACL>(writable.data()), user.data(), "object", &error));
			CHECK(error.find("modified by another account") != std::string::npos);
		}
		CHECK_FALSE(WindowsFileSecurity::CheckOwnerAndAccess(user.data(), nullptr, user.data(), "object", &error));
	}

	TEST_CASE("Only reparse points that redirect elsewhere count as links")
	{
		CHECK(WindowsFileSecurity::IsNameSurrogate(DWORD(FILE_ATTRIBUTE_REPARSE_POINT), DWORD(IO_REPARSE_TAG_SYMLINK)));
		CHECK(WindowsFileSecurity::IsNameSurrogate(DWORD(FILE_ATTRIBUTE_REPARSE_POINT), DWORD(IO_REPARSE_TAG_MOUNT_POINT)));
		// Cloud placeholders (OneDrive) and deduplicated files hold the file's own data.
		CHECK_FALSE(WindowsFileSecurity::IsNameSurrogate(DWORD(FILE_ATTRIBUTE_REPARSE_POINT), DWORD(IO_REPARSE_TAG_CLOUD_6)));
		CHECK_FALSE(WindowsFileSecurity::IsNameSurrogate(DWORD(FILE_ATTRIBUTE_REPARSE_POINT), DWORD(IO_REPARSE_TAG_DEDUP)));
		CHECK_FALSE(WindowsFileSecurity::IsNameSurrogate(DWORD(FILE_ATTRIBUTE_NORMAL), DWORD(IO_REPARSE_TAG_SYMLINK)));

		// Junctions are still refused.
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("Junctions");
		const std::filesystem::path target = root / "Target";
		std::string error;
		REQUIRE_MESSAGE(Platform::EnsurePrivateDirectory(target, &error), error);
		const std::filesystem::path junction = root / "Junction";
		if (!Tests::CreateJunction(junction, target))
		{
			MESSAGE("Skipping the junction checks: this file system cannot create junctions");
			return;
		}
		CHECK_FALSE(Platform::EnsurePrivateDirectory(junction, &error));
		CHECK(error.find("link or junction") != std::string::npos);
		std::error_code removeError;
		std::filesystem::remove(junction, removeError);
	}

	TEST_CASE("Owner-only security grants the current user alone access to files and directories")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("OwnerOnlySecurity");
		std::string error;

		// A directory's entry is inherited by what is created inside it.
		WindowsFileSecurity::OwnerOnlySecurity directorySecurity;
		REQUIRE_MESSAGE(directorySecurity.Initialize(WindowsFileSecurity::OwnerOnlyObject::Directory, error), error);
		const std::filesystem::path directory = root / "Directory";
		REQUIRE(CreateDirectoryW(directory.c_str(), directorySecurity.GetAttributes()));
		CheckOwnerOnlyDacl(directory, c_InheritedByContents);
		REQUIRE(FileSystem::WriteText(directory / "Inside.txt", "inside"));
		CheckInheritedOwnerOnlyDacl(directory / "Inside.txt");

		// A file's entry has nothing to pass on.
		WindowsFileSecurity::OwnerOnlySecurity fileSecurity;
		REQUIRE_MESSAGE(fileSecurity.Initialize(WindowsFileSecurity::OwnerOnlyObject::File, error), error);
		const std::filesystem::path file = root / "File.txt";
		HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, fileSecurity.GetAttributes(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		REQUIRE(handle != INVALID_HANDLE_VALUE);
		CloseHandle(handle);
		CheckOwnerOnlyDacl(file, 0);

		// Private files are created that way.
		REQUIRE_MESSAGE(Platform::WritePrivateFile(root / "Private.json", "{}", &error), error);
		CheckOwnerOnlyDacl(root / "Private.json", 0);
	}

	TEST_CASE("Runtime and private directories grant only the current user access")
	{
		const std::filesystem::path root = Tests::CreateTemporaryDirectory("RuntimeSecurity");
		const Tests::ScopedEnvironmentVariable scopedRuntime("STRATA_RUNTIME_DIR", FileSystem::ToUTF8(root));

		// New directories get an owner-only DACL instead of inheriting the parent's entries; what is created inside
		// inherits it.
		const std::filesystem::path runtime = Platform::GetUserRuntimeDirectory("StrataSecurity");
		REQUIRE(runtime == root / "StrataSecurity");
		CheckOwnerOnlyDacl(runtime, c_InheritedByContents);
		const std::filesystem::path directory = Platform::CreatePrivateDirectory(runtime, "Private-");
		REQUIRE_FALSE(directory.empty());
		CheckOwnerOnlyDacl(directory, c_InheritedByContents);
		REQUIRE(FileSystem::WriteText(directory / "File.txt", "Private"));
		CheckInheritedOwnerOnlyDacl(directory / "File.txt");

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
			const Tests::ScopedEnvironmentVariable scopedRuntime("STRATA_RUNTIME_DIR", FileSystem::ToUTF8(shared));
			CHECK(Platform::GetUserRuntimeDirectory("StrataRefused").empty());
		}

		const Tests::ScopedEnvironmentVariable scopedRuntime("STRATA_RUNTIME_DIR", FileSystem::ToUTF8(root));
		REQUIRE(CreateSharedDirectory(root / "StrataShared"));
		CHECK(Platform::GetUserRuntimeDirectory("StrataShared").empty());

		// A junction to a private directory: the directory itself must be the user's.
		const std::filesystem::path target = Platform::GetUserRuntimeDirectory("StrataTarget");
		REQUIRE_FALSE(target.empty());
		const std::filesystem::path junction = root / "StrataJunction";
		REQUIRE(Tests::CreateJunction(junction, target));
		CHECK(Platform::GetUserRuntimeDirectory("StrataJunction").empty());
		CHECK(RemoveDirectoryW(junction.c_str()));
		CHECK(FileSystem::IsDirectory(target));
	}
#endif

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
