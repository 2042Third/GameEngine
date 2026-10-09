#include "CLI/EditorConnection.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"

#include <algorithm>

namespace Strata::CLI
{

	namespace
	{

		std::filesystem::path NormalizePath(const std::filesystem::path& path)
		{
			std::error_code error;
			std::filesystem::path absolute = std::filesystem::absolute(path, error);
			if (error)
				absolute = path;
			return absolute.lexically_normal();
		}

		bool IsDuplicate(const std::vector<EditorEndpoint>& endpoints, const EditorSessionInfo& session)
		{
			return std::any_of(endpoints.begin(), endpoints.end(), [&](const EditorEndpoint& endpoint)
			{
				return endpoint.Session && endpoint.Session->ProcessId == session.ProcessId && endpoint.Port == session.Port;
			});
		}

		EditorEndpoint MakeSessionEndpoint(const EditorConnectionOptions& options, const EditorSessionInfo& session, const char* source)
		{
			EditorEndpoint endpoint;
			endpoint.Host = options.Host;
			endpoint.Port = session.Port;
			endpoint.Token = session.Token;
			endpoint.Session = session;
			endpoint.Source = source;
			return endpoint;
		}

	}

	std::vector<EditorEndpoint> DiscoverEditorEndpoints(const EditorConnectionOptions& options)
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

		const std::filesystem::path sessionDirectory = options.SessionDirectory.empty() ? EditorSession::GetSessionDirectory() : options.SessionDirectory;
		const std::vector<EditorSessionInfo> sessions = EditorSession::FindSessions(sessionDirectory);
		if (!options.ProjectDirectory.empty())
		{
			if (std::optional<EditorSessionInfo> projectSession = EditorSession::ReadProjectSession(options.ProjectDirectory))
				endpoints.push_back(MakeSessionEndpoint(options, *projectSession, "project"));

			for (const EditorSessionInfo& session : sessions)
			{
				if (IsSameProject(session.ProjectPath, options.ProjectDirectory) && !IsDuplicate(endpoints, session))
					endpoints.push_back(MakeSessionEndpoint(options, session, "session"));
			}
			return endpoints;
		}

		for (const EditorSessionInfo& session : sessions)
			endpoints.push_back(MakeSessionEndpoint(options, session, "session"));
		return endpoints;
	}

	bool IsSameProject(const std::string& sessionProjectPath, const std::filesystem::path& projectDirectory)
	{
		if (sessionProjectPath.empty() || projectDirectory.empty())
			return false;

		const std::filesystem::path sessionPath = FileSystem::FromUTF8(sessionProjectPath);
		std::error_code error;
		if (std::filesystem::equivalent(sessionPath, projectDirectory, error) && !error)
			return true;

		// Fall back to a lexical comparison (e.g. a directory that no longer exists), ignoring a trailing separator.
		std::filesystem::path left = NormalizePath(sessionPath);
		std::filesystem::path right = NormalizePath(projectDirectory);
		if (!left.has_filename())
			left = left.parent_path();
		if (!right.has_filename())
			right = right.parent_path();
		return left == right;
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

		const std::vector<EditorEndpoint> endpoints = DiscoverEditorEndpoints(m_Options);
		if (endpoints.empty())
		{
			if (!m_Options.ProjectDirectory.empty())
				m_LastError = fmt::format("No Strata editor session found for project '{}'", FileSystem::ToUTF8(m_Options.ProjectDirectory));
			else
				m_LastError = fmt::format("No Strata editor session found in '{}'", FileSystem::ToUTF8(GetSessionDirectory()));
			return false;
		}

		std::vector<std::string> failures;
		for (const EditorEndpoint& endpoint : endpoints)
		{
			if (TryEndpoint(endpoint))
				return true;
			failures.push_back(fmt::format("port {} ({}): {}", endpoint.Port, endpoint.Source, m_Client.GetLastError()));
		}

		m_LastError = "No Strata editor is reachable";
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

		// Prefer this editor when reconnecting later; an explicit endpoint would otherwise take precedence.
		m_Options.Port.reset();
		m_Options.Token.clear();
		if (!session.ProjectPath.empty())
			m_Options.ProjectDirectory = FileSystem::FromUTF8(session.ProjectPath);

		if (TryEndpoint(MakeSessionEndpoint(m_Options, session, "session")))
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

	std::filesystem::path EditorConnection::GetSessionDirectory() const
	{
		return m_Options.SessionDirectory.empty() ? EditorSession::GetSessionDirectory() : m_Options.SessionDirectory;
	}

	nlohmann::json EditorConnection::DescribeStatus() const
	{
		nlohmann::json status = nlohmann::json::object();
		status["connected"] = IsConnected() && m_Endpoint.has_value();
		if (m_Endpoint && IsConnected())
		{
			status["endpoint"] = nlohmann::json { { "host", m_Endpoint->Host }, { "port", m_Endpoint->Port }, { "source", m_Endpoint->Source } };
			status["session"] = m_Endpoint->Session ? DescribeSession(*m_Endpoint->Session) : nlohmann::json();
		}
		else
		{
			status["error"] = m_LastError;
		}
		status["sessionDirectory"] = FileSystem::ToUTF8(GetSessionDirectory());
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
		ST_INFO("Connected to the editor on {}:{} ({})", endpoint.Host, endpoint.Port, endpoint.Source);
		return true;
	}

}
