#include "stpch.h"
#include "Strata/Core/Platform.h"

#include "Strata/Core/BoundedRead.h"
#include "Strata/Core/Crypto.h"
#include "Strata/Core/FileSystem.h"

#include <array>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <pthread.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <system_error>
#include <time.h>
#include <unistd.h>
#include <vector>

#if defined(ST_PLATFORM_MACOS)
	#include <libproc.h>
	#include <mach-o/dyld.h>
	#include <mach/mach.h>
	#include <sys/mount.h>
	#include <sys/param.h>
	#include <sys/proc_info.h>
	#include <sys/sysctl.h>
	#include <sys/types.h>
#else
	#include <sys/random.h>
	#include <sys/statvfs.h>
#endif

namespace Strata
{

	namespace
	{

		// Temporary names are random; a name that already exists (planted by someone else) is skipped.
		constexpr int c_TemporaryNameAttempts = 8;
#if !defined(ST_PLATFORM_MACOS)
		// /proc/<pid>/stat is a few hundred bytes; the limit only guards against something unexpected.
		constexpr size_t c_MaxProcStatSize = 64 * 1024;
#endif

		// Thread-safe, unlike std::strerror.
		std::string GetErrorMessage(int errorCode)
		{
			return std::generic_category().message(errorCode);
		}

		bool SetError(std::string* error, std::string message)
		{
			if (error)
				*error = std::move(message);
			return false;
		}

		// A file or directory that only the current user can modify: owned by it, and not writable by its group or
		// by others.
		bool CheckOwnedAndPrivate(const struct stat& information, const std::string& name, std::string* error)
		{
			if (information.st_uid != geteuid())
				return SetError(error, fmt::format("'{}' is owned by another user", name));
			if ((information.st_mode & (S_IWGRP | S_IWOTH)) != 0)
				return SetError(error, fmt::format("'{}' is writable by other users", name));
			return true;
		}

		// Opens path once and does every check and the read through that descriptor, so the file cannot be swapped
		// in between. O_NOFOLLOW rejects a final symbolic link, and O_NONBLOCK keeps opening a FIFO from blocking
		// until it is identified below.
		std::optional<std::string> ReadThroughOneDescriptor(const std::filesystem::path& path, size_t maxSize, bool requireTrusted, std::string* error)
		{
			const std::string name = FileSystem::ToUTF8(path);
			const int descriptor = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
			if (descriptor < 0)
			{
				SetError(error, fmt::format("Cannot open '{}': {}", name, GetErrorMessage(errno)));
				return std::nullopt;
			}

			struct DescriptorGuard
			{
				int Descriptor;
				~DescriptorGuard() { close(Descriptor); }
			} descriptorGuard { descriptor };

			struct stat information = {};
			if (fstat(descriptor, &information) != 0 || !S_ISREG(information.st_mode))
			{
				SetError(error, fmt::format("'{}' is not a regular file", name));
				return std::nullopt;
			}
			if (requireTrusted && !CheckOwnedAndPrivate(information, name, error))
				return std::nullopt;
			if (information.st_size < 0 || static_cast<uint64_t>(information.st_size) > maxSize)
			{
				SetError(error, fmt::format("'{}' is larger than {} bytes", name, maxSize));
				return std::nullopt;
			}

			int readError = 0;
			auto readChunk = [descriptor, &readError](std::span<char> buffer) -> std::optional<size_t>
			{
				while (true)
				{
					const ssize_t result = read(descriptor, buffer.data(), buffer.size());
					if (result >= 0)
						return static_cast<size_t>(result);
					if (errno != EINTR)
					{
						readError = errno;
						return std::nullopt;
					}
				}
			};

			// The size only sizes the first buffer: files such as /proc/<pid>/stat report 0 but have contents.
			std::string contents;
			const BoundedReadStatus status = BoundedRead::ReadAll(readChunk, static_cast<size_t>(information.st_size), maxSize, contents);
			if (status == BoundedReadStatus::Failed)
			{
				SetError(error, fmt::format("Failed to read '{}': {}", name, GetErrorMessage(readError)));
				return std::nullopt;
			}
			if (status == BoundedReadStatus::TooLarge)
			{
				SetError(error, fmt::format("'{}' is larger than {} bytes", name, maxSize));
				return std::nullopt;
			}
			return contents;
		}

