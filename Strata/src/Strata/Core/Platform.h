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

		// Per-user directory for files that only matter while the application runs, such as private copies of libraries
		// it loads. Nobody but the current user can add, replace or rename files in it. Created if missing; empty if no
		// such location exists.
		//   Windows: %LOCALAPPDATA%/<applicationName>/Runtime
		//   Linux:   $XDG_RUNTIME_DIR/<applicationName> (default $XDG_CACHE_HOME/<applicationName> or ~/.cache/<applicationName>)
		//   macOS:   <per-user temporary directory>/<applicationName> (default ~/Library/Caches/<applicationName>)
		// POSIX: the directory and the one containing it must be owned by the user and writable by nobody else; a location
		// that fails the check is skipped.
		static std::filesystem::path GetUserRuntimeDirectory(std::string_view applicationName);
		// Creates a new directory "<prefix><random characters>" in `parent` that only the current user can access (POSIX:
		// mode 0700; Windows: it inherits the access rules of a per-user parent such as GetUserRuntimeDirectory). Never
		// reuses an existing directory. Returns an empty path on failure.
		static std::filesystem::path CreatePrivateDirectory(const std::filesystem::path& parent, std::string_view prefix);

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
