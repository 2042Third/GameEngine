#include "stpch.h"
#include "Strata/Core/Platform.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/UUID.h"

#include "Platform/Windows/WindowsFileSecurity.h"
#include "Platform/Windows/WindowsUtils.h"

#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>

namespace Strata
{

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
		std::filesystem::path base;
		PWSTR knownFolder = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &knownFolder)))
		{
			base = std::filesystem::path(knownFolder);
			CoTaskMemFree(knownFolder);
		}
		else
		{
			base = std::filesystem::temp_directory_path();
		}

		std::filesystem::path directory = base / FileSystem::FromUTF8(applicationName);
		FileSystem::CreateDirectories(directory);
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

}
