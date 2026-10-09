#pragma once

#include "Strata/Core/Base.h"

#include <filesystem>
#include <optional>
#include <span>
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
		// Falls back to the system temp directory when the platform reports no per-user location.
		static std::filesystem::path GetUserDataDirectory(std::string_view applicationName);
		// Like GetUserDataDirectory, but without the shared temp-directory fallback: nullopt when no per-user
		// location exists (or it cannot be created). Use it for anything secret.
		static std::optional<std::filesystem::path> FindUserDataDirectory(std::string_view applicationName);

		static bool IsDebuggerAttached();
		static void SetCurrentThreadName(std::string_view name);
		static uint32_t GetProcessID();
		// Whether a process with this id is running (false once it has exited, even while a handle keeps its id).
		static bool IsProcessAlive(uint32_t processId);

		// Fills buffer from the operating system's cryptographically secure random number generator.
		static bool GenerateSecureRandom(std::span<uint8_t> buffer);

		// Atomically replaces path with contents, readable and writable only by the current user (POSIX mode 0600,
		// a protected owner-only DACL on Windows). The data is written to a freshly created temporary file (exclusive
		// create, never following an existing file or link) that is then renamed over path; the rename is retried
		// briefly while readers hold the destination open (Windows). Parent directories are created.
		static bool WritePrivateFile(const std::filesystem::path& path, std::string_view contents, std::string* error = nullptr);
		// Creates directory (and its parents) if missing and verifies it may hold secrets. POSIX: the directory must
		// be a real directory (not a symbolic link) owned by the current user; a newly created leaf gets mode 0700,
		// and group/other write permission on an owned directory is removed. Windows: it must not be a reparse
		// point (per-user profile ACLs protect the default locations).
		static bool EnsurePrivateDirectory(const std::filesystem::path& directory, std::string* error = nullptr);
		// Whether a file can be trusted as written by the current user. POSIX: a regular file (not a symbolic link)
		// owned by the current user and not writable by group or others. Windows: a regular file that is not a
		// reparse point.
		static bool IsTrustedFile(const std::filesystem::path& path, std::string* error = nullptr);
		// Renames from to to, failing (without touching either file) if to already exists.
		static bool RenameNoReplace(const std::filesystem::path& from, const std::filesystem::path& to);

		// Switches stdin and stdout to binary mode (no newline translation) for machine-readable protocols such as
		// MCP over stdio. No-op on POSIX, where streams are always binary.
		static void SetBinaryStandardStreams();

		static std::optional<std::string> GetEnvVar(std::string_view name);
		static bool SetEnvVar(std::string_view name, std::string_view value);

		// Opens a file, folder or URL with the system's default handler.
		static bool OpenWithDefaultApplication(const std::string& pathOrUrl);

		// Resident memory of the current process in bytes (0 if unavailable).
		static uint64_t GetProcessMemoryUsage();
	};

}
