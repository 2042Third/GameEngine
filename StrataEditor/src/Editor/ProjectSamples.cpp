#include "Editor/ProjectSamples.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Project/Project.h>

#include <algorithm>
#include <system_error>

namespace Strata
{

	namespace
	{

		constexpr const char* c_Format = "Samples";
		constexpr int64_t c_Version = 1;
		// The index is a few hundred bytes; the limit only guards against reading something else.
		constexpr size_t c_MaxIndexSize = 1024 * 1024;

		bool Fail(std::string* outError, std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		}

		// A sample's directory is one directory of the samples directory: the index cannot point elsewhere.
		bool IsValidDirectoryName(const std::string& name)
		{
			if (name.empty() || name == "." || name == ".." || name == ProjectSamples::c_LocalDataDirectory)
				return false;
			return std::none_of(name.begin(), name.end(), [](char character)
			{
				const auto value = static_cast<unsigned char>(character);
				return value < 0x20 || character == '/' || character == '\\' || character == ':';
			});
		}

		std::filesystem::path Resolve(const std::filesystem::path& path)
		{
			std::error_code error;
			std::filesystem::path resolved = std::filesystem::weakly_canonical(path, error);
			return error ? std::filesystem::absolute(path, error).lexically_normal() : resolved;
		}

	}

	std::filesystem::path ProjectSamples::GetDefaultDirectory()
	{
		return Platform::GetExecutableDirectory() / "Samples";
	}

	std::optional<std::vector<ProjectSample>> ProjectSamples::List(const std::filesystem::path& directory, std::string* outError)
	{
		const std::filesystem::path indexFile = directory / c_IndexFileName;
		const std::optional<uint64_t> size = FileSystem::IsRegularFile(indexFile) ? FileSystem::GetFileSize(indexFile) : std::nullopt;
		if (!size)
		{
			Fail(outError, fmt::format("No samples in '{}': its index {} is missing", FileSystem::ToUTF8(directory), c_IndexFileName));
			return std::nullopt;
		}
		if (*size > c_MaxIndexSize)
		{
			Fail(outError, fmt::format("'{}' is too large for a samples index", FileSystem::ToUTF8(indexFile)));
			return std::nullopt;
		}
		const std::optional<std::string> text = FileSystem::ReadText(indexFile);
		if (!text)
		{
			Fail(outError, fmt::format("'{}' cannot be read", FileSystem::ToUTF8(indexFile)));
			return std::nullopt;
		}
		std::string error;
		const std::optional<nlohmann::json> index = JsonUtils::Parse(*text, &error);
		if (!index)
		{
			Fail(outError, fmt::format("'{}' is not valid JSON: {}", FileSystem::ToUTF8(indexFile), error));
			return std::nullopt;
		}
		const nlohmann::json* header = JsonUtils::Find(*index, "Strata");
		if (!header || JsonUtils::GetString(*header, "Format") != c_Format || JsonUtils::GetInt(*header, "Version", 0) != c_Version)
		{
			Fail(outError, fmt::format("'{}' is not a samples index (format \"{}\", version {})", FileSystem::ToUTF8(indexFile), c_Format, c_Version));
			return std::nullopt;
		}
		const nlohmann::json* entries = JsonUtils::Find(*index, "Samples");
		if (!entries || !entries->is_array())
		{
			Fail(outError, fmt::format("'{}' has no \"Samples\" array", FileSystem::ToUTF8(indexFile)));
			return std::nullopt;
		}

		std::vector<ProjectSample> samples;
		std::vector<std::string> ids;
		for (size_t entryIndex = 0; entryIndex < entries->size(); entryIndex++)
		{
			const nlohmann::json& entry = (*entries)[entryIndex];
			const nlohmann::json* sampleDirectory = JsonUtils::Find(entry, "Directory");
			const nlohmann::json* name = JsonUtils::Find(entry, "Name");
			const nlohmann::json* description = JsonUtils::Find(entry, "Description");
			if (!sampleDirectory || !sampleDirectory->is_string() || !IsValidDirectoryName(sampleDirectory->get<std::string>()) || !name
				|| !name->is_string() || name->get_ref<const std::string&>().empty() || (description && !description->is_string()))
			{
				Fail(outError, fmt::format("Sample {} of '{}' needs a Directory (one directory name), a Name and a text Description", entryIndex,
					FileSystem::ToUTF8(indexFile)));
				return std::nullopt;
			}
			ProjectSample sample;
			sample.Id = sampleDirectory->get<std::string>();
			sample.Name = name->get<std::string>();
			sample.Description = description ? description->get<std::string>() : std::string();
			if (std::find(ids.begin(), ids.end(), sample.Id) != ids.end())
			{
				Fail(outError, fmt::format("'{}' lists the sample '{}' twice", FileSystem::ToUTF8(indexFile), sample.Id));
				return std::nullopt;
			}
			ids.push_back(sample.Id);
			// A sample whose project is missing (e.g. a partial copy) is left out rather than failing the others.
			sample.ProjectFile = Project::FindProjectFile(directory / FileSystem::FromUTF8(sample.Id), &error);
			if (sample.ProjectFile.empty())
			{
				ST_WARN("The sample '{}' is left out: {}", sample.Id, error);
				continue;
			}
			samples.push_back(std::move(sample));
		}
		return samples;
	}

