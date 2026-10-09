#include "stpch.h"
#include "Strata/Core/Platform.h"

#include "Strata/Core/BoundedRead.h"
#include "Strata/Core/Crypto.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/UUID.h"

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

		// Security attributes whose DACL grants access to the current user only, protected from inheriting the
		// parent directory's entries.
		class OwnerOnlySecurity
		{
		public:
			bool Initialize(std::string& error)
			{
				m_User = WindowsFileSecurity::GetCurrentUserSid(error);
				if (m_User.empty())
					return false;

				PSID user = m_User.data();
				const DWORD aclSize = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(user));
				m_Acl.resize(aclSize);
				PACL acl = reinterpret_cast<PACL>(m_Acl.data());
				if (!InitializeAcl(acl, aclSize, ACL_REVISION) || !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, user)
					|| !InitializeSecurityDescriptor(&m_Descriptor, SECURITY_DESCRIPTOR_REVISION)
					|| !SetSecurityDescriptorDacl(&m_Descriptor, TRUE, acl, FALSE)
					|| !SetSecurityDescriptorControl(&m_Descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
				{
					error = "Failed to build an owner-only security descriptor: " + GetLastErrorMessage();
					return false;
				}

				m_Attributes.nLength = sizeof(m_Attributes);
				m_Attributes.lpSecurityDescriptor = &m_Descriptor;
				m_Attributes.bInheritHandle = FALSE;
				return true;
			}

			SECURITY_ATTRIBUTES* GetAttributes() { return &m_Attributes; }
		private:
			std::vector<uint8_t> m_User;
			std::vector<uint8_t> m_Acl;
			SECURITY_DESCRIPTOR m_Descriptor = {};
			SECURITY_ATTRIBUTES m_Attributes = {};
		};

		// Opens a file or directory itself (never the target of a link) just to inspect it.
		HANDLE OpenForInspection(const std::filesystem::path& path, bool directory)
		{
			const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
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

	namespace
	{

		// Security attributes for a directory only the current user can access: a protected DACL (nothing is inherited
		// from the parent directory) with a single entry that grants the user full access and is inherited by everything
		// created inside. Not copyable: the attributes point into the object.
		class OwnerOnlyDirectorySecurity
		{
		public:
			OwnerOnlyDirectorySecurity() = default;
			OwnerOnlyDirectorySecurity(const OwnerOnlyDirectorySecurity&) = delete;
			OwnerOnlyDirectorySecurity& operator=(const OwnerOnlyDirectorySecurity&) = delete;

			bool Initialize()
			{
				std::string error;
				m_User = WindowsFileSecurity::GetCurrentUserSid(error);
				if (m_User.empty())
					return false;

				PSID user = m_User.data();
				const DWORD aclSize = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(user));
				m_Acl.resize(aclSize);
				PACL acl = reinterpret_cast<PACL>(m_Acl.data());
				if (!InitializeAcl(acl, aclSize, ACL_REVISION)
					|| !AddAccessAllowedAceEx(acl, ACL_REVISION, CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE, FILE_ALL_ACCESS, user)
					|| !InitializeSecurityDescriptor(&m_Descriptor, SECURITY_DESCRIPTOR_REVISION)
					|| !SetSecurityDescriptorDacl(&m_Descriptor, TRUE, acl, FALSE)
					|| !SetSecurityDescriptorControl(&m_Descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
				{
					return false;
				}

				m_Attributes.nLength = sizeof(m_Attributes);
				m_Attributes.lpSecurityDescriptor = &m_Descriptor;
				m_Attributes.bInheritHandle = FALSE;
				return true;
			}

			SECURITY_ATTRIBUTES* GetAttributes() { return &m_Attributes; }
		private:
			std::vector<uint8_t> m_User;
			std::vector<uint8_t> m_Acl;
			SECURITY_DESCRIPTOR m_Descriptor = {};
			SECURITY_ATTRIBUTES m_Attributes = {};
		};

		// Whether `directory` is a directory that only the current user can modify (besides SYSTEM and the
		// Administrators, who can take over anything anyway; see WindowsFileSecurity::CheckOwnerAndAccess). With
		// `followLink` a link or junction is followed and its target checked; otherwise links are refused.
		bool IsPrivateDirectory(const std::filesystem::path& directory, bool followLink)
		{
			const DWORD flags = FILE_FLAG_BACKUP_SEMANTICS | (followLink ? 0 : FILE_FLAG_OPEN_REPARSE_POINT);
			HANDLE handle = CreateFileW(directory.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr, OPEN_EXISTING, flags, nullptr);
			if (handle == INVALID_HANDLE_VALUE)
				return false;

			BY_HANDLE_FILE_INFORMATION information = {};
			const bool isDirectory = GetFileInformationByHandle(handle, &information) && (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
			const std::optional<bool> isLink = WindowsFileSecurity::IsNameSurrogate(handle);
			const bool isPrivate = isDirectory && isLink.has_value() && !*isLink
				&& WindowsFileSecurity::CheckHandleOwnerAndAccess(handle, FileSystem::ToUTF8(directory), nullptr);
			CloseHandle(handle);
			return isPrivate;
		}

		// `directory` inside `base` (created if missing) for files that only the current user may modify. A new
		// `directory` gets an owner-only DACL; an existing one is used as it is. Both must pass IsPrivateDirectory (`base`
		// may be a link, say to a relocated folder); empty otherwise.
		std::filesystem::path PreparePrivateDirectory(const std::filesystem::path& base, const std::filesystem::path& directory)
		{
			if (!base.is_absolute() || !FileSystem::CreateDirectories(base) || !IsPrivateDirectory(base, true))
				return {};

			OwnerOnlyDirectorySecurity security;
			if (!security.Initialize())
				return {};
			if (!CreateDirectoryW(directory.c_str(), security.GetAttributes()) && ::GetLastError() != ERROR_ALREADY_EXISTS)
				return {};
			if (!IsPrivateDirectory(directory, false))
				return {};
			return directory;
		}

	}

	std::filesystem::path Platform::GetUserRuntimeDirectory(std::string_view applicationName)
	{
		// An explicit location (tests, sandboxes) replaces the default; it must pass the same checks.
		if (const std::optional<std::string> configured = GetEnvVar("STRATA_RUNTIME_DIR"); configured && !configured->empty())
		{
			const std::filesystem::path base = FileSystem::FromUTF8(*configured);
			return PreparePrivateDirectory(base, base / FileSystem::FromUTF8(applicationName));
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
		return PreparePrivateDirectory(base, base / "Runtime");
	}

	std::filesystem::path Platform::CreatePrivateDirectory(const std::filesystem::path& parent, std::string_view prefix)
	{
		// A name collision (practically impossible) picks another name; an existing directory is never reused.
		OwnerOnlyDirectorySecurity security;
		if (!security.Initialize())
			return {};
		constexpr int c_MaxAttempts = 16;
		for (int attempt = 0; attempt < c_MaxAttempts; attempt++)
		{
			const std::filesystem::path path = parent / FileSystem::FromUTF8(fmt::format("{}{}", prefix, UUID().ToString()));
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

		OwnerOnlySecurity security;
		std::string securityError;
		if (!security.Initialize(securityError))
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

	bool Platform::EnsurePrivateDirectory(const std::filesystem::path& directory, std::string* error)
	{
		const std::string name = FileSystem::ToUTF8(directory);
		if (!FileSystem::CreateDirectories(directory))
			return SetError(error, fmt::format("Failed to create the directory '{}'", name));

		HANDLE handle = OpenForInspection(directory, true);
		if (handle == INVALID_HANDLE_VALUE)
			return SetError(error, fmt::format("Cannot open '{}': {}", name, GetLastErrorMessage()));
		HandleGuard handleGuard(handle);

		BY_HANDLE_FILE_INFORMATION information = {};
		if (!GetFileInformationByHandle(handle, &information))
			return SetError(error, fmt::format("Cannot inspect '{}': {}", name, GetLastErrorMessage()));
		if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			return SetError(error, fmt::format("'{}' is not a directory", name));
		if (!CheckNotLink(handle, name, error))
			return false;
		return WindowsFileSecurity::CheckHandleOwnerAndAccess(handle, name, error);
	}

	bool Platform::IsTrustedFile(const std::filesystem::path& path, std::string* error)
	{
		const std::string name = FileSystem::ToUTF8(path);
		HANDLE handle = OpenForInspection(path, false);
		if (handle == INVALID_HANDLE_VALUE)
			return SetError(error, fmt::format("Cannot open '{}': {}", name, GetLastErrorMessage()));
		HandleGuard handleGuard(handle);

		BY_HANDLE_FILE_INFORMATION information = {};
		if (!GetFileInformationByHandle(handle, &information))
			return SetError(error, fmt::format("Cannot inspect '{}': {}", name, GetLastErrorMessage()));
		if (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			return SetError(error, fmt::format("'{}' is not a regular file", name));
		if (!CheckNotLink(handle, name, error))
			return false;
		return WindowsFileSecurity::CheckHandleOwnerAndAccess(handle, name, error);
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
