#include "stpch.h"
#include "Strata/Network/EditorSession.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Platform.h"
#include "Strata/Network/JsonRpc.h"

#include <random>

namespace Strata
{

	namespace
	{

		constexpr const char* c_SessionDirectoryVariable = "STRATA_SESSION_DIR";
		constexpr const char* c_ProjectDataDirectory = ".strata";
		constexpr const char* c_ProjectSessionFileName = "EditorSession.json";
		constexpr int c_WriteAttempts = 10;
		constexpr std::chrono::milliseconds c_WriteRetryDelay = std::chrono::milliseconds(20);

		std::optional<uint64_t> GetUnsigned(const nlohmann::json& object, const char* key)
		{
			const auto it = object.find(key);
			if (it == object.end())
				return std::nullopt;
			if (it->is_number_unsigned())
				return it->get<uint64_t>();
			if (it->is_number_integer() && it->get<int64_t>() >= 0)
				return static_cast<uint64_t>(it->get<int64_t>());
			return std::nullopt;
		}

		std::string GetString(const nlohmann::json& object, const char* key)
		{
			const auto it = object.find(key);
			return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
		}

		std::string SerializeSession(const EditorSessionInfo& info)
		{
			return info.ToJson().dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
		}

		// Tools polling for sessions briefly hold session files open, and Windows refuses to replace a file that is
		// open without delete sharing. Such failures are transient, so the atomic write is retried for a moment.
		bool WriteSessionFile(const std::filesystem::path& path, const std::string& text)
		{
			for (int attempt = 0; attempt < c_WriteAttempts; attempt++)
			{
				if (attempt > 0)
					std::this_thread::sleep_for(c_WriteRetryDelay);
				if (FileSystem::WriteText(path, text))
					return true;
			}
			return false;
		}

		// Restricts a path to its owner (POSIX permission bits; on Windows this only keeps it writable, and
		// per-user profile ACLs already protect it). Best effort: failures leave the default permissions.
		void RestrictToOwner(const std::filesystem::path& path, bool isDirectory)
		{
			const std::filesystem::perms permissions = isDirectory
				? std::filesystem::perms::owner_all
				: std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
			std::error_code error;
			std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, error);
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// EditorSessionInfo
	////////////////////////////////////////////////////////////////////////////////

	nlohmann::json EditorSessionInfo::ToJson() const
	{
		nlohmann::json json = nlohmann::json::object();
		json["ProcessId"] = ProcessId;
		json["Port"] = Port;
		json["Token"] = Token;
		json["ProjectPath"] = ProjectPath;
		json["EditorVersion"] = EditorVersion;
		json["Headless"] = Headless;
		json["StartedAt"] = StartedAt;
		return json;
	}

	std::optional<EditorSessionInfo> EditorSessionInfo::FromJson(const nlohmann::json& json)
	{
		if (!json.is_object())
			return std::nullopt;

		const std::optional<uint64_t> processId = GetUnsigned(json, "ProcessId");
		const std::optional<uint64_t> port = GetUnsigned(json, "Port");
		if (!processId || *processId == 0 || *processId > UINT32_MAX)
			return std::nullopt;
		if (!port || *port == 0 || *port > UINT16_MAX)
			return std::nullopt;

		EditorSessionInfo info;
		info.ProcessId = static_cast<uint32_t>(*processId);
		info.Port = static_cast<uint16_t>(*port);
		info.Token = GetString(json, "Token");
		info.ProjectPath = GetString(json, "ProjectPath");
		info.EditorVersion = GetString(json, "EditorVersion");
		info.StartedAt = GetString(json, "StartedAt");
		const auto headless = json.find("Headless");
		info.Headless = headless != json.end() && headless->is_boolean() && headless->get<bool>();
		return info;
	}

	////////////////////////////////////////////////////////////////////////////////
	// EditorSession
	////////////////////////////////////////////////////////////////////////////////

	std::filesystem::path EditorSession::GetSessionDirectory()
	{
		if (const std::optional<std::string> overridden = Platform::GetEnvVar(c_SessionDirectoryVariable); overridden && !overridden->empty())
			return FileSystem::FromUTF8(*overridden);
		return Platform::GetUserDataDirectory("Strata") / "Sessions";
	}

	std::filesystem::path EditorSession::GetSessionFilePath(uint32_t processId)
	{
		return GetSessionFilePath(GetSessionDirectory(), processId);
	}

	std::filesystem::path EditorSession::GetSessionFilePath(const std::filesystem::path& sessionDirectory, uint32_t processId)
	{
		return sessionDirectory / FileSystem::FromUTF8(fmt::format("{}.json", processId));
	}

	std::filesystem::path EditorSession::GetProjectSessionFilePath(const std::filesystem::path& projectDirectory)
	{
		return projectDirectory / c_ProjectDataDirectory / c_ProjectSessionFileName;
	}