	std::filesystem::path ProjectSamples::Copy(const ProjectSample& sample, const std::filesystem::path& destination, std::string* outError)
	{
		const std::filesystem::path source = sample.ProjectFile.parent_path();
		std::error_code error;
		if (destination.empty() || !destination.is_absolute())
		{
			Fail(outError, fmt::format("The directory for the copy must be an absolute path ('{}')", FileSystem::ToUTF8(destination)));
			return {};
		}
		if (FileSystem::IsInside(Resolve(destination), Resolve(source)))
		{
			Fail(outError, "The copy cannot go inside the sample itself");
			return {};
		}
		const bool existed = FileSystem::Exists(destination);
		if (existed && (!FileSystem::IsDirectory(destination) || !std::filesystem::is_empty(destination, error) || error))
		{
			Fail(outError, fmt::format("'{}' already exists and is not an empty directory", FileSystem::ToUTF8(destination)));
			return {};
		}
		if (!FileSystem::CreateDirectories(destination))
		{
			Fail(outError, fmt::format("Could not create '{}'", FileSystem::ToUTF8(destination)));
			return {};
		}

		// Undoes a partial copy: the destination was missing or empty before.
		auto failCopy = [&](std::string message) -> std::filesystem::path
		{
			if (!existed)
			{
				FileSystem::Remove(destination);
			}
			else
			{
				// Listed first: removing entries while iterating the directory is not defined.
				std::vector<std::filesystem::path> copied;
				std::error_code listError;
				for (std::filesystem::directory_iterator entry(destination, listError), end; !listError && entry != end; entry.increment(listError))
					copied.push_back(entry->path());
				for (const std::filesystem::path& path : copied)
					FileSystem::Remove(path);
			}
			Fail(outError, std::move(message));
			return {};
		};

		std::filesystem::directory_iterator it(source, error);
		const std::filesystem::directory_iterator end;
		for (; !error && it != end; it.increment(error))
		{
			// Local editor data of whoever opened the sample is left out: the copy builds its own.
			const std::filesystem::path entry = it->path();
			if (entry.filename() == c_LocalDataDirectory)
				continue;
			const std::filesystem::path target = destination / entry.filename();
			std::error_code entryError;
			const std::filesystem::file_status status = it->symlink_status(entryError);
			std::string copyError;
			if (entryError)
			{
				return failCopy(fmt::format("Cannot read '{}': {}", FileSystem::ToUTF8(entry), entryError.message()));
			}
			else if (std::filesystem::is_directory(status))
			{
				if (!FileSystem::CopyDirectory(entry, target, &copyError))
					return failCopy(copyError);
			}
			else if (std::filesystem::is_regular_file(status))
			{
				if (!FileSystem::Copy(entry, target, false))
					return failCopy(fmt::format("Could not copy '{}' to '{}'", FileSystem::ToUTF8(entry), FileSystem::ToUTF8(target)));
			}
			else
			{
				// Like FileSystem::CopyDirectory: links and special files are not part of a project.
				ST_WARN("'{}' is not copied: it is neither a file nor a directory", FileSystem::ToUTF8(entry));
			}
		}
		if (error)
			return failCopy(fmt::format("Cannot list the sample '{}': {}", FileSystem::ToUTF8(source), error.message()));
		return destination / sample.ProjectFile.filename();
	}

}
