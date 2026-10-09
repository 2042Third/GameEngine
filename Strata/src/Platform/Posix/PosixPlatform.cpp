#include "stpch.h"
#include "Strata/Core/Platform.h"

#include "Strata/Core/FileSystem.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#if defined(ST_PLATFORM_MACOS)
	#include <mach-o/dyld.h>
	#include <mach/mach.h>
	#include <sys/mount.h>
	#include <sys/param.h>
	#include <sys/sysctl.h>
	#include <sys/types.h>
#else
	#include <sys/statvfs.h>
#endif

namespace Strata
{

	namespace
	{

		// The directory exists (as a directory; a symbolic link only if `followLinks`), belongs to this user and is
		// writable by nobody else, so other users cannot add, replace or rename entries in it.
		bool IsPrivateDirectory(const std::filesystem::path& path, bool followLinks)
		{
			struct stat info = {};
			const int result = followLinks ? stat(path.c_str(), &info) : lstat(path.c_str(), &info);
			return result == 0 && S_ISDIR(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & (S_IWGRP | S_IWOTH)) == 0;
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

		// Creates `directory` with mode 0700 if missing. An existing real directory of this user loses group and other
		// write permission (left by a permissive umask); anything else is refused by the caller's checks.
		bool CreateOwnDirectory(const std::filesystem::path& directory)
		{
			if (mkdir(directory.c_str(), 0700) == 0)
				return true;
			if (errno != EEXIST)
				return false;
			struct stat info = {};
			if (lstat(directory.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && info.st_uid == geteuid() && (info.st_mode & (S_IWGRP | S_IWOTH)) != 0)
				chmod(directory.c_str(), info.st_mode & 0777 & ~static_cast<mode_t>(S_IWGRP | S_IWOTH));
			return true;
		}

		// <base>/<applicationName>, created with mode 0700 where missing, or empty unless both pass IsPrivateDirectory.
		// The base may be a link (a relocated cache directory, say); the application's directory must be a real one.
		std::filesystem::path PreparePrivateSubdirectory(const std::filesystem::path& base, std::string_view applicationName)
		{
			if (base.empty() || !base.is_absolute())
				return {};
			// A missing base is created (e.g. ~/.cache of a new account); an existing one is only checked.
			if (mkdir(base.c_str(), 0700) != 0 && errno != EEXIST)
				return {};
			if (!IsPrivateDirectory(base, true) || !AllowsExecution(base))
				return {};

			std::filesystem::path directory = base / FileSystem::FromUTF8(applicationName);
			if (!CreateOwnDirectory(directory) || !IsPrivateDirectory(directory, false))
				return {};
			return directory;
		}

		// <temp>/<applicationName>-<user id>, for when the user has no private location (containers running as a user
		// whose home belongs to someone else, HOME=/, a group-writable cache directory). The shared temporary directory
		// must keep users from renaming each other's entries (sticky bit) unless only this user can write to it, and the
		// directory must be a real directory of this user that nobody else can access. If another user created it first,
		// this location is unusable - never insecure.
		std::filesystem::path PrepareTemporaryDirectory(std::string_view applicationName)
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

			const std::filesystem::path directory = base / FileSystem::FromUTF8(fmt::format("{}-{}", applicationName, static_cast<uint64_t>(geteuid())));
			if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST)
				return {};
			struct stat info = {};
			if (lstat(directory.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 077) != 0)
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
		std::filesystem::path base;
		const std::optional<std::string> home = GetEnvVar("HOME");
#if defined(ST_PLATFORM_MACOS)
		if (home)
			base = std::filesystem::path(*home) / "Library" / "Application Support";
#else
		if (const std::optional<std::string> xdgDataHome = GetEnvVar("XDG_DATA_HOME"); xdgDataHome && !xdgDataHome->empty())
			base = *xdgDataHome;
		else if (home)
			base = std::filesystem::path(*home) / ".local" / "share";
#endif
		if (base.empty())
			base = std::filesystem::temp_directory_path();

		std::filesystem::path directory = base / FileSystem::FromUTF8(applicationName);
		FileSystem::CreateDirectories(directory);
		return directory;
	}

	std::filesystem::path Platform::GetUserRuntimeDirectory(std::string_view applicationName)
	{
		// An explicit location (tests, sandboxes) replaces the search; it must pass the same checks.
		if (const std::optional<std::string> configured = GetEnvVar("STRATA_RUNTIME_DIR"); configured && !configured->empty())
			return PreparePrivateSubdirectory(FileSystem::FromUTF8(*configured), applicationName);

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
			std::filesystem::path directory = PreparePrivateSubdirectory(candidate, applicationName);
			if (!directory.empty())
				return directory;
		}
		return PrepareTemporaryDirectory(applicationName);
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

}