	bool EditorSession::WriteSessionFiles(const EditorSessionInfo& info)
	{
		const std::string text = SerializeSession(info);
		bool success = true;

		const std::filesystem::path sessionDirectory = GetSessionDirectory();
		if (!FileSystem::CreateDirectories(sessionDirectory))
		{
			ST_CORE_ERROR("EditorSession: failed to create '{}'", FileSystem::ToUTF8(sessionDirectory));
			return false;
		}
		RestrictToOwner(sessionDirectory, true);

		const std::filesystem::path sessionFile = GetSessionFilePath(sessionDirectory, info.ProcessId);
		if (WriteSessionFile(sessionFile, text))
		{
			RestrictToOwner(sessionFile, false);
		}
		else
		{
			ST_CORE_ERROR("EditorSession: failed to write '{}'", FileSystem::ToUTF8(sessionFile));
			success = false;
		}

		if (!info.ProjectPath.empty())
		{
			const std::filesystem::path projectFile = GetProjectSessionFilePath(FileSystem::FromUTF8(info.ProjectPath));
			if (WriteSessionFile(projectFile, text))
			{
				RestrictToOwner(projectFile, false);
			}
			else
			{
				ST_CORE_ERROR("EditorSession: failed to write '{}'", FileSystem::ToUTF8(projectFile));
				success = false;
			}
		}
		return success;
	}

	void EditorSession::RemoveSessionFiles(const EditorSessionInfo& info)
	{
		std::error_code error;
		std::filesystem::remove(GetSessionFilePath(info.ProcessId), error);

		if (info.ProjectPath.empty())
			return;

		// Another editor may have opened the project since; only remove the file if it is still ours.
		const std::filesystem::path projectFile = GetProjectSessionFilePath(FileSystem::FromUTF8(info.ProjectPath));
		const std::optional<EditorSessionInfo> current = ReadSessionFile(projectFile);
		if (current && current->ProcessId == info.ProcessId && current->Token == info.Token)
			std::filesystem::remove(projectFile, error);
	}

	std::vector<EditorSessionInfo> EditorSession::FindSessions()
	{
		return FindSessions(GetSessionDirectory());
	}

	std::vector<EditorSessionInfo> EditorSession::FindSessions(const std::filesystem::path& sessionDirectory)
	{
		struct FoundSession
		{
			EditorSessionInfo Info;
			int64_t WriteTime = 0;
		};

		std::vector<FoundSession> found;
		std::error_code error;
		for (std::filesystem::directory_iterator it(sessionDirectory, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
		{
			std::error_code entryError;
			if (!it->is_regular_file(entryError) || it->path().extension() != ".json")
				continue;

			std::optional<EditorSessionInfo> session = ReadSessionFile(it->path());
			if (!session)
				continue;
			found.push_back(FoundSession { std::move(*session), FileSystem::GetLastWriteTime(it->path()).value_or(0) });
		}

		// ISO-8601 UTC timestamps of a fixed format order lexicographically; the write time breaks ties.
		std::sort(found.begin(), found.end(), [](const FoundSession& left, const FoundSession& right)
		{
			if (left.Info.StartedAt != right.Info.StartedAt)
				return left.Info.StartedAt > right.Info.StartedAt;
			if (left.WriteTime != right.WriteTime)
				return left.WriteTime > right.WriteTime;
			return left.Info.ProcessId > right.Info.ProcessId;
		});

		std::vector<EditorSessionInfo> sessions;
		sessions.reserve(found.size());
		for (FoundSession& session : found)
			sessions.push_back(std::move(session.Info));
		return sessions;
	}

	std::optional<EditorSessionInfo> EditorSession::ReadSessionFile(const std::filesystem::path& path)
	{
		const std::optional<std::string> text = FileSystem::ReadText(path);
		if (!text)
			return std::nullopt;

		const std::optional<nlohmann::json> json = JsonRpc::Parse(*text);
		if (!json)
		{
			ST_CORE_WARN("EditorSession: '{}' is not valid JSON", FileSystem::ToUTF8(path));
			return std::nullopt;
		}
		return EditorSessionInfo::FromJson(*json);
	}

	std::optional<EditorSessionInfo> EditorSession::ReadProjectSession(const std::filesystem::path& projectDirectory)
	{
		return ReadSessionFile(GetProjectSessionFilePath(projectDirectory));
	}

	std::string EditorSession::GenerateSessionToken()
	{
		std::random_device device;
		std::string token;
		token.reserve(32);
		for (int index = 0; index < 4; index++)
			token += fmt::format("{:08x}", static_cast<uint32_t>(device()));
		return token;
	}

	std::string EditorSession::GetCurrentTimestamp()
	{
		const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
		const auto today = std::chrono::floor<std::chrono::days>(now);
		const std::chrono::year_month_day date(today);
		const std::chrono::hh_mm_ss<std::chrono::seconds> time(now - today);
		return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
			static_cast<int>(date.year()), static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
			time.hours().count(), time.minutes().count(), time.seconds().count());
	}

}
