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
		int64_t LastOpened = 0;     // Seconds since the Unix epoch, within [0, RecentProjects::c_MaxLastOpened]
		std::string EngineVersion;  // Version of the editor that opened it last

		bool operator==(const RecentProject&) const = default;
	};

	// What a read of the list's file found (RecentProjects::ReadFile).
	struct RecentProjectsFile
	{
		// The list in the file: empty when there is no file, nullopt when it exists but is not such a list (Error says why).
		std::optional<std::vector<RecentProject>> Projects;
		std::string Error;
	};

	// Which projects of a list are there (RecentProjects::CheckProjects).
	struct RecentProjectsCheck
	{
		std::vector<std::filesystem::path> Missing; // Project files that are not there now
		// The missing ones whose place is still there (the folder that held the project's folder exists): the project was
		// deleted, so it can leave the list. The others may only be out of reach (a network share or a drive that is not
		// connected) and stay on it.
		std::vector<std::filesystem::path> Gone;
	};

	// The projects the editor opened most recently, most recent first, for the launcher and editor.recentProjects. Kept
	// in a JSON file shared by every editor of the user:
	//   {"Strata": {"Format": "RecentProjects", "Version": 1},
	//    "Projects": [{"name": ..., "path": <UTF-8>, "lastOpened": <seconds>, "engineVersion": ...}]}
	// Projects whose file no longer exists are left out of GetProjects. A file that cannot be read as such a list is ignored
	// (one warning) and replaced by the next save.
	//
	// Changes (Add, Remove, Forget) read the file first, so projects other editors added meanwhile are kept, and are kept
	// in memory until they are saved: a read-only list (scripted runs) or one whose file cannot be written applies them
	// again over every read of the file, so the session's own projects never drop out of its list.
	//
	// Only the project being added is looked up on disk by a change (its path is made canonical); the paths of the others
	// are compared as stored, so a project on a drive that is out of reach never holds up the editor. Main thread only,
	// except the static ReadFile and CheckProjects, which the launcher runs on an I/O thread.
	class RecentProjects
	{
	public:
		static constexpr size_t c_MaxProjects = 12;
		static constexpr const char* c_FileVariable = "STRATA_RECENT_PROJECTS";
		// The last second of the year 9999 (UTC): later times, and times before 1970, are not valid in a list.
		static constexpr int64_t c_MaxLastOpened = 253'402'300'799;

		// The user's list: the path in the environment variable STRATA_RECENT_PROJECTS when it is set and not empty (tests
		// set it, so editors they start never touch the user's list), else RecentProjects.json in the user data directory
		// (Platform::FindUserDataDirectory("Strata")); nullopt when there is no such directory.
		static std::optional<std::filesystem::path> GetDefaultFile();

		// file: where the list is kept (empty: in memory only). readOnly: the file is read but never written, e.g. by
		// scripted runs, which are not the user's sessions. Reads the file.
		explicit RecentProjects(std::filesystem::path file = {}, bool readOnly = false);

		// Records that a project was opened now: it moves to the front (added if new; the same file is recognized under
		// other spellings of its path), the oldest beyond c_MaxProjects drop out, and the list is saved.
		void Add(const std::string& name, const std::filesystem::path& projectFile);
		// Takes a project off the list (the launcher's "Remove from list"; its files stay) and saves the list. False when it
		// was not on it.
		bool Remove(const std::filesystem::path& projectFile);
		// Takes projects that were deleted off the list (RecentProjectsCheck::Gone) and saves it when any was on it.
		void Forget(const std::vector<std::filesystem::path>& projectFiles);

		// The list, most recent first, without projects whose file is not there (checked now, on this thread).
		std::vector<RecentProject> GetProjects() const;
		// The whole list, also projects whose file is not there (the launcher checks them on an I/O thread).
		const std::vector<RecentProject>& GetAllProjects() const { return m_Projects; }
		// Reads the file again (ReadFile and ApplyFile).
		void Reload();
		// Takes a read of the file (ReadFile, e.g. done on an I/O thread) as the list, with this session's unsaved changes
		// applied over it. Reports a file that cannot be read once (until it reads again). Ignored for a list in memory.
		void ApplyFile(const RecentProjectsFile& read);

		const std::filesystem::path& GetFile() const { return m_File; }
		bool IsReadOnly() const { return m_ReadOnly; }
		// Counts the changes (Add, Remove, Forget): a read of the file started before a change may miss it.
		uint64_t GetChangeCount() const { return m_ChangeCount; }

		// Reads a list's file (an empty path is no file). Thread-safe.
		static RecentProjectsFile ReadFile(const std::filesystem::path& file);
		// Looks up which of the project files are there. Thread-safe; may take long for paths out of reach.
		static RecentProjectsCheck CheckProjects(const std::vector<std::filesystem::path>& projectFiles);

		static nlohmann::json ToJson(const std::vector<RecentProject>& projects);
		// Fails on anything but a list of the format above (entries are validated: name and path strings, lastOpened a
		// number of seconds within [0, c_MaxLastOpened]); at most c_MaxProjects entries are kept.
		static std::optional<std::vector<RecentProject>> FromJson(const nlohmann::json& json, std::string* outError = nullptr);
	private:
		// A change of the list: a project opened (to the front) or taken off.
		struct Edit
		{
			bool Removal = false;
			RecentProject Project; // Only its path for a removal
		};

		// Applies a change, keeps it until it is saved and saves the list.
		void Record(Edit edit);
		static void ApplyEdit(std::vector<RecentProject>& projects, const Edit& edit);
		void Save();
	private:
		std::filesystem::path m_File;
		bool m_ReadOnly = false;
		bool m_ReportedUnreadable = false; // The file was reported as unreadable (once, until it reads again)
		std::vector<RecentProject> m_Projects;
		// Changes the file does not hold yet, oldest first, at most one per project (the latest).
		std::vector<Edit> m_UnsavedEdits;
		uint64_t m_ChangeCount = 0;
	};

}