		// An existing directory that only the current user can modify (CheckOwnedAndPrivate), so other users cannot add,
		// replace or rename entries in it. With `followLinks` a symbolic link to such a directory counts as well.
		bool CheckPrivateDirectory(const std::filesystem::path& directory, bool followLinks, std::string* error)
		{
			const std::string name = FileSystem::ToUTF8(directory);
			struct stat information = {};
			const int result = followLinks ? stat(directory.c_str(), &information) : lstat(directory.c_str(), &information);
			if (result != 0)
				return SetError(error, fmt::format("Cannot inspect '{}': {}", name, GetErrorMessage(errno)));
			if (!S_ISDIR(information.st_mode))
				return SetError(error, fmt::format("'{}' is not a directory", name));
			return CheckOwnedAndPrivate(information, name, error);
		}

		// Script modules are loaded from copies in the runtime directory: a file system mounted noexec cannot hold it.
		bool AllowsExecution(const std::filesystem::path& path)
		{
#if defined(ST_PLATFORM_MACOS)
			struct statfs info = {};
			return statfs(path.c_str(), &info) != 0 || (info.f_flags & MNT_NOEXEC) == 0;
#else
			struct statvfs info = {};
			return statvfs(path.c_str(), &info) != 0 || (info.f_flag & ST_NOEXEC) == 0;
#endif
		}

		// <base>/<applicationName> for GetUserRuntimeDirectory: a private directory (Platform::EnsurePrivateDirectory) in a
		// base that only the current user can modify, on a file system that allows executing files. The base may be a
		// link (a relocated cache directory, say); a missing one is created, an existing one is only checked. Empty if
		// the location does not qualify.
		std::filesystem::path PrepareRuntimeDirectory(const std::filesystem::path& base, std::string_view applicationName)
		{
			if (base.empty() || !base.is_absolute())
				return {};
			if (mkdir(base.c_str(), S_IRWXU) != 0 && errno != EEXIST)
				return {};
			if (!CheckPrivateDirectory(base, true, nullptr) || !AllowsExecution(base))
				return {};

			std::filesystem::path directory = base / FileSystem::FromUTF8(applicationName);
			if (!Platform::EnsurePrivateDirectory(directory))
				return {};
			return directory;
		}

		// <temp>/<applicationName>-<user id>, for when the user has no private location (containers running as a user
		// whose home belongs to someone else, HOME=/, a group-writable cache directory). The shared temporary directory
		// must keep users from renaming each other's entries (sticky bit) unless only this user can write to it; the
		// directory itself is private (Platform::EnsurePrivateDirectory). If another user created it first, this location
		// is unusable - never insecure.
		std::filesystem::path PrepareTemporaryRuntimeDirectory(std::string_view applicationName)
		{
			std::error_code error;
			const std::filesystem::path base = std::filesystem::temp_directory_path(error);
			if (error || !base.is_absolute())
				return {};
			struct stat baseInfo = {};
			if (stat(base.c_str(), &baseInfo) != 0 || !S_ISDIR(baseInfo.st_mode) || !AllowsExecution(base))
				return {};
			if ((baseInfo.st_mode & (S_IWGRP | S_IWOTH)) != 0 && (baseInfo.st_mode & S_ISVTX) == 0)
				return {};

			std::filesystem::path directory = base / FileSystem::FromUTF8(fmt::format("{}-{}", applicationName, static_cast<uint64_t>(geteuid())));
			if (!Platform::EnsurePrivateDirectory(directory))
				return {};
			return directory;
		}

	}

	std::string_view Platform::GetName()
	{
#if defined(ST_PLATFORM_MACOS)
		return "macOS";
#else
		return "Linux";
#endif
	}

	std::filesystem::path Platform::GetExecutablePath()
	{
#if defined(ST_PLATFORM_MACOS)
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size);
		std::string buffer(size, '\0');
		if (_NSGetExecutablePath(buffer.data(), &size) != 0)
			return {};

