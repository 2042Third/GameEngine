#include "CLI/EditorConnection.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Network/Socket.h"

#include <algorithm>

namespace Strata::CLI
{

	namespace
	{

		bool IsDuplicate(const std::vector<EditorEndpoint>& endpoints, const EditorSessionInfo& session)
		{
			return std::any_of(endpoints.begin(), endpoints.end(), [&](const EditorEndpoint& endpoint)
			{
				return endpoint.Session && endpoint.Session->ProcessId == session.ProcessId && endpoint.Port == session.Port;
			});
		}

		EditorEndpoint MakeSessionEndpoint(const EditorSessionInfo& session, const char* source)
		{
			EditorEndpoint endpoint;
			endpoint.Host = session.Address;
			endpoint.Port = session.Port;
			endpoint.Token = session.Token;
			endpoint.Session = session;
			endpoint.Source = source;
			return endpoint;
		}

		// Editors only listen on loopback; a session naming any other address is not followed.
		void AddSessionEndpoint(std::vector<EditorEndpoint>& endpoints, const EditorSessionInfo& session, const char* source)
		{
			if (!IsLoopbackAddress(session.Address))
			{
				ST_WARN("Ignoring the session of process {}: '{}' is not a loopback address", session.ProcessId, session.Address);
				return;
			}
			endpoints.push_back(MakeSessionEndpoint(session, source));
		}

	}

	std::vector<EditorEndpoint> DiscoverEditorEndpoints(const EditorConnectionOptions& options, std::string* error)
	{
		std::vector<EditorEndpoint> endpoints;
		if (options.Port)
		{
			EditorEndpoint& endpoint = endpoints.emplace_back();
			endpoint.Host = options.Host;
			endpoint.Port = *options.Port;
			endpoint.Token = options.Token;
			endpoint.Source = "explicit";
			return endpoints;
		}

		std::filesystem::path sessionDirectory = options.SessionDirectory;
		if (sessionDirectory.empty())
		{
			std::optional<std::filesystem::path> defaultDirectory = EditorSession::GetSessionDirectory(error);
			if (!defaultDirectory)
				return endpoints;
			sessionDirectory = std::move(*defaultDirectory);
		}

		const std::vector<EditorSessionInfo> sessions = EditorSession::FindSessions(sessionDirectory);
		if (!options.ProjectDirectory.empty())
		{
			if (std::optional<EditorSessionInfo> projectSession = EditorSession::ReadProjectSession(options.ProjectDirectory, sessionDirectory))
				AddSessionEndpoint(endpoints, *projectSession, "project");

			for (const EditorSessionInfo& session : sessions)
			{
				if (EditorSession::IsSameProject(session.ProjectPath, options.ProjectDirectory) && !IsDuplicate(endpoints, session))
					AddSessionEndpoint(endpoints, session, "session");
			}
			return endpoints;
		}

		for (const EditorSessionInfo& session : sessions)
			AddSessionEndpoint(endpoints, session, "session");
		return endpoints;
	}

	nlohmann::json DescribeSession(const EditorSessionInfo& session)
	{
		nlohmann::json description = session.ToJson();
		description.erase("Token");
		return description;
	}

	////////////////////////////////////////////////////////////////////////////////
	// EditorConnection
	////////////////////////////////////////////////////////////////////////////////

	EditorConnection::EditorConnection(EditorConnectionOptions options)
		: m_Options(std::move(options))
	{
	}

	bool EditorConnection::EnsureConnected()
	{
		if (m_Client.CheckConnection())
			return true;

		if (m_Endpoint)
			ST_INFO("Lost the connection to the editor on port {}", m_Endpoint->Port);
		m_Endpoint.reset();

		std::string discoveryError;
		std::vector<EditorEndpoint> endpoints = DiscoverEditorEndpoints(m_Options, &discoveryError);
		std::erase_if(endpoints, [this](const EditorEndpoint& endpoint) { return !IsPinnedEditor(endpoint); });
		if (endpoints.empty())
		{
			if (m_Pinned)
				m_LastError = DescribePinnedEditorMissing();
			else if (!discoveryError.empty())
				m_LastError = discoveryError;
			else if (!m_Options.ProjectDirectory.empty())
				m_LastError = fmt::format("No running Strata editor has the project '{}' open", FileSystem::ToUTF8(m_Options.ProjectDirectory));
			else
				m_LastError = "No running Strata editor was found";
			return false;
		}

		std::vector<std::string> failures;
		for (const EditorEndpoint& endpoint : endpoints)
		{
			if (TryEndpoint(endpoint))
				return true;
			failures.push_back(fmt::format("port {} ({}): {}", endpoint.Port, endpoint.Source, m_Client.GetLastError()));
		}

		m_LastError = m_Pinned ? DescribePinnedEditorMissing() : "No Strata editor is reachable";
		for (const std::string& failure : failures)
			m_LastError += "\n  " + failure;
		return false;
	}

	bool EditorConnection::IsConnected() const
	{
		return m_Client.IsConnected();
	}

