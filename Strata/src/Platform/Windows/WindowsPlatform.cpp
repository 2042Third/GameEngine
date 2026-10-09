#include "stpch.h"
#include "Strata/Core/Platform.h"

#include "Strata/Core/Crypto.h"
#include "Strata/Core/FileSystem.h"

#include "Platform/Windows/WindowsUtils.h"

#include <aclapi.h>
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

		// Rights that let a principal change a file or directory: its data or entries, its attributes, its name,
		// or its security.
		constexpr ACCESS_MASK c_ModifyRights = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES
			| FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;

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

		std::vector<uint8_t> MakeWellKnownSid(WELL_KNOWN_SID_TYPE type)
		{
			DWORD size = SECURITY_MAX_SID_SIZE;
			std::vector<uint8_t> sid(size);
			if (!CreateWellKnownSid(type, nullptr, sid.data(), &size))
				return {};
			sid.resize(size);
			return sid;
		}

		bool IsSid(PSID sid, const std::vector<uint8_t>& wellKnownSid)
		{
			return !wellKnownSid.empty() && EqualSid(sid, const_cast<uint8_t*>(wellKnownSid.data())) != FALSE;
		}

		// The account this process runs as, and whether its token is elevated.
		class CurrentUser
		{
		public:
			bool Query(std::string& error)
			{
				HANDLE token = nullptr;
				if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
				{
					error = "OpenProcessToken failed: " + GetLastErrorMessage();
					return false;
				}
				HandleGuard tokenGuard(token);

				DWORD size = 0;
				GetTokenInformation(token, TokenUser, nullptr, 0, &size);
				m_TokenUser.resize(size);
				if (size == 0 || !GetTokenInformation(token, TokenUser, m_TokenUser.data(), size, &size))
				{
					error = "GetTokenInformation failed: " + GetLastErrorMessage();
					return false;
				}

				TOKEN_ELEVATION elevation = {};
				DWORD elevationSize = 0;
				m_Elevated = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &elevationSize) && elevation.TokenIsElevated != 0;
				return true;
			}

			PSID GetSid() const { return reinterpret_cast<const TOKEN_USER*>(m_TokenUser.data())->User.Sid; }
			bool IsElevated() const { return m_Elevated; }
		private:
			std::vector<uint8_t> m_TokenUser;
			bool m_Elevated = false;
		};

		// Security attributes whose DACL grants access to the current user only, protected from inheriting the
		// parent directory's entries.
		class OwnerOnlySecurity
		{
		public:
			bool Initialize(std::string& error)
			{
				if (!m_User.Query(error))
					return false;

				PSID user = m_User.GetSid();
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
			CurrentUser m_User;
			std::vector<uint8_t> m_Acl;
			SECURITY_DESCRIPTOR m_Descriptor = {};
			SECURITY_ATTRIBUTES m_Attributes = {};
		};

		// Verifies, on an open handle, that only the current user (plus the system and administrators, who can take
		// any file anyway) can modify the object: it must be owned by the current user (or by the Administrators
		// group, which owns what an elevated administrator creates), and no access control entry may grant modify
		// rights to anyone else.
		bool CheckOwnerAndAccess(HANDLE handle, const std::string& name, std::string* error)
		{
			PSID owner = nullptr;
			PACL dacl = nullptr;
			PSECURITY_DESCRIPTOR descriptor = nullptr;
			const DWORD result = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &descriptor);
			if (result != ERROR_SUCCESS)
				return SetError(error, fmt::format("Cannot read the permissions of '{}': {}", name, WindowsUtils::GetErrorMessage(result)));

			// owner and dacl point into the descriptor.
			struct DescriptorGuard
			{
				PSECURITY_DESCRIPTOR Descriptor;
				~DescriptorGuard() { LocalFree(Descriptor); }
			} descriptorGuard { descriptor };

			CurrentUser user;
			std::string userError;
			if (!user.Query(userError))
				return SetError(error, userError);

			const std::vector<uint8_t> administrators = MakeWellKnownSid(WinBuiltinAdministratorsSid);
			const std::vector<uint8_t> system = MakeWellKnownSid(WinLocalSystemSid);
			const std::vector<uint8_t> ownerRights = MakeWellKnownSid(WinCreatorOwnerRightsSid);

			if (!owner)
				return SetError(error, fmt::format("'{}' has no owner (its file system does not support permissions)", name));
			const bool trustedOwner = EqualSid(owner, user.GetSid()) || (user.IsElevated() && IsSid(owner, administrators));
			if (!trustedOwner)
				return SetError(error, fmt::format("'{}' is owned by another account", name));
			if (!dacl)
				return SetError(error, fmt::format("'{}' has no access control list, so any account may modify it", name));

			for (DWORD index = 0; index < dacl->AceCount; index++)
			{
				void* entry = nullptr;
				if (!GetAce(dacl, index, &entry))
					return SetError(error, fmt::format("Cannot read the permissions of '{}': {}", name, GetLastErrorMessage()));

				const ACE_HEADER* header = static_cast<const ACE_HEADER*>(entry);
				if ((header->AceFlags & INHERIT_ONLY_ACE) != 0)
					continue; // Applies only to objects created inside it later
				if (header->AceType == ACCESS_DENIED_ACE_TYPE)
					continue; // Deny entries only take rights away
				if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
					return SetError(error, fmt::format("'{}' has an access control entry of an unexpected kind", name));

				const ACCESS_ALLOWED_ACE* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
				if ((allowed->Mask & c_ModifyRights) == 0)
					continue;

				PSID sid = reinterpret_cast<PSID>(const_cast<DWORD*>(&allowed->SidStart));
				const bool trusted = EqualSid(sid, user.GetSid()) || EqualSid(sid, owner) || IsSid(sid, system) || IsSid(sid, administrators) || IsSid(sid, ownerRights);
				if (!trusted)
					return SetError(error, fmt::format("'{}' can be modified by another account", name));
			}
			return true;
		}

		// Opens a file or directory itself (never the target of a link) just to inspect it.
		HANDLE OpenForInspection(const std::filesystem::path& path, bool directory)
		{
			const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0);
			return CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, flags, nullptr);
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
		if (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
			return SetError(error, fmt::format("'{}' is a link or junction, which is not trusted", name));
		return CheckOwnerAndAccess(handle, name, error);
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
		if (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
			return SetError(error, fmt::format("'{}' is not a regular file (links are not trusted)", name));
		return CheckOwnerAndAccess(handle, name, error);
	}

	std::optional<std::string> Platform::ReadRegularFile(const std::filesystem::path& path, size_t maxSize, std::string* error)
	{
		const std::string name = FileSystem::ToUTF8(path);
		// One handle for every check and the read, so the file cannot be swapped in between. Delete sharing lets
		// writers replace the file atomically while it is being read.
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			SetError(error, fmt::format("Cannot open '{}': {}", name, GetLastErrorMessage()));
			return std::nullopt;
		}
		HandleGuard fileGuard(file);

		BY_HANDLE_FILE_INFORMATION information = {};
		if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &information)
			|| (information.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
		{
			SetError(error, fmt::format("'{}' is not a regular file", name));
			return std::nullopt;
		}

		const uint64_t size = (static_cast<uint64_t>(information.nFileSizeHigh) << 32) | information.nFileSizeLow;
		if (size > maxSize)
		{
			SetError(error, fmt::format("'{}' is larger than {} bytes", name, maxSize));
			return std::nullopt;
		}

		// Read one byte more than allowed, to notice a file that grew since it was inspected.
		std::string contents(static_cast<size_t>(size) + 1, '\0');
		size_t total = 0;
		while (total < contents.size())
		{
			const DWORD chunk = static_cast<DWORD>(std::min<size_t>(contents.size() - total, 1u << 30));
			DWORD bytesRead = 0;
			if (!ReadFile(file, contents.data() + total, chunk, &bytesRead, nullptr))
			{
				SetError(error, fmt::format("Failed to read '{}': {}", name, GetLastErrorMessage()));
				return std::nullopt;
			}
			if (bytesRead == 0)
				break;
			total += bytesRead;
		}
		if (total > maxSize)
		{
			SetError(error, fmt::format("'{}' is larger than {} bytes", name, maxSize));
			return std::nullopt;
		}
		contents.resize(total);
		return contents;
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
