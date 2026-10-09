#pragma once

#include "Strata/Core/Base.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	// Describes a running editor's automation endpoint. The editor publishes it in session files so that
	// tools (StrataCLI, the MCP server) can find and authenticate to it.
	struct EditorSessionInfo
	{
		uint32_t ProcessId = 0;
		uint16_t Port = 0;
		std::string Token;         // Secret required by rpc.authenticate
		std::string ProjectPath;   // UTF-8; empty when no project is open
		std::string EditorVersion;
		bool Headless = false;
		std::string StartedAt;     // ISO-8601 UTC, e.g. "2026-01-31T12:00:00Z"

		// Keys: "ProcessId", "Port", "Token", "ProjectPath", "EditorVersion", "Headless", "StartedAt".
		nlohmann::json ToJson() const;
		// Returns nullopt unless json is an object with a valid ProcessId and Port (other keys are optional).
		static std::optional<EditorSessionInfo> FromJson(const nlohmann::json& json);
	};

	// Session files.
	//
	//   <session directory>/<process id>.json  The full session, including the token. Written owner-only (POSIX mode
	//                                          0600, owner-only DACL on Windows) into a private directory: per-user
	//                                          data/Strata/Sessions (see Platform::FindUserDataDirectory), or the
	//                                          STRATA_SESSION_DIR environment variable (used by tests to isolate
	//                                          themselves). There is no fallback to a shared location; on POSIX the
	//                                          directory must be owned by the current user and not writable by others.
	//   <project>/.strata/EditorSession.json   A pointer for tools started in a project: ProcessId, ProjectPath,
	//                                          EditorVersion, Headless and StartedAt only, never the port or token.
	//                                          Project directories may be shared, so the pointer is untrusted: it only
	//                                          names a process whose per-user session file must exist, be trusted,
	//                                          belong to a running process and name the same project.
	//
	// Session files of processes that are no longer running are deleted by FindSessions.
	class EditorSession
	{
	public:
		// The private session directory (created if missing), or nullopt with the reason in error.
		static std::optional<std::filesystem::path> GetSessionDirectory(std::string* error = nullptr);
		static std::filesystem::path GetSessionFilePath(const std::filesystem::path& sessionDirectory, uint32_t processId);
		static std::filesystem::path GetProjectSessionFilePath(const std::filesystem::path& projectDirectory);

		// Writes the per-process file and, when ProjectPath is set, the project pointer.
		static bool WriteSessionFiles(const EditorSessionInfo& info, std::string* error = nullptr);
		// Removes the per-process file, and the project pointer if it still refers to info's process (another
		// editor may have opened the project since; its pointer is left alone).
		static void RemoveSessionFiles(const EditorSessionInfo& info);

		// Sessions of running editors in the directory, newest (StartedAt) first. Files of processes that have
		// exited are deleted; untrusted files are ignored. Callers still verify sessions by connecting.
		static std::vector<EditorSessionInfo> FindSessions();
		static std::vector<EditorSessionInfo> FindSessions(const std::filesystem::path& sessionDirectory);
		// Reads a per-user session file (nullopt if it is missing, untrusted or invalid).
		static std::optional<EditorSessionInfo> ReadSessionFile(const std::filesystem::path& path);

		// The session of the running editor that has projectDirectory open, located through the project pointer
		// and validated as described above.
		static std::optional<EditorSessionInfo> ReadProjectSession(const std::filesystem::path& projectDirectory);
		static std::optional<EditorSessionInfo> ReadProjectSession(const std::filesystem::path& projectDirectory, const std::filesystem::path& sessionDirectory);

		// Whether a session's ProjectPath (UTF-8) refers to projectDirectory.
		static bool IsSameProject(const std::string& sessionProjectPath, const std::filesystem::path& projectDirectory);

		// 32 lower-case hexadecimal characters (128 bits from the system's secure random generator), or an empty
		// string if the generator fails (which RpcServer::Start then refuses).
		static std::string GenerateSessionToken();
		// The current UTC time in ISO-8601 form ("YYYY-MM-DDTHH:MM:SSZ").
		static std::string GetCurrentTimestamp();
	};

}
