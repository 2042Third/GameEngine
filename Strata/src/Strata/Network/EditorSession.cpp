#include "stpch.h"
#include "Strata/Network/EditorSession.h"

#include "Strata/Core/Crypto.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Version.h"
#include "Strata/Network/JsonRpc.h"

namespace Strata
{

	namespace
	{

		constexpr const char* c_SessionDirectoryVariable = "STRATA_SESSION_DIR";
		constexpr const char* c_ProjectDataDirectory = ".strata";
		constexpr const char* c_ProjectSessionFileName = "EditorSession.json";
		// Session files are tiny; anything larger is not one, and is not read into memory.
		constexpr size_t c_MaxSessionFileSize = 64 * 1024;

		std::optional<uint64_t> GetUnsigned(const nlohmann::json& object, const char* key)
		{
			const nlohmann::json* value = JsonUtils::Find(object, key);
			if (!value)
				return std::nullopt;
			if (value->is_number_unsigned())
				return value->get<uint64_t>();
			if (value->is_number_integer() && value->get<int64_t>() >= 0)
				return static_cast<uint64_t>(value->get<int64_t>());
			return std::nullopt;
		}

		std::optional<uint32_t> GetProcessId(const nlohmann::json& object)
		{
			const std::optional<uint64_t> processId = GetUnsigned(object, "ProcessId");
			if (!processId || *processId == 0 || *processId > UINT32_MAX)
				return std::nullopt;
			return static_cast<uint32_t>(*processId);
		}

		std::optional<nlohmann::json> ReadJsonFile(const std::filesystem::path& path)
		{
			// Checked and read through one handle: a file planted in a shared project cannot be swapped for a FIFO,
			// a device or a link between the checks and the read.
			const std::optional<std::string> text = Platform::ReadRegularFile(path, c_MaxSessionFileSize);
			if (!text)
				return std::nullopt;
			return JsonRpc::Parse(*text);
		}

		// The project pointer: identifies the editor's process, but holds no secret.
		nlohmann::json MakeProjectPointer(const EditorSessionInfo& info)
		{
			nlohmann::json pointer = nlohmann::json::object();
			pointer["ProcessId"] = info.ProcessId;
			pointer["ProjectPath"] = info.ProjectPath;
			pointer["EditorVersion"] = info.EditorVersion;
			pointer["Headless"] = info.Headless;
			pointer["StartedAt"] = info.StartedAt;
			return pointer;
		}

		bool IsPointerTo(const nlohmann::json& pointer, const EditorSessionInfo& info)
		{
			return GetProcessId(pointer) == info.ProcessId && JsonUtils::GetString(pointer, "StartedAt") == info.StartedAt;
		}

