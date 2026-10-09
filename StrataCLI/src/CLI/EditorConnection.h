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

	// Default host of an explicit endpoint. Discovered sessions are reached at the loopback address they record.
	constexpr const char* c_EditorSessionHost = "127.0.0.1";

	struct EditorConnectionOptions
	{
		std::string Host = c_EditorSessionHost; // Host of the explicit endpoint (Port); discovery uses each session's address
		// Explicit endpoint (--port/--token, or STRATA_EDITOR_PORT/STRATA_EDITOR_TOKEN). Disables session discovery.
		std::optional<uint16_t> Port;
		std::string Token;
		// Prefer the editor that has this project open: its .strata/EditorSession.json pointer, then sessions whose
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
	// project's session and the sessions of that project, or (without a project) every running editor's session,
	// newest first. error (if given) receives why no session directory could be used.
	std::vector<EditorEndpoint> DiscoverEditorEndpoints(const EditorConnectionOptions& options, std::string* error = nullptr);

	// Session information safe to show to users and AI agents (the token is omitted).
	nlohmann::json DescribeSession(const EditorSessionInfo& session);

	// The editor a connection stays with once it has connected.
	struct PinnedEditor
	{
		std::string ProjectPath;       // Any running editor with this project is accepted (e.g. after an editor restart)
		uint32_t ProcessId = 0;        // The process last connected to, which is always accepted (whatever project it has
		uint64_t ProcessStartTime = 0; // open now); its start time tells it apart from a process that reuses its id
	};

	// Client for a running editor that connects lazily and reconnects when the connection drops.
	//
	// After the first successful connection through a session, the connection is pinned to that editor: later
	// reconnects accept the same process (even after it opened another project, which the pin then follows) or
	// another process with the same project (so an editor restart is followed). A different editor never silently takes
	// its place (its tools and state would differ); calls fail with a "disconnected" error instead until
	// ConnectToSession picks an editor.
	class EditorConnection
	{
	public:
		explicit EditorConnection(EditorConnectionOptions options = {});

		// Returns true if a live connection exists or could be established through discovery.
		bool EnsureConnected();
		bool IsConnected() const;
		// Connects to a known session (e.g. one that was just launched) and pins the connection to it.
		bool ConnectToSession(const EditorSessionInfo& session);
		void Disconnect();

		// Calls an editor method, connecting first if needed. Fails with ConnectionClosed if no editor is reachable.
		RpcResult Call(const std::string& method, const nlohmann::json& params, std::chrono::milliseconds timeout);

		// The endpoint/session of the current connection (empty when disconnected).
		const std::optional<EditorEndpoint>& GetEndpoint() const { return m_Endpoint; }
		const std::optional<PinnedEditor>& GetPinnedEditor() const { return m_Pinned; }
		const std::string& GetLastError() const { return m_LastError; }
		const EditorConnectionOptions& GetOptions() const { return m_Options; }
		std::optional<std::filesystem::path> GetSessionDirectory(std::string* error = nullptr) const;

		// {"connected", "endpoint", "session", "pinnedEditor", "error", ...}; the token is never included.
		nlohmann::json DescribeStatus() const;
	private:
		bool TryEndpoint(const EditorEndpoint& endpoint);
		void AddPinnedProcessEndpoint(std::vector<EditorEndpoint>& endpoints) const;
		bool IsPinnedEditor(const EditorEndpoint& endpoint) const;
		std::string DescribePinnedEditorMissing() const;
	private:
		EditorConnectionOptions m_Options;
		RpcClient m_Client;
		std::optional<EditorEndpoint> m_Endpoint;
		std::optional<PinnedEditor> m_Pinned;
		std::string m_LastError;
	};

}
