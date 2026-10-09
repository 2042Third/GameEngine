#include "stpch.h"
#include "Strata/Core/Platform.h"

#include "Strata/Core/BoundedRead.h"
#include "Strata/Core/Crypto.h"
#include "Strata/Core/FileSystem.h"

#include "Platform/Windows/WindowsFileSecurity.h"
#include "Platform/Windows/WindowsUtils.h"

#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <climits>
#include <cstdio>

namespace Strata
{

	namespace
	{

		// Replacing a file fails while a reader has it open without delete sharing (e.g. a tool polling it).
		// That is transient, so the final rename of WritePrivateFile is retried for a moment.
		constexpr int c_ReplaceAttempts = 10;
		constexpr DWORD c_ReplaceRetryDelayMilliseconds = 20;
		// Temporary names are random; a name that already exists (planted by someone else) is skipped.
		constexpr int c_TemporaryNameAttempts = 8;

		std::string GetLastErrorMessage()
		{
			return WindowsUtils::GetErrorMessage(::GetLastError());
		}

		bool SetError(std::string* error, std::string message)
		{
			if (error)
				*error = std::move(message);
			return false;
		}

		// Closes a handle when it goes out of scope.
		class HandleGuard
		{
		public:
			explicit HandleGuard(HANDLE handle)
				: m_Handle(handle)
			{
			}

			~HandleGuard()
			{
				if (m_Handle && m_Handle != INVALID_HANDLE_VALUE)
					CloseHandle(m_Handle);
			}

			HandleGuard(const HandleGuard&) = delete;
			HandleGuard& operator=(const HandleGuard&) = delete;
		private:
			HANDLE m_Handle;
		};

		enum class ObjectKind
		{
			File,
			Directory
		};

		// Opens a file or directory just to inspect it: the object itself, or with `followLink` the object a link or
		// junction leads to.
		HANDLE OpenForInspection(const std::filesystem::path& path, ObjectKind kind, bool followLink)
		{
			const DWORD flags = (followLink ? 0 : FILE_FLAG_OPEN_REPARSE_POINT) | (kind == ObjectKind::Directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
			return CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags, nullptr);
		}

		// Rejects symbolic links, junctions and other reparse points that redirect to another object. Reparse
		// points that hold the file's own data (e.g. OneDrive placeholders) are fine.
		bool CheckNotLink(HANDLE handle, const std::string& name, std::string* error)
		{
			const std::optional<bool> isLink = WindowsFileSecurity::IsNameSurrogate(handle);
			if (!isLink)
				return SetError(error, fmt::format("Cannot inspect '{}': {}", name, GetLastErrorMessage()));
			if (*isLink)
				return SetError(error, fmt::format("'{}' is a link or junction, which is not trusted", name));
			return true;
		}

		// Whether an existing file or directory only the current user can modify: of the expected kind, not a link or
		// junction (with `followLink`, the object it leads to is checked instead), owned and protected as
		// WindowsFileSecurity::CheckOwnerAndAccess requires.
		bool CheckPrivateObject(const std::filesystem::path& path, ObjectKind kind, bool followLink, std::string* error)
		{
			const std::string name = FileSystem::ToUTF8(path);
			HANDLE handle = OpenForInspection(path, kind, followLink);
			if (handle == INVALID_HANDLE_VALUE)
				return SetError(error, fmt::format("Cannot open '{}': {}", name, GetLastErrorMessage()));
			HandleGuard handleGuard(handle);

			BY_HANDLE_FILE_INFORMATION information = {};
			if (!GetFileInformationByHandle(handle, &information))
				return SetError(error, fmt::format("Cannot inspect '{}': {}", name, GetLastErrorMessage()));
			const bool isDirectory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			if (kind == ObjectKind::Directory && !isDirectory)
				return SetError(error, fmt::format("'{}' is not a directory", name));
			if (kind == ObjectKind::File && isDirectory)
				return SetError(error, fmt::format("'{}' is not a regular file", name));
			if (!CheckNotLink(handle, name, error))
				return false;
			return WindowsFileSecurity::CheckHandleOwnerAndAccess(handle, name, error);
		}

		// Opens path once and does every check and the read through that handle, so the file cannot be swapped in
		// between. Delete sharing lets writers replace the file atomically while it is being read.
		std::optional<std::string> ReadThroughOneHandle(const std::filesystem::path& path, size_t maxSize, bool requireTrusted, std::string* error)
		{
			const std::string name = FileSystem::ToUTF8(path);
			const DWORD access = GENERIC_READ | (requireTrusted ? READ_CONTROL : 0);
			HANDLE file = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				SetError(error, fmt::format("Cannot open '{}': {}", name, GetLastErrorMessage()));
				return std::nullopt;
			}
			HandleGuard fileGuard(file);