		std::filesystem::path NormalizePath(const std::filesystem::path& path)
		{
			std::error_code error;
			std::filesystem::path absolute = std::filesystem::absolute(path, error);
			if (error)
				absolute = path;
			absolute = absolute.lexically_normal();
			if (!absolute.has_filename() && absolute.has_parent_path())
				absolute = absolute.parent_path(); // Ignore a trailing separator
			return absolute;
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// EditorSessionInfo
	////////////////////////////////////////////////////////////////////////////////

	nlohmann::json EditorSessionInfo::ToJson() const
	{
		nlohmann::json json = nlohmann::json::object();
		json["ProcessId"] = ProcessId;
		json["ProcessStartTime"] = ProcessStartTime;
		json["Address"] = Address;
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

		const std::optional<uint32_t> processId = GetProcessId(json);
		const std::optional<uint64_t> port = GetUnsigned(json, "Port");
		if (!processId || !port || *port == 0 || *port > UINT16_MAX)
			return std::nullopt;
		std::string token = JsonUtils::GetString(json, "Token");
		if (token.empty())
			return std::nullopt;

		EditorSessionInfo info;
		info.ProcessId = *processId;
		info.ProcessStartTime = GetUnsigned(json, "ProcessStartTime").value_or(0);
		info.Address = JsonUtils::GetString(json, "Address", info.Address);
		info.Port = static_cast<uint16_t>(*port);
		info.Token = std::move(token);
		info.ProjectPath = JsonUtils::GetString(json, "ProjectPath");
		info.EditorVersion = JsonUtils::GetString(json, "EditorVersion");
		info.StartedAt = JsonUtils::GetString(json, "StartedAt");
		info.Headless = JsonUtils::GetBool(json, "Headless", false);
		return info;
	}

	////////////////////////////////////////////////////////////////////////////////
	// EditorSession
	////////////////////////////////////////////////////////////////////////////////

	EditorSessionInfo EditorSession::DescribeCurrentProcess()
	{
		EditorSessionInfo info;
		info.ProcessId = Platform::GetProcessID();
		if (const std::optional<uint64_t> startTime = Platform::GetProcessStartTime(info.ProcessId))
			info.ProcessStartTime = *startTime;
		else
			ST_CORE_WARN("EditorSession: the start time of this process (id {}) is unavailable; clients cannot verify its session and will ignore it", info.ProcessId);
		info.EditorVersion = c_EngineVersion;
		info.StartedAt = GetCurrentTimestamp();
		return info;
	}

	std::optional<std::filesystem::path> EditorSession::GetSessionDirectory(std::string* error)
	{
		std::filesystem::path directory;
		if (const std::optional<std::string> overridden = Platform::GetEnvVar(c_SessionDirectoryVariable); overridden && !overridden->empty())
		{
			directory = FileSystem::FromUTF8(*overridden);
		}
		else if (const std::optional<std::filesystem::path> userData = Platform::FindUserDataDirectory("Strata"))
		{
			directory = *userData / "Sessions";
		}
		else
		{
			// Session files hold authentication tokens, so a shared location such as the temp directory is never used.
			if (error)
				*error = fmt::format("No per-user data directory is available for editor sessions (set {} to a private directory)", c_SessionDirectoryVariable);
			return std::nullopt;
		}

		std::string problem;
		if (!Platform::EnsurePrivateDirectory(directory, &problem))
		{
			if (error)
				*error = fmt::format("The editor session directory cannot be used: {}", problem);
			return std::nullopt;
		}
		return directory;
	}

	std::filesystem::path EditorSession::GetSessionFilePath(const std::filesystem::path& sessionDirectory, uint32_t processId)
	{
		return sessionDirectory / FileSystem::FromUTF8(fmt::format("{}.json", processId));
	}

	std::filesystem::path EditorSession::GetProjectSessionFilePath(const std::filesystem::path& projectDirectory)
	{
		return projectDirectory / c_ProjectDataDirectory / c_ProjectSessionFileName;
	}

	bool EditorSession::WriteSessionFiles(const EditorSessionInfo& info, std::string* error)
	{
		std::string problem;
		const std::optional<std::filesystem::path> sessionDirectory = GetSessionDirectory(&problem);
		if (!sessionDirectory)
		{
			ST_CORE_ERROR("EditorSession: {}", problem);
			if (error)
				*error = problem;
			return false;
		}

		const std::filesystem::path sessionFile = GetSessionFilePath(*sessionDirectory, info.ProcessId);
		const std::string session = info.ToJson().dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
		if (!Platform::WritePrivateFile(sessionFile, session, &problem))
		{
			ST_CORE_ERROR("EditorSession: {}", problem);
			if (error)
				*error = problem;
			return false;
		}

		if (!info.ProjectPath.empty())
		{
			const std::filesystem::path pointerFile = GetProjectSessionFilePath(FileSystem::FromUTF8(info.ProjectPath));
			const std::string pointer = MakeProjectPointer(info).dump(1, '\t', false, nlohmann::json::error_handler_t::replace) + "\n";
			if (!Platform::WritePrivateFile(pointerFile, pointer, &problem))
			{
				ST_CORE_ERROR("EditorSession: {}", problem);
				if (error)
					*error = problem;
				return false;
			}
		}
		return true;
	}

	void EditorSession::RemoveSessionFiles(const EditorSessionInfo& info)
	{
		std::error_code error;
		if (const std::optional<std::filesystem::path> sessionDirectory = GetSessionDirectory())
			std::filesystem::remove(GetSessionFilePath(*sessionDirectory, info.ProcessId), error);

		if (info.ProjectPath.empty())
			return;

		// Another editor may write its own pointer at any moment, so checking and then deleting would race.
		// Instead, take the pointer out of place atomically, inspect it, and put it back unless it is ours; if a
		// newer pointer appeared in the meantime, the taken (older) one is dropped.
		const std::filesystem::path pointerFile = GetProjectSessionFilePath(FileSystem::FromUTF8(info.ProjectPath));
		std::filesystem::path takenFile = pointerFile;
		takenFile += FileSystem::FromUTF8(fmt::format(".removing-{}", info.ProcessId));
		std::filesystem::rename(pointerFile, takenFile, error);
		if (error)
			return;

		const std::optional<nlohmann::json> pointer = ReadJsonFile(takenFile);
		if (pointer && IsPointerTo(*pointer, info))
		{
			std::filesystem::remove(takenFile, error);
			return;
		}
		if (!Platform::RenameNoReplace(takenFile, pointerFile))
			std::filesystem::remove(takenFile, error);
	}

	std::vector<EditorSessionInfo> EditorSession::FindSessions()
	{
		std::string error;
		const std::optional<std::filesystem::path> sessionDirectory = GetSessionDirectory(&error);
		if (!sessionDirectory)
		{
			ST_CORE_WARN("EditorSession: {}", error);
			return {};
		}
		return FindSessions(*sessionDirectory);
	}

	std::vector<EditorSessionInfo> EditorSession::FindSessions(const std::filesystem::path& sessionDirectory)
	{
		std::string problem;
		if (!Platform::EnsurePrivateDirectory(sessionDirectory, &problem))
		{
			ST_CORE_WARN("EditorSession: ignoring the session directory: {}", problem);
			return {};
		}

		std::vector<EditorSessionInfo> sessions;
		std::error_code error;
		for (std::filesystem::directory_iterator it(sessionDirectory, error); !error && it != std::filesystem::directory_iterator(); it.increment(error))
		{
			if (it->path().extension() != ".json")
				continue;

			std::optional<EditorSessionInfo> session = ReadSessionFile(it->path());
			if (!session || FileSystem::ToUTF8(it->path().stem()) != std::to_string(session->ProcessId))
				continue;

			const SessionProcessState state = GetSessionProcessState(*session);
			if (state == SessionProcessState::Exited)
			{
				// The editor exited without cleaning up (e.g. it crashed).
				if (RemoveStaleSessionFile(it->path()))
					ST_CORE_INFO("EditorSession: removed the stale session of process {}", session->ProcessId);
				continue;
			}
			if (state == SessionProcessState::Unverifiable)
			{
				// Possibly a live editor (e.g. one whose start time could not be read): never deleted, never used.
				ST_CORE_INFO("EditorSession: skipping the session of process {}: its process cannot be verified", session->ProcessId);
				continue;
			}
			sessions.push_back(std::move(*session));
		}

		// ISO-8601 UTC timestamps of a fixed format order lexicographically; the process id breaks ties.
		std::sort(sessions.begin(), sessions.end(), [](const EditorSessionInfo& left, const EditorSessionInfo& right)
		{
			if (left.StartedAt != right.StartedAt)
				return left.StartedAt > right.StartedAt;
			return left.ProcessId > right.ProcessId;
		});
		return sessions;
	}

	bool EditorSession::RemoveStaleSessionFile(const std::filesystem::path& path)
	{
		// The file was judged stale from contents read earlier, and a new editor that reuses the process id may have
		// written its own session at the same path since. So the file is taken out of place atomically and judged
		// again: only a file that is still stale is deleted; anything else goes back (unless an even newer session
		// has taken the path in the meantime, which then wins). The taken name does not end in ".json", so even if
		// this process dies in between, no client ever reads it as a session.
		std::array<uint8_t, 8> suffix = {};
		if (!Platform::GenerateSecureRandom(suffix))
			return false;
		std::filesystem::path takenFile = path;
		takenFile += FileSystem::FromUTF8(".stale-" + Crypto::ToHex(suffix));

		std::error_code error;
		std::filesystem::rename(path, takenFile, error);
		if (error)
			return false; // Already removed (e.g. by another client), or held open by a reader

		const std::optional<EditorSessionInfo> session = ReadSessionFile(takenFile);
		if (!session || GetSessionProcessState(*session) != SessionProcessState::Exited)
		{
			if (!Platform::RenameNoReplace(takenFile, path))
				std::filesystem::remove(takenFile, error);
			return false;
		}
		std::filesystem::remove(takenFile, error);
		return !error;
	}

	std::optional<EditorSessionInfo> EditorSession::ReadSessionFile(const std::filesystem::path& path)
	{
		if (!FileSystem::Exists(path))
			return std::nullopt;

		// The trust checks and the read use one handle, so the file cannot be swapped in between.
		std::string problem;
		const std::optional<std::string> text = Platform::ReadTrustedFile(path, c_MaxSessionFileSize, &problem);
		if (!text)
		{
			ST_CORE_WARN("EditorSession: ignoring an untrusted or unreadable session file: {}", problem);
			return std::nullopt;
		}

		const std::optional<nlohmann::json> json = JsonRpc::Parse(*text);
		if (!json)
		{
			ST_CORE_TRACE("EditorSession: '{}' is not a valid session file", FileSystem::ToUTF8(path));
			return std::nullopt;
		}
		return EditorSessionInfo::FromJson(*json);
	}

	std::optional<EditorSessionInfo> EditorSession::ReadProjectSession(const std::filesystem::path& projectDirectory)
	{
		const std::optional<std::filesystem::path> sessionDirectory = GetSessionDirectory();
		if (!sessionDirectory)
			return std::nullopt;
		return ReadProjectSession(projectDirectory, *sessionDirectory);
	}

	std::optional<EditorSessionInfo> EditorSession::ReadProjectSession(const std::filesystem::path& projectDirectory, const std::filesystem::path& sessionDirectory)
	{
		const std::optional<nlohmann::json> pointer = ReadJsonFile(GetProjectSessionFilePath(projectDirectory));
		if (!pointer)
			return std::nullopt;
		const std::optional<uint32_t> processId = GetProcessId(*pointer);
		if (!processId)
			return std::nullopt;

		// Everything the connection needs comes from the per-user file; the pointer only chose which one.
		std::optional<EditorSessionInfo> session = ReadSessionFile(GetSessionFilePath(sessionDirectory, *processId));
		if (!session || session->ProcessId != *processId || !IsSameProject(session->ProjectPath, projectDirectory))
			return std::nullopt;
		if (!IsSessionProcessRunning(*session))
			return std::nullopt;
		return session;
	}

	SessionProcessState EditorSession::GetSessionProcessState(const EditorSessionInfo& session)
	{
		if (!Platform::IsProcessAlive(session.ProcessId))
			return SessionProcessState::Exited;

		// The start time tells the process that wrote the session apart from a later one that reuses its id. Only a
		// known start time that differs proves the id was reused; an unknown one proves nothing either way.
		const std::optional<uint64_t> startTime = Platform::GetProcessStartTime(session.ProcessId);
		if (!startTime || session.ProcessStartTime == 0)
			return SessionProcessState::Unverifiable;
		return *startTime == session.ProcessStartTime ? SessionProcessState::Running : SessionProcessState::Exited;
	}

	bool EditorSession::IsSessionProcessRunning(const EditorSessionInfo& session)
	{
		return GetSessionProcessState(session) == SessionProcessState::Running;
	}

	bool EditorSession::IsSameProject(const std::string& sessionProjectPath, const std::filesystem::path& projectDirectory)
	{
		if (sessionProjectPath.empty() || projectDirectory.empty())
			return false;

		const std::filesystem::path sessionPath = FileSystem::FromUTF8(sessionProjectPath);
		std::error_code error;
		if (std::filesystem::equivalent(sessionPath, projectDirectory, error) && !error)
			return true;

		// Fall back to a lexical comparison (e.g. a directory that no longer exists).
		return NormalizePath(sessionPath) == NormalizePath(projectDirectory);
	}

	std::string EditorSession::GenerateSessionToken()
	{
		std::array<uint8_t, 16> bytes = {};
		if (!Platform::GenerateSecureRandom(bytes))
		{
			ST_CORE_ERROR("EditorSession: the system random number generator failed");
			return {};
		}

		std::string token;
		token.reserve(bytes.size() * 2);
		for (const uint8_t byte : bytes)
			token += fmt::format("{:02x}", byte);
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
