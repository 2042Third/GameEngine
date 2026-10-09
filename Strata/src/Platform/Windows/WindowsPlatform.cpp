#include "stpch.h"
#include "Strata/Core/Platform.h"

#include "Strata/Core/FileSystem.h"

#include "Platform/Windows/WindowsUtils.h"

#include <bcrypt.h>
#include <fcntl.h>
#include <io.h>
#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
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

		std::string GetLastErrorMessage()
		{
			return WindowsUtils::GetErrorMessage(::GetLastError());
		}

		// Security attributes whose DACL grants access to the current user only, protected from inheriting the
		// parent directory's entries.
		class OwnerOnlySecurity
		{
		public:
			bool Initialize(std::string& error)
			{
				HANDLE token = nullptr;
				if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
				{
					error = "OpenProcessToken failed: " + GetLastErrorMessage();
					return false;
				}

				DWORD size = 0;
				GetTokenInformation(token, TokenUser, nullptr, 0, &size);
				m_TokenUser.resize(size);
				const BOOL queried = size > 0 && GetTokenInformation(token, TokenUser, m_TokenUser.data(), size, &size);
				const std::string queryError = queried ? std::string() : GetLastErrorMessage();
				CloseHandle(token);
				if (!queried)
				{
					error = "GetTokenInformation failed: " + queryError;
					return false;
				}

				PSID user = reinterpret_cast<TOKEN_USER*>(m_TokenUser.data())->User.Sid;
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
			std::vector<uint8_t> m_TokenUser;
			std::vector<uint8_t> m_Acl;
			SECURITY_DESCRIPTOR m_Descriptor = {};
			SECURITY_ATTRIBUTES m_Attributes = {};
		};

		bool SetError(std::string* error, std::string message)
		{
			if (error)
				*error = std::move(message);
			return false;
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

		static std::atomic<uint32_t> s_TemporaryCounter = 0;
		std::filesystem::path temporaryPath = path;
		temporaryPath += FileSystem::FromUTF8(fmt::format(".tmp-{}-{}", GetCurrentProcessId(), s_TemporaryCounter.fetch_add(1)));

		// CREATE_NEW never opens an existing file or link, so the data only ever lands in a file created here.
		HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, security.GetAttributes(), CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
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
		if (!FileSystem::CreateDirectories(directory))
			return SetError(error, fmt::format("Failed to create the directory '{}'", FileSystem::ToUTF8(directory)));

		const DWORD attributes = GetFileAttributesW(directory.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
			return SetError(error, fmt::format("'{}' is not a directory", FileSystem::ToUTF8(directory)));
		if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
			return SetError(error, fmt::format("'{}' is a link or junction, which is not trusted", FileSystem::ToUTF8(directory)));
		return true;
	}

	bool Platform::IsTrustedFile(const std::filesystem::path& path, std::string* error)
	{
		const DWORD attributes = GetFileAttributesW(path.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES)
			return SetError(error, fmt::format("'{}' does not exist", FileSystem::ToUTF8(path)));
		if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
			return SetError(error, fmt::format("'{}' is not a regular file", FileSystem::ToUTF8(path)));
		return true;
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
