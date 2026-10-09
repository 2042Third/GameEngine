#include "stpch.h"
#include "Strata/Core/Platform.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/UUID.h"

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

	std::filesystem::path Platform::GetUserRuntimeDirectory(std::string_view applicationName)
	{
		// Local application data is only accessible to the user (and administrators); its subdirectories inherit that.
		PWSTR knownFolder = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &knownFolder)))
			return {};
		const std::filesystem::path base(knownFolder);
		CoTaskMemFree(knownFolder);

		std::filesystem::path directory = base / FileSystem::FromUTF8(applicationName) / "Runtime";
		if (!FileSystem::CreateDirectories(directory))
			return {};
		return directory;
	}

	std::filesystem::path Platform::CreatePrivateDirectory(const std::filesystem::path& parent, std::string_view prefix)
	{
		// The directory inherits the parent's access rules. A name collision (practically impossible) picks another name;
		// an existing directory is never reused.
		constexpr int c_MaxAttempts = 16;
		for (int attempt = 0; attempt < c_MaxAttempts; attempt++)
		{
			const std::filesystem::path path = parent / FileSystem::FromUTF8(fmt::format("{}{}", prefix, UUID().ToString()));
			if (CreateDirectoryW(path.c_str(), nullptr))
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

	bool Platform::IsProcessRunning(uint32_t processID)
	{
		if (processID == 0)
			return false;
		HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(processID));
		if (!process)
			return ::GetLastError() == ERROR_ACCESS_DENIED; // It exists, but belongs to someone else
		// A process object outlives the process while handles to it are open; it is signaled once the process ended.
		const DWORD state = WaitForSingleObject(process, 0);
		CloseHandle(process);
		return state == WAIT_TIMEOUT;
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