		std::error_code error;
		std::filesystem::path path = std::filesystem::weakly_canonical(std::filesystem::path(buffer.c_str()), error);
		return error ? std::filesystem::path(buffer.c_str()) : path;
#else
		std::error_code error;
		std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", error);
		return error ? std::filesystem::path() : path;
#endif
	}

	std::filesystem::path Platform::GetExecutableDirectory()
	{
		return GetExecutablePath().parent_path();
	}

	std::filesystem::path Platform::GetUserDataDirectory(std::string_view applicationName)
	{
		if (std::optional<std::filesystem::path> directory = FindUserDataDirectory(applicationName))
			return *directory;

		std::error_code error;
		std::filesystem::path directory = std::filesystem::temp_directory_path(error) / FileSystem::FromUTF8(applicationName);
		FileSystem::CreateDirectories(directory);
		return directory;
	}

	std::optional<std::filesystem::path> Platform::FindUserDataDirectory(std::string_view applicationName)
	{
		std::filesystem::path base;
		const std::optional<std::string> home = GetEnvVar("HOME");
#if defined(ST_PLATFORM_MACOS)
		if (home && !home->empty())
			base = FileSystem::FromUTF8(*home) / "Library" / "Application Support";
#else
		// XDG requires absolute paths; a relative XDG_DATA_HOME is ignored.
		if (const std::optional<std::string> xdgDataHome = GetEnvVar("XDG_DATA_HOME"); xdgDataHome && !xdgDataHome->empty() && FileSystem::FromUTF8(*xdgDataHome).is_absolute())
			base = FileSystem::FromUTF8(*xdgDataHome);
		else if (home && !home->empty())
			base = FileSystem::FromUTF8(*home) / ".local" / "share";
#endif
		if (base.empty())
			return std::nullopt;

		std::filesystem::path directory = base / FileSystem::FromUTF8(applicationName);
		if (!FileSystem::CreateDirectories(directory))
			return std::nullopt;
		return directory;
	}

	std::optional<std::filesystem::path> Platform::FindHomeDirectory()
	{
		const std::optional<std::string> home = GetEnvVar("HOME");
		if (!home || home->empty())
			return std::nullopt;
		std::filesystem::path directory = FileSystem::FromUTF8(*home);
		if (!directory.is_absolute())
			return std::nullopt;
		return directory;
	}

	std::filesystem::path Platform::GetUserRuntimeDirectory(std::string_view applicationName)
	{
		// An explicit location (tests, sandboxes) replaces the search; it must pass the same checks.
		if (const std::optional<std::string> configured = GetEnvVar("STRATA_RUNTIME_DIR"); configured && !configured->empty())
			return PrepareRuntimeDirectory(FileSystem::FromUTF8(*configured), applicationName);

		std::vector<std::filesystem::path> candidates;
		const std::optional<std::string> home = GetEnvVar("HOME");
#if defined(ST_PLATFORM_MACOS)
		// The per-user temporary directory (what $TMPDIR names in a login session).
		const size_t size = confstr(_CS_DARWIN_USER_TEMP_DIR, nullptr, 0);
		if (size > 0)
		{
			std::string buffer(size, '\0');
			if (confstr(_CS_DARWIN_USER_TEMP_DIR, buffer.data(), buffer.size()) == size)
				candidates.push_back(FileSystem::FromUTF8(buffer.c_str()));
		}
		if (home && !home->empty())
			candidates.push_back(FileSystem::FromUTF8(*home) / "Library" / "Caches");
#else
		if (const std::optional<std::string> runtime = GetEnvVar("XDG_RUNTIME_DIR"); runtime && !runtime->empty())
			candidates.push_back(FileSystem::FromUTF8(*runtime));
		if (const std::optional<std::string> cache = GetEnvVar("XDG_CACHE_HOME"); cache && !cache->empty())
			candidates.push_back(FileSystem::FromUTF8(*cache));
		else if (home && !home->empty())
			candidates.push_back(FileSystem::FromUTF8(*home) / ".cache");
#endif

		for (const std::filesystem::path& candidate : candidates)
		{
			std::filesystem::path directory = PrepareRuntimeDirectory(candidate, applicationName);
			if (!directory.empty())
				return directory;
		}
		return PrepareTemporaryRuntimeDirectory(applicationName);
	}

	std::filesystem::path Platform::CreatePrivateDirectory(const std::filesystem::path& parent, std::string_view prefix)
	{
		// mkdtemp picks an unused name and creates the directory with mode 0700 in one step.
		std::string pattern = (parent / FileSystem::FromUTF8(fmt::format("{}XXXXXX", prefix))).string();
		if (!mkdtemp(pattern.data()))
			return {};
		return std::filesystem::path(pattern);
	}

	bool Platform::IsDebuggerAttached()
	{
#if defined(ST_PLATFORM_MACOS)
		int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
		struct kinfo_proc info = {};
		size_t size = sizeof(info);
		if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0)
			return false;
		return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
		std::ifstream status("/proc/self/status");
		std::string line;
		while (std::getline(status, line))
		{
			constexpr std::string_view prefix = "TracerPid:";
			if (line.compare(0, prefix.size(), prefix) == 0)
				return std::atoi(line.c_str() + prefix.size()) != 0;
		}
		return false;
