#include "Editor/RecentProjects.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/Version.h>

#include <algorithm>
#include <chrono>
#include <system_error>

namespace Strata
{

	namespace
	{

		constexpr const char* c_Format = "RecentProjects";
		constexpr int64_t c_Version = 1;
		constexpr const char* c_FileName = "RecentProjects.json";

		// One spelling per file, so a project opened through another path (relative, different case on Windows, through a
		// link) is still one entry. Looks the path up on disk: only for the project being opened or named.
		std::filesystem::path NormalizePath(const std::filesystem::path& path)
		{
			std::error_code error;
			std::filesystem::path normalized = std::filesystem::weakly_canonical(path, error);
			if (error || normalized.empty())
			{
				normalized = std::filesystem::absolute(path, error);
				if (error)
					normalized = path;
			}
			return normalized.lexically_normal();
		}

		// Stored paths were normalized when they were added: they compare without looking at the disk.
		bool IsSamePath(const std::filesystem::path& stored, const std::filesystem::path& path)
		{
			return stored.lexically_normal() == path.lexically_normal();
		}

		const RecentProject* FindProject(const std::vector<RecentProject>& projects, const std::filesystem::path& path)
		{
			const auto found = std::find_if(projects.begin(), projects.end(), [&path](const RecentProject& project) { return IsSamePath(project.Path, path); });
			return found != projects.end() ? &*found : nullptr;
		}

		int64_t GetUnixTime()
		{
			return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
		}

	}

	std::optional<std::filesystem::path> RecentProjects::GetDefaultFile()
	{
		if (const std::optional<std::string> overridden = Platform::GetEnvVar(c_FileVariable); overridden && !overridden->empty())
			return FileSystem::FromUTF8(*overridden);
		if (const std::optional<std::filesystem::path> userData = Platform::FindUserDataDirectory("Strata"))
			return *userData / c_FileName;
		return std::nullopt;
	}

	RecentProjects::RecentProjects(std::filesystem::path file, bool readOnly)
		: m_File(std::move(file)), m_ReadOnly(readOnly)
	{
		Reload();
	}

	void RecentProjects::Add(const std::string& name, const std::filesystem::path& projectFile)
	{
		// Other editors of the user may have added projects since the file was read.
		Reload();
		Record({ false, RecentProject { name, NormalizePath(projectFile), GetUnixTime(), c_EngineVersion } });
		Save();
	}

	bool RecentProjects::Remove(const std::filesystem::path& projectFile)
	{
		Reload();
		// As listed (the launcher's cards, editor.recentProjects), else under another spelling.
		const RecentProject* project = FindProject(m_Projects, projectFile);
		if (!project)
			project = FindProject(m_Projects, NormalizePath(projectFile));
		if (!project)
			return false;
		Record({ true, RecentProject { {}, project->Path, 0, {} } });
		Save();
		return true;
	}

	void RecentProjects::Forget(const std::vector<std::filesystem::path>& projectFiles)
	{
		if (projectFiles.empty())
			return;
		Reload();
		bool changed = false;
		for (const std::filesystem::path& projectFile : projectFiles)
		{
			if (const RecentProject* project = FindProject(m_Projects, projectFile))
			{
				Record({ true, RecentProject { {}, project->Path, 0, {} } });
				changed = true;
			}
		}
		if (changed)
			Save();
	}

	std::vector<RecentProject> RecentProjects::GetProjects() const
	{
		std::vector<RecentProject> projects;
		for (const RecentProject& project : m_Projects)
		{
			if (FileSystem::IsRegularFile(project.Path))
				projects.push_back(project);
		}
		return projects;
	}

	void RecentProjects::Reload()
	{
		// A list kept in memory only has nothing to read.
		if (!m_File.empty())
			ApplyFile(ReadFile(m_File));
	}

	void RecentProjects::ApplyFile(const RecentProjectsFile& read)
	{
		if (m_File.empty())
			return;
		if (read.Projects)
		{
			m_ReportedUnreadable = false;
			m_Projects = *read.Projects;
		}
		else
		{
			// Reported once: the list is read again whenever a project opens and while the launcher shows.
			if (!m_ReportedUnreadable)
				ST_WARN("The list of recent projects '{}' is ignored: {}", FileSystem::ToUTF8(m_File), read.Error);
			m_ReportedUnreadable = true;
			m_Projects.clear();
		}
		// What this session changed and the file does not hold (yet): a read-only list's projects, or a failed save.
		for (const Edit& edit : m_UnsavedEdits)
			ApplyEdit(m_Projects, edit);
	}

	RecentProjectsFile RecentProjects::ReadFile(const std::filesystem::path& file)
	{
		RecentProjectsFile read;
		if (file.empty() || !FileSystem::Exists(file))
		{
			read.Projects.emplace();
			return read;
		}
		if (const std::optional<std::string> text = FileSystem::ReadText(file); !text)
			read.Error = "it cannot be read";
		else if (const std::optional<nlohmann::json> json = JsonUtils::Parse(*text, &read.Error); json)
			read.Projects = FromJson(*json, &read.Error);
		return read;
	}

