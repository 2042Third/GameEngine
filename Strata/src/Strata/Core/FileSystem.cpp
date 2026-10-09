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

	bool FileSystem::CopyDirectory(const std::filesystem::path& from, const std::filesystem::path& to)
	{
		std::error_code error;
		std::filesystem::create_directories(to, error);
		std::filesystem::copy(from, to, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
		return !error;
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