			BY_HANDLE_FILE_INFORMATION information = {};
			if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &information) || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			{
				SetError(error, fmt::format("'{}' is not a regular file", name));
				return std::nullopt;
			}
			if (!CheckNotLink(file, name, error))
				return std::nullopt;
			if (requireTrusted && !WindowsFileSecurity::CheckHandleOwnerAndAccess(file, name, error))
				return std::nullopt;

			const uint64_t size = (static_cast<uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
			if (size > maxSize)
			{
				SetError(error, fmt::format("'{}' is larger than {} bytes", name, maxSize));
				return std::nullopt;
			}

			DWORD readError = ERROR_SUCCESS;
			auto readChunk = [file, &readError](std::span<char> buffer) -> std::optional<size_t>
			{
				const DWORD chunk = static_cast<DWORD>(std::min<size_t>(buffer.size(), 1u << 30));
				DWORD bytesRead = 0;
				if (!ReadFile(file, buffer.data(), chunk, &bytesRead, nullptr))
				{
					readError = ::GetLastError();
					return std::nullopt;
				}
				return static_cast<size_t>(bytesRead);
			};

			std::string contents;
			const BoundedReadStatus status = BoundedRead::ReadAll(readChunk, static_cast<size_t>(size), maxSize, contents);
			if (status == BoundedReadStatus::Failed)
			{
				SetError(error, fmt::format("Failed to read '{}': {}", name, WindowsUtils::GetErrorMessage(readError)));
				return std::nullopt;
			}
			if (status == BoundedReadStatus::TooLarge)
			{
				SetError(error, fmt::format("'{}' is larger than {} bytes", name, maxSize));
				return std::nullopt;
			}
			return contents;
		}

		std::string MakeRandomSuffix()
		{
			std::array<uint8_t, 8> bytes = {};
			if (!Platform::GenerateSecureRandom(bytes))
				return {};
			return Crypto::ToHex(bytes);
		}

