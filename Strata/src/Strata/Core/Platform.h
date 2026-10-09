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

		// Private directories hold files that nobody but the current user may add, replace or rename: editor session
		// files (tokens), the copies of script modules the engine loads. EnsurePrivateDirectory, CreatePrivateDirectory and
		// GetUserRuntimeDirectory share one contract:
		//  - A directory they create is owner-only from the start. POSIX: mode 0700. Windows: a protected DACL (nothing
		//    inherited from the parent) whose single entry grants the current user full access and is inherited by what
		//    is created inside.
		//  - An existing directory qualifies only if it is a real directory (not a symbolic link or junction) that only
		//    the current user can modify.
		//      POSIX:   owned by the current user. If group or others have any permission on it, it is tightened to
		//               mode 0700 (they could plant or replace files in it, or read what it holds).
		//      Windows: owned by the current user, the Administrators or SYSTEM (an elevated run creates objects owned by
		//               the Administrators), with no access control entry that grants anyone else modify rights; its
		//               permissions are never changed. Reparse points that hold the object's own data (such as cloud
		//               placeholders) are accepted.
		//  - The directory containing it is not checked, and missing parents are created with default permissions:
		//    callers that do not control the parent check it themselves (GetUserRuntimeDirectory does).

		// Creates `directory` if it is missing and makes sure it is private (see above). False, with the reason in error,
		// if it is not and cannot be made so.
		static bool EnsurePrivateDirectory(const std::filesystem::path& directory, std::string* error = nullptr);
		// Creates a new private directory (see above) "<prefix><random characters>" in `parent`, which should be private
		// itself. Never reuses an existing directory. Returns an empty path on failure.
		static std::filesystem::path CreatePrivateDirectory(const std::filesystem::path& parent, std::string_view prefix);
		// Per-user private directory (see above) for files that only matter while the application runs, such as private
		// copies of libraries it loads: <location>/<name> in the first location that qualifies. Created if missing;
		// empty if no location qualifies.
		//   Windows: %LOCALAPPDATA%/<applicationName>, name "Runtime"
		//   Linux:   $XDG_RUNTIME_DIR, else $XDG_CACHE_HOME or ~/.cache; name <applicationName>
		//   macOS:   the per-user temporary directory, else ~/Library/Caches; name <applicationName>
		//   POSIX, when none of those qualifies: the temporary directory, name <applicationName>-<user id>
		// The environment variable STRATA_RUNTIME_DIR replaces the search with the location <STRATA_RUNTIME_DIR>, name
		// <applicationName> (tests use it to stay out of the user's real directory).
		// A location qualifies if it is an absolute path to a directory that only the current user can modify, by the
		// rules above, except that it may be a symbolic link or junction (a relocated cache folder, say) and is never
		// changed; a missing one is created (POSIX: mode 0700). On POSIX it must also be on a file system that allows
		// executing files, and the shared temporary directory of the last fallback qualifies if it has the sticky bit
		// (users cannot rename or remove each other's entries).
		static std::filesystem::path GetUserRuntimeDirectory(std::string_view applicationName);

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
		// Whether a file can be trusted as written by the current user: a regular file (not a link) that only the
		// current user can modify, by the rules private directories follow (see EnsurePrivateDirectory; POSIX: owned by
		// the current user and writable by nobody else, never changed). To read such a file, use
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