	RecentProjectsCheck RecentProjects::CheckProjects(const std::vector<std::filesystem::path>& projectFiles)
	{
		RecentProjectsCheck check;
		for (const std::filesystem::path& projectFile : projectFiles)
		{
			if (FileSystem::IsRegularFile(projectFile))
				continue;
			check.Missing.push_back(projectFile);
			// Deleted, or out of reach? The folder that held the project's folder tells: it is there when the drive or
			// share is, so then the project itself was deleted.
			const std::filesystem::path place = projectFile.parent_path().parent_path();
			if (!place.empty() && FileSystem::IsDirectory(place))
				check.Gone.push_back(projectFile);
		}
		return check;
	}

	void RecentProjects::Record(Edit edit)
	{
		m_ChangeCount++;
		ApplyEdit(m_Projects, edit);
		// Only the latest change of a project counts.
		std::erase_if(m_UnsavedEdits, [&edit](const Edit& unsaved) { return IsSamePath(unsaved.Project.Path, edit.Project.Path); });
		m_UnsavedEdits.push_back(std::move(edit));
	}

	void RecentProjects::ApplyEdit(std::vector<RecentProject>& projects, const Edit& edit)
	{
		std::erase_if(projects, [&edit](const RecentProject& project) { return IsSamePath(project.Path, edit.Project.Path); });
		if (edit.Removal)
			return;
		projects.insert(projects.begin(), edit.Project);
		if (projects.size() > c_MaxProjects)
			projects.resize(c_MaxProjects);
	}

	void RecentProjects::Save()
	{
		// In memory only, the list itself holds every change (nothing is read over it).
		if (m_File.empty())
		{
			m_UnsavedEdits.clear();
			return;
		}
		// Read-only: the changes stay in memory and apply over every read of the file.
		if (m_ReadOnly)
			return;
		if (!FileSystem::CreateDirectories(m_File.parent_path()) || !FileSystem::WriteText(m_File, JsonUtils::Dump(ToJson(m_Projects), 1, '\t') + "\n"))
		{
			// Kept like a read-only list's changes, and written by the next save that succeeds.
			ST_WARN("The list of recent projects could not be saved to '{}'", FileSystem::ToUTF8(m_File));
			return;
		}
		m_UnsavedEdits.clear();
	}

	nlohmann::json RecentProjects::ToJson(const std::vector<RecentProject>& projects)
	{
		nlohmann::json list = nlohmann::json::array();
		for (const RecentProject& project : projects)
		{
			list.push_back({
				{ "name", project.Name },
				{ "path", FileSystem::ToUTF8(project.Path) },
				{ "lastOpened", project.LastOpened },
				{ "engineVersion", project.EngineVersion } });
		}
		return { { "Strata", { { "Format", c_Format }, { "Version", c_Version } } }, { "Projects", std::move(list) } };
	}

	std::optional<std::vector<RecentProject>> RecentProjects::FromJson(const nlohmann::json& json, std::string* outError)
	{
		auto fail = [outError](std::string message) -> std::optional<std::vector<RecentProject>>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};
		const nlohmann::json* header = JsonUtils::Find(json, "Strata");
		if (!header || JsonUtils::GetString(*header, "Format") != c_Format)
			return fail(fmt::format("not a list of recent projects (expected the format \"{}\")", c_Format));
		const int64_t version = JsonUtils::GetInt(*header, "Version", 0);
		if (version != c_Version)
			return fail(fmt::format("unsupported version {} (supported: {})", version, c_Version));
		const nlohmann::json* list = JsonUtils::Find(json, "Projects");
		if (!list || !list->is_array())
			return fail("'Projects' must be an array");

		std::vector<RecentProject> projects;
		for (size_t index = 0; index < list->size(); index++)
		{
			const nlohmann::json& entry = (*list)[index];
			const nlohmann::json* name = JsonUtils::Find(entry, "name");
			const nlohmann::json* path = JsonUtils::Find(entry, "path");
			const nlohmann::json* lastOpened = JsonUtils::Find(entry, "lastOpened");
			const nlohmann::json* engineVersion = JsonUtils::Find(entry, "engineVersion");
			if (!name || !name->is_string() || !path || !path->is_string() || path->get_ref<const std::string&>().empty() || !lastOpened
				|| !lastOpened->is_number_integer() || (engineVersion && !engineVersion->is_string()))
			{
				return fail(fmt::format("entry {} is not a project (name, path, lastOpened and engineVersion)", index));
			}
			// An unsigned number beyond the signed range is out of range too (reading it as int64_t would wrap it).
			const bool inRange = lastOpened->is_number_unsigned() ? lastOpened->get<uint64_t>() <= static_cast<uint64_t>(c_MaxLastOpened)
				: (lastOpened->get<int64_t>() >= 0 && lastOpened->get<int64_t>() <= c_MaxLastOpened);
			if (!inRange)
				return fail(fmt::format("entry {} was last opened at {}, which is no time between 1970 and the year 9999", index, lastOpened->dump()));
			if (projects.size() < c_MaxProjects)
			{
				projects.push_back(RecentProject { name->get<std::string>(), FileSystem::FromUTF8(path->get_ref<const std::string&>()), lastOpened->get<int64_t>(),
					engineVersion ? engineVersion->get<std::string>() : std::string() });
			}
		}
		return projects;
	}

}