#endif
	}

	void Platform::SetCurrentThreadName(std::string_view name)
	{
#if defined(ST_PLATFORM_MACOS)
		const std::string threadName(name.substr(0, 63));
		pthread_setname_np(threadName.c_str());
#else
		// Linux limits thread names to 15 characters plus the terminator.
		const std::string threadName(name.substr(0, 15));
		pthread_setname_np(pthread_self(), threadName.c_str());
#endif
	}

	uint32_t Platform::GetProcessID()
	{
		return static_cast<uint32_t>(getpid());
	}

	std::optional<std::string> Platform::GetEnvVar(std::string_view name)
	{
		const std::string key(name);
		const char* value = std::getenv(key.c_str());
		if (!value)
			return std::nullopt;
		return std::string(value);
	}

	bool Platform::SetEnvVar(std::string_view name, std::string_view value)
	{
		const std::string key(name);
		const std::string text(value);
		return setenv(key.c_str(), text.c_str(), 1) == 0;
	}

	bool Platform::OpenWithDefaultApplication(const std::string& pathOrUrl)
	{
		const pid_t pid = fork();
		if (pid < 0)
			return false;
		if (pid == 0)
		{
#if defined(ST_PLATFORM_MACOS)
			execlp("open", "open", pathOrUrl.c_str(), static_cast<char*>(nullptr));
#else
			execlp("xdg-open", "xdg-open", pathOrUrl.c_str(), static_cast<char*>(nullptr));
#endif
			_exit(127);
		}
		return true;
	}

	uint64_t Platform::GetProcessMemoryUsage()
	{
#if defined(ST_PLATFORM_MACOS)
		mach_task_basic_info info = {};
		mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
		if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
			return static_cast<uint64_t>(info.resident_size);
		return 0;
#else
		std::ifstream statm("/proc/self/statm");
		uint64_t totalPages = 0;
		uint64_t residentPages = 0;
		if (statm >> totalPages >> residentPages)
			return residentPages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
		return 0;
#endif
	}

	bool Platform::IsProcessAlive(uint32_t processId)
	{
		if (processId == 0 || processId > static_cast<uint32_t>(std::numeric_limits<pid_t>::max()))
			return false;

		// Signal 0 only checks for existence. EPERM means the process exists but belongs to another user. An exited
		// child counts as running until its parent has waited for it.
		if (kill(static_cast<pid_t>(processId), 0) == 0)
			return true;
		return errno == EPERM;
	}

	std::optional<uint64_t> Platform::GetProcessStartTime(uint32_t processId)
	{
		if (processId == 0 || processId > static_cast<uint32_t>(std::numeric_limits<pid_t>::max()))
			return std::nullopt;

#if defined(ST_PLATFORM_MACOS)
		proc_bsdinfo information = {};
		const int size = proc_pidinfo(static_cast<int>(processId), PROC_PIDTBSDINFO, 0, &information, sizeof(information));
		if (size != static_cast<int>(sizeof(information)))
			return std::nullopt;
		return static_cast<uint64_t>(information.pbi_start_tvsec) * 1000000 + static_cast<uint64_t>(information.pbi_start_tvusec);
#else
		// Field 22 of /proc/<pid>/stat is the start time in clock ticks since boot. Field 2 (the command name, in
		// parentheses) may contain spaces, parentheses and even newlines, so the whole file is read and fields are
		// counted after its last ')'.
		const std::optional<std::string> content = ReadRegularFile("/proc/" + std::to_string(processId) + "/stat", c_MaxProcStatSize);
		if (!content)
			return std::nullopt;
		const size_t commandEnd = content->rfind(')');
		if (commandEnd == std::string::npos)
			return std::nullopt;

		std::istringstream fields(content->substr(commandEnd + 1));
		std::string field;
		for (int index = 3; index <= 22; index++)
		{
			if (!(fields >> field))
				return std::nullopt;
		}

		uint64_t startTime = 0;
		const auto [end, parseError] = std::from_chars(field.data(), field.data() + field.size(), startTime);
		if (parseError != std::errc() || end != field.data() + field.size())
			return std::nullopt;
		return startTime;
#endif
	}

	std::optional<double> Platform::GetProcessUptime()
	{
#if defined(ST_PLATFORM_MACOS)
		// The start time is wall-clock time (microseconds since the epoch).
		const std::optional<uint64_t> start = GetProcessStartTime(GetProcessID());
		timespec now = {};
		if (!start || clock_gettime(CLOCK_REALTIME, &now) != 0)
			return std::nullopt;
		const double current = static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1e-9;
		const double started = static_cast<double>(*start) * 1e-6;
#else
		// The start time is in clock ticks since boot, which CLOCK_BOOTTIME also counts from.
		const std::optional<uint64_t> start = GetProcessStartTime(GetProcessID());
		const long ticksPerSecond = sysconf(_SC_CLK_TCK);
		timespec now = {};
		if (!start || ticksPerSecond <= 0 || clock_gettime(CLOCK_BOOTTIME, &now) != 0)
			return std::nullopt;
		const double current = static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1e-9;
		const double started = static_cast<double>(*start) / static_cast<double>(ticksPerSecond);
#endif
		if (current < started)
			return std::nullopt;
		return current - started;
	}

	bool Platform::GenerateSecureRandom(std::span<uint8_t> buffer)
	{
#if defined(ST_PLATFORM_MACOS)
		arc4random_buf(buffer.data(), buffer.size());
		return true;
#else
		size_t offset = 0;
		while (offset < buffer.size())
		{
			const ssize_t result = getrandom(buffer.data() + offset, buffer.size() - offset, 0);
			if (result < 0)
			{
				if (errno == EINTR)
					continue;
				return false;
			}
			offset += static_cast<size_t>(result);
		}
		return true;
#endif
	}

	bool Platform::WritePrivateFile(const std::filesystem::path& path, std::string_view contents, std::string* error)
	{
		std::error_code directoryError;
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), directoryError);

		// O_EXCL never opens an existing file or symbolic link, so the data only ever lands in a file created here,
		// and it is created owner-only rather than restricted after the fact. The random name keeps other users
		// from blocking the write by creating the file first.
		std::filesystem::path temporaryPath;
		int descriptor = -1;
		int createError = 0;
		for (int attempt = 0; attempt < c_TemporaryNameAttempts && descriptor < 0; attempt++)
		{
			std::array<uint8_t, 8> suffix = {};
			if (!GenerateSecureRandom(suffix))
				return SetError(error, "The system random number generator failed");
			temporaryPath = path;
			temporaryPath += FileSystem::FromUTF8(".tmp-" + Crypto::ToHex(suffix));
			descriptor = open(temporaryPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
			createError = descriptor < 0 ? errno : 0;
			if (descriptor < 0 && createError != EEXIST)
				break;
		}
		if (descriptor < 0)
			return SetError(error, fmt::format("Failed to create '{}': {}", FileSystem::ToUTF8(temporaryPath), GetErrorMessage(createError)));

		bool written = true;
		int writeError = 0;
		size_t offset = 0;
		while (offset < contents.size())
		{
			const ssize_t result = write(descriptor, contents.data() + offset, contents.size() - offset);
			if (result < 0 && errno == EINTR)
				continue;
			if (result <= 0)
			{
				written = false;
				writeError = result < 0 ? errno : EIO;
				break;
			}
			offset += static_cast<size_t>(result);
		}
		if (close(descriptor) != 0 && written)
		{
			written = false;
			writeError = errno;
		}
		if (!written)
		{
			unlink(temporaryPath.c_str());
			return SetError(error, fmt::format("Failed to write '{}': {}", FileSystem::ToUTF8(temporaryPath), GetErrorMessage(writeError)));
		}

		if (rename(temporaryPath.c_str(), path.c_str()) != 0)
		{
			const int renameError = errno;
			unlink(temporaryPath.c_str());
			return SetError(error, fmt::format("Failed to replace '{}': {}", FileSystem::ToUTF8(path), GetErrorMessage(renameError)));
		}
		return true;
	}

	bool Platform::EnsurePrivateDirectory(const std::filesystem::path& path, std::string* error)
	{
		// With a trailing separator lstat would follow a final symbolic link.
		const std::filesystem::path directory = FileSystem::RemoveTrailingSeparators(path);
		const std::string name = FileSystem::ToUTF8(directory);
		std::error_code parentError;
		if (directory.has_parent_path())
			std::filesystem::create_directories(directory.parent_path(), parentError);
		if (mkdir(directory.c_str(), S_IRWXU) != 0 && errno != EEXIST)
			return SetError(error, fmt::format("Failed to create the directory '{}': {}", name, GetErrorMessage(errno)));

		struct stat information = {};
		if (lstat(directory.c_str(), &information) != 0)
			return SetError(error, fmt::format("Cannot inspect '{}': {}", name, GetErrorMessage(errno)));
		if (!S_ISDIR(information.st_mode))
			return SetError(error, fmt::format("'{}' is not a directory (symbolic links are not trusted)", name));
		if (information.st_uid != geteuid())
			return SetError(error, fmt::format("'{}' is owned by another user", name));

		// Other users could plant or replace files in a directory they can write to, or read what it holds. Since the
		// directory is ours, tighten it instead of failing; files planted earlier are still rejected by ReadTrustedFile.
		if ((information.st_mode & (S_IRWXG | S_IRWXO)) != 0 && chmod(directory.c_str(), S_IRWXU) != 0)
			return SetError(error, fmt::format("'{}' is accessible to other users and its permissions cannot be fixed: {}", name, GetErrorMessage(errno)));
		return true;
	}

	bool Platform::IsTrustedFile(const std::filesystem::path& path, std::string* error)
	{
		const std::string name = FileSystem::ToUTF8(path);
		struct stat information = {};
		if (lstat(path.c_str(), &information) != 0)
			return SetError(error, fmt::format("Cannot inspect '{}': {}", name, GetErrorMessage(errno)));
		if (!S_ISREG(information.st_mode))
			return SetError(error, fmt::format("'{}' is not a regular file (symbolic links are not trusted)", name));
		return CheckOwnedAndPrivate(information, name, error);
	}

	std::optional<std::string> Platform::ReadRegularFile(const std::filesystem::path& path, size_t maxSize, std::string* error)
	{
		return ReadThroughOneDescriptor(path, maxSize, false, error);
	}

	std::optional<std::string> Platform::ReadTrustedFile(const std::filesystem::path& path, size_t maxSize, std::string* error)
	{
		return ReadThroughOneDescriptor(path, maxSize, true, error);
	}

	bool Platform::RenameNoReplace(const std::filesystem::path& from, const std::filesystem::path& to)
	{
		// link() fails if the destination exists, which makes the check and the rename one atomic step.
		if (link(from.c_str(), to.c_str()) == 0)
		{
			unlink(from.c_str());
			return true;
		}
		if (errno == EEXIST)
			return false;

		// File systems without hard links: fall back to checking first (not atomic, but never replaces a file that
		// existed before the call).
		std::error_code existsError;
		if (std::filesystem::exists(to, existsError) || existsError)
			return false;
		return rename(from.c_str(), to.c_str()) == 0;
	}

	void Platform::SetBinaryStandardStreams()
	{
		// POSIX streams never translate newlines.
	}

}
