#include "stpch.h"
#include "Strata/Core/FileSystem.h"

#include <atomic>
#include <cstdio>
#include <fstream>

namespace Strata
{

	std::optional<std::vector<uint8_t>> FileSystem::ReadBytes(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream)
			return std::nullopt;

		const std::streamoff size = stream.tellg();
		if (size < 0)
			return std::nullopt;

		std::vector<uint8_t> data(static_cast<size_t>(size));
		stream.seekg(0, std::ios::beg);
		if (size > 0 && !stream.read(reinterpret_cast<char*>(data.data()), size))
			return std::nullopt;

		return data;
	}

	std::optional<std::string> FileSystem::ReadText(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream)
			return std::nullopt;

		const std::streamoff size = stream.tellg();
		if (size < 0)
			return std::nullopt;

		std::string text(static_cast<size_t>(size), '\0');
		stream.seekg(0, std::ios::beg);
		if (size > 0 && !stream.read(text.data(), size))
			return std::nullopt;

		// Strip a UTF-8 byte order mark if present.
		if (text.size() >= 3 && static_cast<uint8_t>(text[0]) == 0xEF && static_cast<uint8_t>(text[1]) == 0xBB && static_cast<uint8_t>(text[2]) == 0xBF)
			text.erase(0, 3);

		return text;
	}

	static bool WriteFileAtomically(const std::filesystem::path& path, const void* data, size_t size)
	{
		std::error_code error;
		if (path.has_parent_path())
			std::filesystem::create_directories(path.parent_path(), error);

		static std::atomic<uint32_t> s_TempCounter = 0;
		std::filesystem::path tempPath = path;
		tempPath += ".tmp" + std::to_string(s_TempCounter.fetch_add(1));

		{
			std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
			if (!stream)
				return false;
			if (size > 0)
				stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
			stream.flush();
			if (!stream)
			{
				stream.close();
				std::filesystem::remove(tempPath, error);
				return false;
			}
		}

		std::filesystem::rename(tempPath, path, error);
		if (error)
		{
			std::error_code removeError;
			std::filesystem::remove(tempPath, removeError);
			return false;
		}
		return true;
	}

	bool FileSystem::WriteBytes(const std::filesystem::path& path, std::span<const uint8_t> data)
	{
		return WriteFileAtomically(path, data.data(), data.size());
	}

	bool FileSystem::WriteText(const std::filesystem::path& path, std::string_view text)
	{
		return WriteFileAtomically(path, text.data(), text.size());
	}

	bool FileSystem::Exists(const std::filesystem::path& path)
	{
		std::error_code error;
		return std::filesystem::exists(path, error);
	}

	bool FileSystem::IsDirectory(const std::filesystem::path& path)
	{
		std::error_code error;
		return std::filesystem::is_directory(path, error);
	}

	bool FileSystem::IsRegularFile(const std::filesystem::path& path)
	{
		std::error_code error;
		return std::filesystem::is_regular_file(path, error);
	}

	bool FileSystem::CreateDirectories(const std::filesystem::path& path)
	{
		std::error_code error;
		std::filesystem::create_directories(path, error);
		return !error && std::filesystem::is_directory(path, error);
	}

	bool FileSystem::Remove(const std::filesystem::path& path)
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
		return !error;
	}

	bool FileSystem::Rename(const std::filesystem::path& from, const std::filesystem::path& to)
	{
		std::error_code error;
		if (to.has_parent_path())
			std::filesystem::create_directories(to.parent_path(), error);
		std::filesystem::rename(from, to, error);
		return !error;
	}

	bool FileSystem::Copy(const std::filesystem::path& from, const std::filesystem::path& to, bool overwrite)
	{
		std::error_code error;
		if (to.has_parent_path())
			std::filesystem::create_directories(to.parent_path(), error);

		const auto options = overwrite ? std::filesystem::copy_options::overwrite_existing : std::filesystem::copy_options::skip_existing;
		std::filesystem::copy_file(from, to, options, error);
		return !error;
	}

	namespace
	{

		// The type of the object at path itself (a link is not followed); not_found when nothing is there.
		std::optional<std::filesystem::file_type> GetOwnFileType(const std::filesystem::path& path, std::error_code& error)
		{
			const std::filesystem::file_type type = std::filesystem::symlink_status(path, error).type();
			if (type == std::filesystem::file_type::not_found)
			{
				error.clear();
				return type;
			}
			if (error)
				return std::nullopt;
			return type;
		}

		bool SetCopyError(std::string* outError, std::string error)
		{
			if (outError)
				*outError = std::move(error);
			return false;
		}

	}

	bool FileSystem::CopyDirectory(const std::filesystem::path& from, const std::filesystem::path& to, std::string* outError)
	{
		std::error_code error;
		if (!std::filesystem::is_directory(from, error))
			return SetCopyError(outError, fmt::format("'{}' is not a directory", ToUTF8(from)));

		// A destination inside the source would be copied into itself again and again.
		const std::filesystem::path resolvedFrom = std::filesystem::weakly_canonical(from, error);
		if (error)
			return SetCopyError(outError, fmt::format("Cannot resolve '{}': {}", ToUTF8(from), error.message()));
		const std::filesystem::path resolvedTo = std::filesystem::weakly_canonical(to, error);
		if (error)
			return SetCopyError(outError, fmt::format("Cannot resolve '{}': {}", ToUTF8(to), error.message()));
		if (IsInside(resolvedTo, resolvedFrom))
			return SetCopyError(outError, fmt::format("Cannot copy '{}' into itself ('{}')", ToUTF8(from), ToUTF8(to)));

		std::filesystem::create_directories(to, error);
		if (error)
			return SetCopyError(outError, fmt::format("Cannot create '{}': {}", ToUTF8(to), error.message()));
		if (!std::filesystem::is_directory(to, error))
			return SetCopyError(outError, fmt::format("Cannot copy into '{}': it is not a directory", ToUTF8(to)));

		// Entry by entry rather than with std::filesystem::copy: MSVC's implementation compares file identities first,
		// which fails (ERROR_INVALID_PARAMETER) on file systems without 128-bit file ids, such as exFAT. Every entry has
		// its own error code: one shared with the iteration would be cleared by the next successful increment.
		std::filesystem::recursive_directory_iterator it(from, error);
		if (error)
			return SetCopyError(outError, fmt::format("Cannot list '{}': {}", ToUTF8(from), error.message()));
		const std::filesystem::recursive_directory_iterator end;
		while (it != end)
		{
			const std::filesystem::path source = it->path();
			const std::filesystem::path target = to / source.lexically_relative(from);
			std::error_code entryError;
			const std::optional<std::filesystem::file_type> sourceType = GetOwnFileType(source, entryError);
			if (!sourceType)
				return SetCopyError(outError, fmt::format("Cannot read '{}': {}", ToUTF8(source), entryError.message()));

			if (*sourceType == std::filesystem::file_type::directory)
			{
				const std::optional<std::filesystem::file_type> targetType = GetOwnFileType(target, entryError);
				if (!targetType)
					return SetCopyError(outError, fmt::format("Cannot read '{}': {}", ToUTF8(target), entryError.message()));
				if (*targetType == std::filesystem::file_type::not_found)
					std::filesystem::create_directory(target, entryError);
				else if (*targetType != std::filesystem::file_type::directory)
					return SetCopyError(outError, fmt::format("Cannot copy the directory '{}': '{}' exists and is not a directory", ToUTF8(source), ToUTF8(target)));
				if (entryError)
					return SetCopyError(outError, fmt::format("Cannot create '{}': {}", ToUTF8(target), entryError.message()));
			}
			else if (*sourceType == std::filesystem::file_type::regular)
			{
				// Overwriting a link would write through it, possibly outside the destination.
				const std::optional<std::filesystem::file_type> targetType = GetOwnFileType(target, entryError);
				if (!targetType)
					return SetCopyError(outError, fmt::format("Cannot read '{}': {}", ToUTF8(target), entryError.message()));
				if (*targetType != std::filesystem::file_type::not_found && *targetType != std::filesystem::file_type::regular)
					return SetCopyError(outError, fmt::format("Cannot copy '{}': '{}' exists and is not a regular file", ToUTF8(source), ToUTF8(target)));
				std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, entryError);
				if (entryError)
					return SetCopyError(outError, fmt::format("Cannot copy '{}' to '{}': {}", ToUTF8(source), ToUTF8(target), entryError.message()));
			}
			else
			{
				// Symbolic links, junctions (which MSVC's std::filesystem does not report as symbolic links) and special
				// files. Following a link could copy from outside the source tree: never descend into one.
				it.disable_recursion_pending();
				ST_CORE_WARN("Copying '{}': skipped '{}' (links and special files are not copied)", ToUTF8(from), ToUTF8(source));
			}

			it.increment(error);
			if (error)
				return SetCopyError(outError, fmt::format("Cannot list '{}': {}", ToUTF8(from), error.message()));
		}
		return true;
	}

	std::optional<uint64_t> FileSystem::GetFileSize(const std::filesystem::path& path)
	{
		std::error_code error;
		const uintmax_t size = std::filesystem::file_size(path, error);
		if (error)
			return std::nullopt;
		return static_cast<uint64_t>(size);
	}

	std::optional<int64_t> FileSystem::GetLastWriteTime(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::file_time_type time = std::filesystem::last_write_time(path, error);
		if (error)
			return std::nullopt;
		return static_cast<int64_t>(time.time_since_epoch().count());
	}

	std::filesystem::path FileSystem::FromUTF8(std::string_view utf8)
	{
		return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
	}

	std::string FileSystem::ToUTF8(const std::filesystem::path& path)
	{
		const std::u8string text = path.generic_u8string();
		return std::string(reinterpret_cast<const char*>(text.data()), text.size());
	}

	std::filesystem::path FileSystem::RemoveTrailingSeparators(std::filesystem::path path)
	{
		// The last element of "a/b/" is an empty file name; its parent is "a/b". A root has no relative part to remove.
		while (!path.has_filename() && path.has_relative_path())
			path = path.parent_path();
		return path;
	}

	std::filesystem::path FileSystem::GetRelativePath(const std::filesystem::path& path, const std::filesystem::path& base)
	{
		const std::filesystem::path relative = path.lexically_normal().lexically_relative(base.lexically_normal());
		if (relative.empty())
			return {};

		const std::filesystem::path first = *relative.begin();
		if (first == "..")
			return {};

		return relative;
	}

	bool FileSystem::IsInside(const std::filesystem::path& path, const std::filesystem::path& base)
	{
		return !GetRelativePath(path, base).empty();
	}

	bool FileSystem::IsInsideResolved(const std::filesystem::path& path, const std::filesystem::path& base)
	{
		std::error_code error;
		const std::filesystem::path resolvedPath = std::filesystem::weakly_canonical(path, error);
		if (error)
			return false;
		const std::filesystem::path resolvedBase = std::filesystem::weakly_canonical(base, error);
		if (error)
			return false;
		return IsInside(resolvedPath, resolvedBase);
	}

	std::filesystem::path FileSystem::GetUniquePath(const std::filesystem::path& desiredPath)
	{
		if (!Exists(desiredPath))
			return desiredPath;

		const std::filesystem::path parent = desiredPath.parent_path();
		const std::string stem = ToUTF8(desiredPath.stem());
		const std::string extension = ToUTF8(desiredPath.extension());
		for (uint32_t index = 1; index < 100000; index++)
		{
			std::filesystem::path candidate = parent / FromUTF8(fmt::format("{} ({}){}", stem, index, extension));
			if (!Exists(candidate))
				return candidate;
		}
		return desiredPath;
	}

}
