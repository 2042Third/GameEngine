#include "Editor/EditorAutomation.h"

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorContext.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Network/JsonRpc.h>

namespace Strata
{

	namespace
	{

		constexpr const char* c_StatusSection = "automation";
		// Parameters can be large (component values, scripts): the log keeps their start.
		constexpr size_t c_MaxLoggedParametersSize = 256;

		std::string DescribeParameters(const nlohmann::json& parameters)
		{
			std::string text = JsonRpc::Serialize(parameters);
			if (text.size() > c_MaxLoggedParametersSize)
				text = fmt::format("{}... ({} bytes)", text.substr(0, c_MaxLoggedParametersSize), text.size());
			return text;
		}

		std::string DescribeOutcome(const RpcResult& result)
		{
			if (result.IsSuccess())
				return "ok";
			return fmt::format("error {}: {}", result.GetError().Code, result.GetError().Message);
		}

		// A request's identity in the log, and when it started (to report how long it took).
		struct RequestTrace
		{
			uint64_t Id = 0;
			uint64_t StartFrame = 0;
			std::chrono::steady_clock::time_point StartTime;
			std::string Parameters;
			bool Pending = false;
		};

		void LogCompletion(const RequestTrace& trace, const std::string& command, const RpcResult& result, uint64_t frame)
		{
			const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - trace.StartTime).count();
			if (trace.Pending)
			{
				ST_TRACE("Automation: #{} {} {} -> {} ({} frames, {:.1f} ms)", trace.Id, command, trace.Parameters, DescribeOutcome(result),
					frame - trace.StartFrame, milliseconds);
			}
			else
			{
				ST_TRACE("Automation: #{} {} {} -> {} ({:.1f} ms)", trace.Id, command, trace.Parameters, DescribeOutcome(result), milliseconds);
			}
		}

	}

	EditorAutomation::EditorAutomation(EditorContext& context, const EditorCommandRegistry& commands, EditorCommandRunner& runner)
		: m_Context(context), m_Commands(commands), m_Runner(runner), m_State(CreateRef<RequestState>())
	{
	}

	EditorAutomation::~EditorAutomation()
	{
		Stop();
	}

	bool EditorAutomation::Start(const EditorAutomationSpecification& specification, std::string* outError)
	{
		Stop();

		auto fail = [this, outError](std::string error)
		{
			if (m_Session)
			{
				EditorSession::RemoveSessionFiles(*m_Session);
				m_Session.reset();
			}
			m_Server.Stop();
			UnregisterMethods();
			if (outError)
				*outError = std::move(error);
			return false;
		};

		m_Specification = specification;
		RpcServerSpecification serverSpecification;
		serverSpecification.BindAddress = specification.BindAddress;
		serverSpecification.Port = specification.Port;
		serverSpecification.AuthToken = specification.AuthToken.empty() ? EditorSession::GenerateSessionToken() : specification.AuthToken;
		if (serverSpecification.AuthToken.empty())
			return fail("No session token could be generated: the system's secure random generator failed");

		// The methods exist before the first client can list them.
		SyncMethods();
		if (!m_Server.Start(serverSpecification))
			return fail(fmt::format("The automation server cannot listen on {} port {}: {}", specification.BindAddress, specification.Port, m_Server.GetLastError()));

		if (specification.PublishSession)
		{
			EditorSessionInfo session = EditorSession::DescribeCurrentProcess();
			session.Address = specification.BindAddress;
			session.Port = m_Server.GetPort();
			session.Token = serverSpecification.AuthToken;
			session.ProjectPath = GetProjectPath();
			session.Headless = specification.Headless;
			// Set first, so a partly written session is removed by fail().
			m_Session = session;
			std::string error;
			if (!EditorSession::WriteSessionFiles(session, &error))
				return fail(fmt::format("The automation session cannot be published, so no client could find this editor: {}", error));
		}

		m_Context.SetStatusProvider(c_StatusSection, [this]() { return DescribeStatus(); });
		m_State->RefreshSession = [this]() { UpdateSession(); };
		ST_INFO("Automation: listening on {}:{}{}", specification.BindAddress, m_Server.GetPort(),
			m_Session ? " (StrataCLI and its MCP server find this editor through its session file)" : "");
		return true;
	}

	void EditorAutomation::Stop()
	{
		if (!m_Server.IsRunning())
			return;

		m_State->RefreshSession = nullptr;
		m_Context.SetStatusProvider(c_StatusSection, nullptr);
		// No new client should find an editor that is going away.
		if (m_Session)
		{
			EditorSession::RemoveSessionFiles(*m_Session);
			m_Session.reset();
		}
		m_Server.Stop(m_Specification.ShutdownGracePeriod);
		UnregisterMethods();
		ST_INFO("Automation: stopped ({} requests answered)", m_State->CompletedRequests);
	}

	bool EditorAutomation::IsRunning() const
	{
		return m_Server.IsRunning();
	}

	void EditorAutomation::Update()
	{
		if (!m_Server.IsRunning())
			return;

		m_State->Frame++;
		SyncMethods();
		UpdateSession();
		m_Server.ProcessRequests();
	}

	uint16_t EditorAutomation::GetPort() const
	{
		return m_Server.GetPort();
	}

	uint32_t EditorAutomation::GetClientCount() const
	{
		return m_Server.GetClientCount();
	}

	size_t EditorAutomation::GetPendingRequestCount() const
	{
		return m_State->PendingRequests;
	}

	uint64_t EditorAutomation::GetCompletedRequestCount() const
	{
		return m_State->CompletedRequests;
	}

	nlohmann::json EditorAutomation::DescribeStatus() const
	{
		return {
			{ "running", IsRunning() },
			{ "address", m_Specification.BindAddress },
			{ "port", GetPort() },
			{ "clients", GetClientCount() },
			{ "methods", m_Methods.size() },
			{ "completedRequests", m_State->CompletedRequests },
			{ "pendingRequests", m_State->PendingRequests },
			{ "pendingCommands", m_Runner.GetPendingCount() },
			{ "sessionPublished", m_Session.has_value() } };
	}

	RpcResult EditorAutomation::ToRpcResult(const EditorCommandResult& result, const std::string& command, const nlohmann::json& parameterSchema)
	{
		if (result.Success)
			return RpcResult::Success(result.Value);

		switch (result.ErrorKind)
		{
			case EditorCommandError::UnknownCommand:
				return RpcResult::Failure(JsonRpc::ErrorCode::MethodNotFound, result.Error);
			case EditorCommandError::InvalidParameters:
				// The schema lets a client correct its request without another round trip.
				return RpcResult::Failure(JsonRpc::ErrorCode::InvalidParams, result.Error, { { "command", command }, { "parameters", parameterSchema } });
			case EditorCommandError::Cancelled:
				return RpcResult::Failure(JsonRpc::ErrorCode::Cancelled, result.Error);
			case EditorCommandError::Internal:
				return RpcResult::Failure(JsonRpc::ErrorCode::InternalError, result.Error);
			case EditorCommandError::None:
			case EditorCommandError::Failed:
				break;
		}
		return RpcResult::Failure(JsonRpc::ErrorCode::OperationFailed, result.Error);
	}

	void EditorAutomation::SyncMethods()
	{
		const uint64_t revision = m_Commands.GetRevision();
		if (m_SyncedRevision == revision)
			return;

		std::map<std::string, Ref<const CommandMethod>> synced;
		for (const EditorCommand* command : m_Commands.GetAll())
		{
			const auto existing = m_Methods.find(command->Name);
			if (existing != m_Methods.end() && existing->second->Description == command->Description && existing->second->Parameters == command->Parameters)
			{
				synced.emplace(command->Name, existing->second);
				continue;
			}
			// New, or registered again with another description or schema: replaced in one step, so listings show the new
			// one and no request in between finds the method missing.
			Ref<const CommandMethod> method = CreateRef<CommandMethod>(CommandMethod { command->Name, command->Description, command->Parameters });
			RpcMethodInfo info;
			info.Name = command->Name;
			info.Description = command->Description;
			info.ParamsSchema = command->Parameters;
			const bool registered = m_Server.ReplaceMethod(std::move(info), RpcHandler([this, method](const nlohmann::json& params, const Ref<RpcResponder>& responder)
			{
				HandleRequest(method, params, responder);
			}));
			if (!registered)
			{
				ST_WARN("Automation: the command '{}' cannot be offered to clients (its name is reserved or invalid)", command->Name);
				continue;
			}
			synced.emplace(command->Name, std::move(method));
		}

		for (const auto& [name, method] : m_Methods)
		{
			if (!synced.contains(name))
				m_Server.UnregisterMethod(name);
		}
		m_Methods = std::move(synced);
		m_SyncedRevision = revision;
	}

	void EditorAutomation::UnregisterMethods()
	{
		for (const auto& [name, method] : m_Methods)
			m_Server.UnregisterMethod(name);
		m_Methods.clear();
		m_SyncedRevision.reset();
	}

	void EditorAutomation::HandleRequest(const Ref<const CommandMethod>& method, const nlohmann::json& params, const Ref<RpcResponder>& responder)
	{
		Ref<RequestTrace> trace = CreateRef<RequestTrace>();
		trace->Id = m_State->NextRequestId++;
		trace->StartFrame = m_State->Frame;
		trace->StartTime = std::chrono::steady_clock::now();
		trace->Parameters = DescribeParameters(params);

		// JSON-RPC also allows positional (array) parameters; commands only take named ones.
		if (!params.is_object())
		{
			const EditorCommandResult invalid = EditorCommandResult::InvalidParameters(fmt::format("{} takes named parameters (a JSON object)", method->Name));
			RpcResult result = ToRpcResult(invalid, method->Name, method->Parameters);
			m_State->CompletedRequests++;
			LogCompletion(*trace, method->Name, result, m_State->Frame);
			responder->Respond(std::move(result));
			return;
		}

		// The completion may run after this object is gone (the runner cancels what is still pending when it is destroyed):
		// it only uses what it holds. A responder whose client disconnected discards the answer.
		const bool pending = m_Runner.Run(m_Context, m_Commands, method->Name, params, [state = m_State, method, responder, trace](const EditorCommandResult& result)
		{
			if (trace->Pending)
				state->PendingRequests--;
			state->CompletedRequests++;
			// The command may have opened or created a project: publish it before the client hears of it.
			if (state->RefreshSession)
				state->RefreshSession();
			RpcResult rpcResult = ToRpcResult(result, method->Name, method->Parameters);
			LogCompletion(*trace, method->Name, rpcResult, state->Frame);
			responder->Respond(std::move(rpcResult));
		});
		if (pending)
		{
			trace->Pending = true;
			m_State->PendingRequests++;
			ST_TRACE("Automation: #{} {} {} is pending", trace->Id, method->Name, trace->Parameters);
		}
	}

	void EditorAutomation::UpdateSession()
	{
		if (!m_Session)
			return;
		std::string projectPath = GetProjectPath();
		if (projectPath == m_Session->ProjectPath)
			return;

		// The editor opened, created or closed a project: the session moves with it, so `StrataCLI --project` finds the
		// editor by the project it has open.
		EditorSessionInfo updated = *m_Session;
		updated.ProjectPath = std::move(projectPath);
		std::string error;
		if (!EditorSession::WriteSessionFiles(updated, &error))
			ST_WARN("Automation: updating the session file for the project '{}' failed: {}", updated.ProjectPath, error);
		EditorSession::RemoveProjectPointer(*m_Session);
		m_Session = std::move(updated);
	}

	std::string EditorAutomation::GetProjectPath() const
	{
		return m_Context.HasProject() ? FileSystem::ToUTF8(m_Context.GetProject()->GetProjectDirectory()) : std::string();
	}

}
