#pragma once

#include "Strata/Core/Base.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// File system helpers. All functions report failure through their return value and never throw.
	// Strings are UTF-8 throughout the engine; convert with FromUTF8/ToUTF8 at std::filesystem boundaries.
	class FileSystem
	{
	public:
		static std::optional<std::vector<uint8_t>> ReadBytes(const std::filesystem::path& path);
		static std::optional<std::string> ReadText(const std::filesystem::path& path);

		// Writes are atomic: data goes to a temporary file that then replaces the destination,
		// so readers never observe a partially written file. Parent directories are created.
		static bool WriteBytes(const std::filesystem::path& path, std::span<const uint8_t> data);
		static bool WriteText(const std::filesystem::path& path, std::string_view text);

		static bool Exists(const std::filesystem::path& path);
		static bool IsDirectory(const std::filesystem::path& path);
		static bool IsRegularFile(const std::filesystem::path& path);
		static bool CreateDirectories(const std::filesystem::path& path);
		static bool Remove(const std::filesystem::path& path); // Files, or directories recursively
		static bool Rename(const std::filesystem::path& from, const std::filesystem::path& to);
		static bool Copy(const std::filesystem::path& from, const std::filesystem::path& to, bool overwrite = true);
		// Copies the directories and regular files below `from` into `to` (created if needed), overwriting files. Fails, with
		// a description in outError, when an entry cannot be copied (e.g. a directory is in the way of a file); entries
		// copied before the failure stay. The copy never leaves the two trees: links (symbolic links, junctions) and
		// special files below `from` are neither followed nor copied but skipped with a logged warning, existing links in
		// `to` are never written through, and copying into `from` itself (or a directory inside it) is refused.
		static bool CopyDirectory(const std::filesystem::path& from, const std::filesystem::path& to, std::string* outError = nullptr);

		static std::optional<uint64_t> GetFileSize(const std::filesystem::path& path);
		// Last modification time as an opaque, monotonic-per-file tick count.
		static std::optional<int64_t> GetLastWriteTime(const std::filesystem::path& path);

		static std::filesystem::path FromUTF8(std::string_view utf8);
		static std::string ToUTF8(const std::filesystem::path& path); // Generic form with '/' separators

		// `path` without trailing directory separators ("a/b/" -> "a/b"), which name the same directory but make some
		// system calls follow a final symbolic link. A root ("/", "C:/") stays as it is.
		static std::filesystem::path RemoveTrailingSeparators(std::filesystem::path path);

		// Lexically normalized path relative to base, or empty if path is not inside base.
		static std::filesystem::path GetRelativePath(const std::filesystem::path& path, const std::filesystem::path& base);
		static bool IsInside(const std::filesystem::path& path, const std::filesystem::path& base);
		// Like IsInside, but resolves symbolic links and relative components against the file system first, so a link
		// inside base pointing elsewhere is outside. False when either path cannot be resolved.
		static bool IsInsideResolved(const std::filesystem::path& path, const std::filesystem::path& base);

		// Returns a path that does not exist yet by appending " (n)" before the extension if needed.
		static std::filesystem::path GetUniquePath(const std::filesystem::path& desiredPath);
	};

}
