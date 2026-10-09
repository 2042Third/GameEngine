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
#include <unistd.h>

#if defined(ST_PLATFORM_MACOS)
	#include <libproc.h>
	#include <mach-o/dyld.h>
	#include <mach/mach.h>
	#include <sys/proc_info.h>
	#include <sys/sysctl.h>
#else
	#include <sys/random.h>
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

	bool Platform::EnsurePrivateDirectory(const std::filesystem::path& directory, std::string* error)
	{
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

		// Other users could plant or replace files in a group- or world-writable directory. Since the directory is
		// ours, tighten it instead of failing; files planted earlier are still rejected by ReadTrustedFile.
		if ((information.st_mode & (S_IWGRP | S_IWOTH)) != 0 && chmod(directory.c_str(), S_IRWXU) != 0)
			return SetError(error, fmt::format("'{}' is writable by other users and its permissions cannot be fixed: {}", name, GetErrorMessage(errno)));
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
