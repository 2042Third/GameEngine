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

	// Session files:
	//   <session directory>/<process id>.json   one per running editor
	//   <project>/.strata/EditorSession.json    when the editor has a project open
	// The session directory is <user data directory>/Strata/Sessions (see Platform::GetUserDataDirectory),
	// unless the STRATA_SESSION_DIR environment variable names another directory (used by tests to isolate
	// themselves from real editor sessions). Files are written atomically and readable only by tools that can
	// read the user's files, which is what protects the token.
	class EditorSession
	{
	public:
		static std::filesystem::path GetSessionDirectory();
		static std::filesystem::path GetSessionFilePath(uint32_t processId);
		static std::filesystem::path GetSessionFilePath(const std::filesystem::path& sessionDirectory, uint32_t processId);
		static std::filesystem::path GetProjectSessionFilePath(const std::filesystem::path& projectDirectory);

		// Writes the per-process file and, when ProjectPath is set, the project file.
		static bool WriteSessionFiles(const EditorSessionInfo& info);
		// Removes the per-process file, and the project file if it still belongs to info's process.
		static void RemoveSessionFiles(const EditorSessionInfo& info);

		// Every readable session in the directory, newest (StartedAt) first. Stale files of editors that exited
		// without cleaning up are included; callers verify sessions by connecting.
		static std::vector<EditorSessionInfo> FindSessions();
		static std::vector<EditorSessionInfo> FindSessions(const std::filesystem::path& sessionDirectory);
		static std::optional<EditorSessionInfo> ReadSessionFile(const std::filesystem::path& path);
		static std::optional<EditorSessionInfo> ReadProjectSession(const std::filesystem::path& projectDirectory);

		// 32 random lower-case hexadecimal characters (128 bits from std::random_device).
		static std::string GenerateSessionToken();
		// The current UTC time in ISO-8601 form ("YYYY-MM-DDTHH:MM:SSZ").
		static std::string GetCurrentTimestamp();
	};

}
