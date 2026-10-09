#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Network/EditorSession.h"
#include "Strata/Network/RpcClient.h"
#include "Strata/Network/RpcServer.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Strata::CLI
{

	struct EditorConnectionOptions
	{
		std::string Host = "127.0.0.1";
		// Explicit endpoint (--port/--token, or STRATA_EDITOR_PORT/STRATA_EDITOR_TOKEN). Disables session discovery.
		std::optional<uint16_t> Port;
		std::string Token;
		// Prefer the editor that has this project open: its .strata/EditorSession.json, then sessions whose
		// ProjectPath is this directory. Editors with other projects are never chosen.
		std::filesystem::path ProjectDirectory;
		// Directory of per-process session files; empty uses EditorSession::GetSessionDirectory().
		std::filesystem::path SessionDirectory;
		std::chrono::milliseconds ConnectTimeout = std::chrono::milliseconds(2000);
	};

	// A connection candidate found by discovery.
	struct EditorEndpoint
	{
		std::string Host;
		uint16_t Port = 0;
		std::string Token;
		std::optional<EditorSessionInfo> Session; // Absent for explicit endpoints
		std::string Source;                       // "explicit", "project" or "session"
	};

	// Connection candidates in discovery order: the explicit endpoint if one is configured; otherwise the
	// project's session file and the sessions of that project, or (without a project) every session, newest first.
	std::vector<EditorEndpoint> DiscoverEditorEndpoints(const EditorConnectionOptions& options);

	// Whether a session's ProjectPath (UTF-8) refers to projectDirectory.
	bool IsSameProject(const std::string& sessionProjectPath, const std::filesystem::path& projectDirectory);

	// Session information safe to show to users and AI agents (the token is omitted).
	nlohmann::json DescribeSession(const EditorSessionInfo& session);

	// Client for a running editor that connects lazily and reconnects (rediscovering the editor) when the
	// connection drops, e.g. after the editor restarted on a different port.
	class EditorConnection
	{
	public:
		explicit EditorConnection(EditorConnectionOptions options = {});

		// Returns true if a live connection exists or could be established through discovery.
		bool EnsureConnected();
		bool IsConnected() const;
		// Connects to a known session (e.g. one that was just launched) and makes its project the preferred one.
		bool ConnectToSession(const EditorSessionInfo& session);
		void Disconnect();

		// Calls an editor method, connecting first if needed. Fails with ConnectionClosed if no editor is reachable.
		RpcResult Call(const std::string& method, const nlohmann::json& params, std::chrono::milliseconds timeout);

		// The endpoint/session of the current connection (empty when disconnected).
		const std::optional<EditorEndpoint>& GetEndpoint() const { return m_Endpoint; }
		const std::string& GetLastError() const { return m_LastError; }
		const EditorConnectionOptions& GetOptions() const { return m_Options; }
		std::filesystem::path GetSessionDirectory() const;

		// {"connected", "endpoint", "session", "error"}; the token is never included.
		nlohmann::json DescribeStatus() const;
	private:
		bool TryEndpoint(const EditorEndpoint& endpoint);
	private:
		EditorConnectionOptions m_Options;
		RpcClient m_Client;
		std::optional<EditorEndpoint> m_Endpoint;
		std::string m_LastError;
	};

}