		// `directory` inside `base` for GetUserRuntimeDirectory: a private directory (Platform::EnsurePrivateDirectory) in
		// a base that only the current user can modify. The base may be a link or junction (say to a relocated folder);
		// a missing one is created, an existing one is only checked. Empty if the location does not qualify.
		std::filesystem::path PrepareRuntimeDirectory(const std::filesystem::path& base, const std::filesystem::path& directory)
		{
			if (!base.is_absolute() || !FileSystem::CreateDirectories(base) || !CheckPrivateObject(base, ObjectKind::Directory, true, nullptr))
				return {};
			if (!Platform::EnsurePrivateDirectory(directory))
				return {};
			return directory;
		}

	}

	std::string_view Platform::GetName()
	{
		return "Windows";
	}

	std::filesystem::path Platform::GetExecutablePath()
	{
		std::wstring buffer(MAX_PATH, L'\0');
		while (true)
		{
			const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
			if (length == 0)
				return {};
			if (length < buffer.size())
			{
				buffer.resize(length);
				return std::filesystem::path(buffer);
			}
			buffer.resize(buffer.size() * 2);
		}
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
		PWSTR knownFolder = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &knownFolder)))
		{
			if (knownFolder)
				CoTaskMemFree(knownFolder);
			return std::nullopt;
		}

		std::filesystem::path directory = std::filesystem::path(knownFolder) / FileSystem::FromUTF8(applicationName);
		CoTaskMemFree(knownFolder);
		if (!FileSystem::CreateDirectories(directory))
			return std::nullopt;
		return directory;
	}

	std::filesystem::path Platform::GetUserRuntimeDirectory(std::string_view applicationName)
	{
		// An explicit location (tests, sandboxes) replaces the default; it must pass the same checks.
		if (const std::optional<std::string> configured = GetEnvVar("STRATA_RUNTIME_DIR"); configured && !configured->empty())
		{
			const std::filesystem::path base = FileSystem::FromUTF8(*configured);
			return PrepareRuntimeDirectory(base, base / FileSystem::FromUTF8(applicationName));
		}

		// Local application data, which only the user (and administrators) can access. The path must be freed even when
		// the call fails.
		PWSTR knownFolder = nullptr;
		const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &knownFolder);
		const std::filesystem::path localAppData = SUCCEEDED(result) ? std::filesystem::path(knownFolder) : std::filesystem::path();
		CoTaskMemFree(knownFolder);
		if (localAppData.empty())
			return {};

		const std::filesystem::path base = localAppData / FileSystem::FromUTF8(applicationName);
		return PrepareRuntimeDirectory(base, base / "Runtime");
	}

	std::filesystem::path Platform::CreatePrivateDirectory(const std::filesystem::path& parent, std::string_view prefix)
	{
		WindowsFileSecurity::OwnerOnlySecurity security;
		std::string securityError;
		if (!security.Initialize(WindowsFileSecurity::OwnerOnlyObject::Directory, securityError))
			return {};

		// Names are random; one that already exists (planted by someone else) is skipped, never reused.
		for (int attempt = 0; attempt < c_TemporaryNameAttempts; attempt++)
		{
			const std::string suffix = MakeRandomSuffix();
			if (suffix.empty())
				return {};
			const std::filesystem::path path = parent / FileSystem::FromUTF8(fmt::format("{}{}", prefix, suffix));
			if (CreateDirectoryW(path.c_str(), security.GetAttributes()))
				return path;
			if (::GetLastError() != ERROR_ALREADY_EXISTS)
				return {};
		}
		return {};
	}

	bool Platform::IsDebuggerAttached()
	{
		return IsDebuggerPresent() != FALSE;
	}

	void Platform::SetCurrentThreadName(std::string_view name)
	{
		const std::wstring wideName = WindowsUtils::Utf8ToWide(name);
		SetThreadDescription(GetCurrentThread(), wideName.c_str());
	}

	uint32_t Platform::GetProcessID()
	{
		return static_cast<uint32_t>(::GetCurrentProcessId());
	}

	std::optional<std::string> Platform::GetEnvVar(std::string_view name)
	{
		const std::wstring wideName = WindowsUtils::Utf8ToWide(name);
		const DWORD length = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
		if (length == 0)
			return std::nullopt;

		std::wstring value(length, L'\0');
		const DWORD written = GetEnvironmentVariableW(wideName.c_str(), value.data(), length);
		value.resize(written);
		return WindowsUtils::WideToUtf8(value);
	}

	bool Platform::SetEnvVar(std::string_view name, std::string_view value)
	{
		const std::wstring wideName = WindowsUtils::Utf8ToWide(name);
		const std::wstring wideValue = WindowsUtils::Utf8ToWide(value);
		return SetEnvironmentVariableW(wideName.c_str(), wideValue.c_str()) != FALSE;
	}

	bool Platform::OpenWithDefaultApplication(const std::string& pathOrUrl)
	{
		const std::wstring target = WindowsUtils::Utf8ToWide(pathOrUrl);
		const HINSTANCE result = ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		return reinterpret_cast<INT_PTR>(result) > 32;
	}

	uint64_t Platform::GetProcessMemoryUsage()
	{
		PROCESS_MEMORY_COUNTERS counters = {};
		if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
			return static_cast<uint64_t>(counters.WorkingSetSize);
		return 0;
	}

	bool Platform::IsProcessAlive(uint32_t processId)
	{
		if (processId == 0)
			return false;

		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(processId));
		if (!process)
			return ::GetLastError() == ERROR_ACCESS_DENIED; // It exists but belongs to a more privileged account

		// The process object (and its id) outlives the process while handles to it are open, so check for exit.
		const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
		CloseHandle(process);
		return alive;
	}

	std::optional<uint64_t> Platform::GetProcessStartTime(uint32_t processId)
	{
		if (processId == 0)
			return std::nullopt;

		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processId));
		if (!process)
			return std::nullopt;
		HandleGuard processGuard(process);

		FILETIME creation = {};
		FILETIME exit = {};
		FILETIME kernel = {};
		FILETIME user = {};
		if (!GetProcessTimes(process, &creation, &exit, &kernel, &user))
			return std::nullopt;
		return (static_cast<uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
	}

	bool Platform::GenerateSecureRandom(std::span<uint8_t> buffer)
	{
		size_t offset = 0;
		while (offset < buffer.size())
		{
			const ULONG chunk = static_cast<ULONG>(std::min<size_t>(buffer.size() - offset, ULONG_MAX));
			if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buffer.data() + offset, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
				return false;
			offset += chunk;
		}
		return true;
	}

	bool Platform::WritePrivateFile(const std::filesystem::path& path, std::string_view contents, std::string* error)
	{
		std::error_code directoryError;
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), directoryError);

		WindowsFileSecurity::OwnerOnlySecurity security;
		std::string securityError;
		if (!security.Initialize(WindowsFileSecurity::OwnerOnlyObject::File, securityError))
			return SetError(error, securityError);

		// CREATE_NEW never opens an existing file or link, so the data only ever lands in a file created here. The
		// random name keeps other accounts from blocking the write by creating the file first.
		std::filesystem::path temporaryPath;
		HANDLE file = INVALID_HANDLE_VALUE;
		for (int attempt = 0; attempt < c_TemporaryNameAttempts && file == INVALID_HANDLE_VALUE; attempt++)
		{
			const std::string suffix = MakeRandomSuffix();
			if (suffix.empty())
				return SetError(error, "The system random number generator failed");
			temporaryPath = path;
			temporaryPath += FileSystem::FromUTF8(".tmp-" + suffix);
			file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, security.GetAttributes(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE && ::GetLastError() != ERROR_FILE_EXISTS && ::GetLastError() != ERROR_ALREADY_EXISTS)
				break;
		}
		if (file == INVALID_HANDLE_VALUE)
			return SetError(error, fmt::format("Failed to create '{}': {}", FileSystem::ToUTF8(temporaryPath), GetLastErrorMessage()));

		bool written = true;
		size_t offset = 0;
		while (offset < contents.size())
		{
			const DWORD chunk = static_cast<DWORD>(std::min<size_t>(contents.size() - offset, 1u << 30));
			DWORD bytesWritten = 0;
			if (!WriteFile(file, contents.data() + offset, chunk, &bytesWritten, nullptr) || bytesWritten == 0)
			{
				written = false;
				break;
			}
			offset += bytesWritten;
		}
		const std::string writeError = written ? std::string() : GetLastErrorMessage();
		CloseHandle(file);
		if (!written)
		{
			DeleteFileW(temporaryPath.c_str());
			return SetError(error, fmt::format("Failed to write '{}': {}", FileSystem::ToUTF8(temporaryPath), writeError));
		}

		DWORD moveError = ERROR_SUCCESS;
		for (int attempt = 0; attempt < c_ReplaceAttempts; attempt++)
		{
			if (attempt > 0)
				Sleep(c_ReplaceRetryDelayMilliseconds);
			if (MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
				return true;
			moveError = ::GetLastError();
			if (moveError != ERROR_ACCESS_DENIED && moveError != ERROR_SHARING_VIOLATION)
				break;
		}

		DeleteFileW(temporaryPath.c_str());
		return SetError(error, fmt::format("Failed to replace '{}': {}", FileSystem::ToUTF8(path), WindowsUtils::GetErrorMessage(moveError)));
	}

	bool Platform::EnsurePrivateDirectory(const std::filesystem::path& path, std::string* error)
	{
		// "a/b/" names the same directory as "a/b", which must be created here (not as one of the parents).
		const std::filesystem::path directory = FileSystem::RemoveTrailingSeparators(path);
		const std::string name = FileSystem::ToUTF8(directory);
		// Missing parents get the access rules they inherit; the directory itself is owner-only from the start.
		if (directory.has_parent_path() && !FileSystem::CreateDirectories(directory.parent_path()))
			return SetError(error, fmt::format("Failed to create the directory '{}'", FileSystem::ToUTF8(directory.parent_path())));

		WindowsFileSecurity::OwnerOnlySecurity security;
		std::string securityError;
		if (!security.Initialize(WindowsFileSecurity::OwnerOnlyObject::Directory, securityError))
			return SetError(error, securityError);
		if (!CreateDirectoryW(directory.c_str(), security.GetAttributes()) && ::GetLastError() != ERROR_ALREADY_EXISTS)
			return SetError(error, fmt::format("Failed to create the directory '{}': {}", name, GetLastErrorMessage()));
		return CheckPrivateObject(directory, ObjectKind::Directory, false, error);
	}

	bool Platform::IsTrustedFile(const std::filesystem::path& path, std::string* error)
	{
		return CheckPrivateObject(path, ObjectKind::File, false, error);
	}

	std::optional<std::string> Platform::ReadRegularFile(const std::filesystem::path& path, size_t maxSize, std::string* error)
	{
		return ReadThroughOneHandle(path, maxSize, false, error);
	}

	std::optional<std::string> Platform::ReadTrustedFile(const std::filesystem::path& path, size_t maxSize, std::string* error)
	{
		return ReadThroughOneHandle(path, maxSize, true, error);
	}

	bool Platform::RenameNoReplace(const std::filesystem::path& from, const std::filesystem::path& to)
	{
		// Without MOVEFILE_REPLACE_EXISTING the move fails if the destination exists.
		return MoveFileExW(from.c_str(), to.c_str(), 0) != FALSE;
	}

	void Platform::SetBinaryStandardStreams()
	{
		_setmode(_fileno(stdin), _O_BINARY);
		_setmode(_fileno(stdout), _O_BINARY);
	}

}
