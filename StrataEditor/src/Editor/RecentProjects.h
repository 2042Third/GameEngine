#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	struct RecentProject
	{
		std::string Name;
		std::filesystem::path Path; // The project file (.stproj)
		int64_t LastOpened = 0;     // Seconds since the Unix epoch
		std::string EngineVersion;  // Version of the editor that opened it last

		bool operator==(const RecentProject&) const = default;
	};

	// The projects the editor opened most recently, most recent first, for the launcher and editor.recentProjects. Kept
	// in a JSON file shared by every editor of the user:
	//   {"Strata": {"Format": "RecentProjects", "Version": 1},
	//    "Projects": [{"name": ..., "path": <UTF-8>, "lastOpened": <seconds>, "engineVersion": ...}]}
	// Projects whose file no longer exists are left out. A file that cannot be read as such a list is ignored (one
	// warning) and replaced by the next save. Main thread only.
	class RecentProjects
	{
	public:
		static constexpr size_t c_MaxProjects = 12;
		static constexpr const char* c_FileVariable = "STRATA_RECENT_PROJECTS";

		// The user's list: the path in the environment variable STRATA_RECENT_PROJECTS when it is set and not empty (tests
		// set it, so editors they start never touch the user's list), else RecentProjects.json in the user data directory
		// (Platform::FindUserDataDirectory("Strata")); nullopt when there is no such directory.
		static std::optional<std::filesystem::path> GetDefaultFile();

		// file: where the list is kept (empty: in memory only). readOnly: the file is read but never written, e.g. by
		// scripted runs, which are not the user's sessions. Reads the file.
		explicit RecentProjects(std::filesystem::path file = {}, bool readOnly = false);

		// Records that a project was opened now: it moves to the front (added if new; the same file is recognized under
		// other spellings of its path), the oldest beyond c_MaxProjects drop out, and the list is saved. The file is read
		// first, so projects other editors added meanwhile are kept.
		void Add(const std::string& name, const std::filesystem::path& projectFile);
		// Takes a project off the list (the launcher's "Remove from list"; its files stay) and saves the list. False when it
		// was not on it. The file is read first, like for Add.
		bool Remove(const std::filesystem::path& projectFile);
		// The list, most recent first, without projects whose file is gone.
		const std::vector<RecentProject>& GetProjects();
		// Reads the file again: a missing file is an empty list.
		void Reload();

		const std::filesystem::path& GetFile() const { return m_File; }
		bool IsReadOnly() const { return m_ReadOnly; }

		static nlohmann::json ToJson(const std::vector<RecentProject>& projects);
		// Fails on anything but a list of the format above (entries are validated: name and path strings, a number of
		// seconds); at most c_MaxProjects entries are kept.
		static std::optional<std::vector<RecentProject>> FromJson(const nlohmann::json& json, std::string* outError = nullptr);
	private:
		void Prune();
		void Save();
	private:
		std::filesystem::path m_File;
		bool m_ReadOnly = false;
		bool m_ReportedUnreadable = false; // The file was reported as unreadable (once, until it reads again)
		std::vector<RecentProject> m_Projects;
	};

}
