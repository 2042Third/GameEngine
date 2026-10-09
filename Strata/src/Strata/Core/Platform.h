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

		// Per-user directory for files that only matter while the application runs, such as private copies of libraries
		// it loads. Nobody but the current user can add, replace or rename files in it. Created if missing; empty if no
		// such location exists.
		//   Windows: %LOCALAPPDATA%/<applicationName>/Runtime
		//   Linux:   $XDG_RUNTIME_DIR/<applicationName>, else $XDG_CACHE_HOME/<applicationName> or ~/.cache/<applicationName>
		//   macOS:   <per-user temporary directory>/<applicationName>, else ~/Library/Caches/<applicationName>
		//   POSIX, when none of those qualifies: <temporary directory>/<applicationName>-<user id>
		// The environment variable STRATA_RUNTIME_DIR replaces the search with <STRATA_RUNTIME_DIR>/<applicationName>
		// (tests use it to stay out of the user's real directory).
		// POSIX: the directory and the one containing it must be owned by the user and writable by nobody else (in the
		// temporary directory: accessible by nobody else, inside a sticky or private temporary directory), and on a file
		// system that allows executing files; a location that fails the checks is skipped. Group and other write
		// permission on an existing directory of the user is removed.
		// Windows: a new directory gets a protected DACL that grants only the current user access (inherited by what is
		// created inside). The directory - not a link or junction - and the one containing it must be owned by the user,
		// SYSTEM or the Administrators, with no access control entry that lets anyone else modify them; otherwise the
		// location is not used.
		static std::filesystem::path GetUserRuntimeDirectory(std::string_view applicationName);
		// Creates a new directory "<prefix><random characters>" in `parent` that only the current user can access (POSIX:
		// mode 0700; Windows: a protected DACL that grants only the current user access, inherited by what is created
		// inside). Never reuses an existing directory. Returns an empty path on failure.
		static std::filesystem::path CreatePrivateDirectory(const std::filesystem::path& parent, std::string_view prefix);

		static bool IsDebuggerAttached();
		static void SetCurrentThreadName(std::string_view name);
		static uint32_t GetProcessID();
		// Whether a process with this id is running (false once it has exited, even while a handle keeps its id).
		static bool IsProcessAlive(uint32_t processId);
		// An opaque value identifying one run of a process: the same for the same process, different for a later
		// process that reuses the id. nullopt if no such process exists or it cannot be inspected.
		static std::optional<uint64_t> GetProcessStartTime(uint32_t processId);

		// Fills buffer from the operating system's cryptographically secure random number generator.
		static bool GenerateSecureRandom(std::span<uint8_t> buffer);

		// Atomically replaces path with contents, readable and writable only by the current user (POSIX mode 0600,
		// a protected owner-only DACL on Windows). The data is written to a freshly created temporary file with a
		// random name (exclusive create, never following an existing file or link) that is then renamed over path;
		// the rename is retried briefly while readers hold the destination open (Windows). Parent directories are
		// created.
		static bool WritePrivateFile(const std::filesystem::path& path, std::string_view contents, std::string* error = nullptr);
		// Creates directory (and its parents) if missing and verifies it may hold secrets: a real directory (not a
		// symbolic link or junction) that only the current user can modify. POSIX: owned by the current user; a
		// newly created leaf gets mode 0700, and group/other write permission on an owned directory is removed.
		// Windows: owned by the current user, Administrators or SYSTEM (an elevated run creates objects owned by
		// Administrators), with no access control entry granting modify rights to anyone else. Reparse points that
		// hold an object's own data (e.g. cloud placeholders) are accepted; links and junctions are not.
		static bool EnsurePrivateDirectory(const std::filesystem::path& directory, std::string* error = nullptr);
		// Whether a file can be trusted as written by the current user: a regular file (not a link) that only the
		// current user can modify, by the same rules as EnsurePrivateDirectory. To read such a file, use
		// ReadTrustedFile, which checks and reads through one handle.
		static bool IsTrustedFile(const std::filesystem::path& path, std::string* error = nullptr);
		// Reads a regular file to its end through a single open handle, so it cannot be swapped between the checks
		// and the read; a file larger than maxSize bytes (also one that grows while it is read) is refused. Links
		// (final component), directories, FIFOs and devices are rejected without blocking. nullopt (with the reason
		// in error) otherwise.
		static std::optional<std::string> ReadRegularFile(const std::filesystem::path& path, size_t maxSize, std::string* error = nullptr);
		// ReadRegularFile that also requires the file to be trusted (see IsTrustedFile), checked on the same handle
		// the file is read through.
		static std::optional<std::string> ReadTrustedFile(const std::filesystem::path& path, size_t maxSize, std::string* error = nullptr);
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