	bool EditorConnection::ConnectToSession(const EditorSessionInfo& session)
	{
		Disconnect();

		// An explicit choice: forget the previous editor, and prefer this one when reconnecting later.
		m_Pinned.reset();
		m_Options.Port.reset();
		m_Options.Token.clear();
		if (!session.ProjectPath.empty())
			m_Options.ProjectDirectory = FileSystem::FromUTF8(session.ProjectPath);

		if (!IsLoopbackAddress(session.Address))
		{
			m_LastError = fmt::format("The session's address '{}' is not a loopback address", session.Address);
			return false;
		}
		if (TryEndpoint(MakeSessionEndpoint(session, "session")))
			return true;
		m_LastError = m_Client.GetLastError();
		return false;
	}

	void EditorConnection::Disconnect()
	{
		m_Client.Close();
		m_Endpoint.reset();
	}

	RpcResult EditorConnection::Call(const std::string& method, const nlohmann::json& params, std::chrono::milliseconds timeout)
	{
		if (!EnsureConnected())
			return RpcResult::Failure(JsonRpc::ErrorCode::ConnectionClosed, m_LastError);

		RpcResult result = m_Client.Call(method, params, timeout);
		if (!m_Client.IsConnected())
		{
			m_LastError = m_Client.GetLastError();
			m_Endpoint.reset();
		}
		return result;
	}

	std::optional<std::filesystem::path> EditorConnection::GetSessionDirectory(std::string* error) const
	{
		if (!m_Options.SessionDirectory.empty())
			return m_Options.SessionDirectory;
		return EditorSession::GetSessionDirectory(error);
	}

	nlohmann::json EditorConnection::DescribeStatus() const
	{
		nlohmann::json status = nlohmann::json::object();
		const bool connected = IsConnected() && m_Endpoint.has_value();
		status["connected"] = connected;
		if (connected)
		{
			status["endpoint"] = nlohmann::json { { "host", m_Endpoint->Host }, { "port", m_Endpoint->Port }, { "source", m_Endpoint->Source } };
			status["session"] = m_Endpoint->Session ? DescribeSession(*m_Endpoint->Session) : nlohmann::json();
		}
		else
		{
			status["error"] = m_LastError;
		}
		if (m_Pinned)
			status["pinnedEditor"] = nlohmann::json { { "ProjectPath", m_Pinned->ProjectPath }, { "ProcessId", m_Pinned->ProcessId } };

		std::string directoryError;
		if (const std::optional<std::filesystem::path> sessionDirectory = GetSessionDirectory(&directoryError))
			status["sessionDirectory"] = FileSystem::ToUTF8(*sessionDirectory);
		else
			status["sessionDirectoryError"] = directoryError;
		if (!m_Options.ProjectDirectory.empty())
			status["project"] = FileSystem::ToUTF8(m_Options.ProjectDirectory);
		return status;
	}

	bool EditorConnection::TryEndpoint(const EditorEndpoint& endpoint)
	{
		if (!m_Client.Connect(endpoint.Host, endpoint.Port, endpoint.Token, m_Options.ConnectTimeout))
		{
			ST_TRACE("Editor endpoint {}:{} ({}) is not reachable: {}", endpoint.Host, endpoint.Port, endpoint.Source, m_Client.GetLastError());
			return false;
		}

		m_Endpoint = endpoint;
		m_LastError.clear();
		if (endpoint.Session)
		{
			if (!m_Pinned)
				m_Pinned = PinnedEditor { endpoint.Session->ProjectPath, endpoint.Session->ProcessId };
			else
				m_Pinned->ProcessId = endpoint.Session->ProcessId; // Followed a restart of the same project
		}
		ST_INFO("Connected to the editor on {}:{} ({})", endpoint.Host, endpoint.Port, endpoint.Source);
		return true;
	}

	bool EditorConnection::IsPinnedEditor(const EditorEndpoint& endpoint) const
	{
		// Explicit endpoints are fixed by configuration and need no pinning.
		if (!m_Pinned || !endpoint.Session)
			return true;
		if (!m_Pinned->ProjectPath.empty())
			return EditorSession::IsSameProject(endpoint.Session->ProjectPath, FileSystem::FromUTF8(m_Pinned->ProjectPath));
		return endpoint.Session->ProcessId == m_Pinned->ProcessId;
	}

	std::string EditorConnection::DescribePinnedEditorMissing() const
	{
		if (!m_Pinned->ProjectPath.empty())
		{
			return fmt::format("Disconnected: the editor for project '{}' (last seen as process {}) is not running. Other editors are not "
				"used automatically; start that project's editor again (e.g. strata_launch_editor).", m_Pinned->ProjectPath, m_Pinned->ProcessId);
		}
		return fmt::format("Disconnected: the editor process {} (no project) is not running. Other editors are not used "
			"automatically; launch or choose an editor explicitly (e.g. strata_launch_editor).", m_Pinned->ProcessId);
	}

}
