#pragma once

#include "Strata/Core/Base.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	// Operating-system services. Implemented per platform in Platform/<OS>/.
	class Platform
	{
	public:
		static std::string_view GetName(); // "Windows", "Linux" or "macOS"

		static std::filesystem::path GetExecutablePath();
		static std::filesystem::path GetExecutableDirectory();

		// Per-user writable directory for application data (logs, settings). Created if missing.
		//   Windows: %LOCALAPPDATA%/<applicationName>
		//   Linux:   $XDG_DATA_HOME/<applicationName> (default ~/.local/share/<applicationName>)
		//   macOS:   ~/Library/Application Support/<applicationName>
		static std::filesystem::path GetUserDataDirectory(std::string_view applicationName);

		static bool IsDebuggerAttached();
		static void SetCurrentThreadName(std::string_view name);
		static uint32_t GetProcessID();

		static std::optional<std::string> GetEnvVar(std::string_view name);
		static bool SetEnvVar(std::string_view name, std::string_view value);

		// Opens a file, folder or URL with the system's default handler.
		static bool OpenWithDefaultApplication(const std::string& pathOrUrl);

		// Resident memory of the current process in bytes (0 if unavailable).
		static uint64_t GetProcessMemoryUsage();
	};

}
