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
		// link) is still one entry.
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
		const std::filesystem::path path = NormalizePath(projectFile);
		std::erase_if(m_Projects, [&path](const RecentProject& project) { return NormalizePath(project.Path) == path; });
		m_Projects.insert(m_Projects.begin(), RecentProject { name, path, GetUnixTime(), c_EngineVersion });
		if (m_Projects.size() > c_MaxProjects)
			m_Projects.resize(c_MaxProjects);
		Save();
	}

	const std::vector<RecentProject>& RecentProjects::GetProjects()
	{
		Prune();
		return m_Projects;
	}

	void RecentProjects::Reload()
	{
		// A list kept in memory only has nothing to read.
		if (m_File.empty())
			return;
		m_Projects.clear();
		if (!FileSystem::Exists(m_File))
		{
			m_ReportedUnreadable = false;
			return;
		}

		std::string error;
		std::optional<std::vector<RecentProject>> projects;
		if (const std::optional<std::string> text = FileSystem::ReadText(m_File); !text)
		{
			error = "it cannot be read";
		}
		else if (const std::optional<nlohmann::json> json = JsonUtils::Parse(*text, &error); json)
		{
			projects = FromJson(*json, &error);
		}
		if (!projects)
		{
			// Reported once: the list is read again whenever a project opens.
			if (!m_ReportedUnreadable)
				ST_WARN("The list of recent projects '{}' is ignored: {}", FileSystem::ToUTF8(m_File), error);
			m_ReportedUnreadable = true;
			return;
		}
		m_ReportedUnreadable = false;
		m_Projects = std::move(*projects);
		Prune();
	}

	void RecentProjects::Prune()
	{
		std::erase_if(m_Projects, [](const RecentProject& project) { return !FileSystem::IsRegularFile(project.Path); });
	}

	void RecentProjects::Save()
	{
		if (m_File.empty() || m_ReadOnly)
			return;
		if (!FileSystem::CreateDirectories(m_File.parent_path()) || !FileSystem::WriteText(m_File, JsonUtils::Dump(ToJson(m_Projects), 1, '\t') + "\n"))
			ST_WARN("The list of recent projects could not be saved to '{}'", FileSystem::ToUTF8(m_File));
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
		for (const nlohmann::json& entry : *list)
		{
			const nlohmann::json* name = JsonUtils::Find(entry, "name");
			const nlohmann::json* path = JsonUtils::Find(entry, "path");
			const nlohmann::json* lastOpened = JsonUtils::Find(entry, "lastOpened");
			const nlohmann::json* engineVersion = JsonUtils::Find(entry, "engineVersion");
			if (!name || !name->is_string() || !path || !path->is_string() || path->get_ref<const std::string&>().empty() || !lastOpened
				|| !lastOpened->is_number_integer() || (engineVersion && !engineVersion->is_string()))
			{
				return fail(fmt::format("entry {} is not a project (name, path, lastOpened and engineVersion)", projects.size()));
			}
			if (projects.size() < c_MaxProjects)
			{
				projects.push_back(RecentProject { name->get<std::string>(), FileSystem::FromUTF8(path->get_ref<const std::string&>()), lastOpened->get<int64_t>(),
					engineVersion ? engineVersion->get<std::string>() : std::string() });
			}
		}
		return projects;
	}

}
