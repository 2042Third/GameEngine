#include "stpch.h"
#include "Strata/Core/Platform.h"

#include "Strata/Core/FileSystem.h"

#include <cstdlib>
#include <fstream>
#include <pthread.h>
#include <unistd.h>

#if defined(ST_PLATFORM_MACOS)
	#include <mach-o/dyld.h>
	#include <mach/mach.h>
	#include <sys/sysctl.h>
	#include <sys/types.h>
#endif

namespace Strata
{

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
