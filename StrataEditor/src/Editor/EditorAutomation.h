#pragma once

#include "Editor/EditorCommands.h"

#include <Strata/Core/Base.h>
#include <Strata/Network/EditorSession.h>
#include <Strata/Network/RpcServer.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace Strata
{

	class EditorCommandRunner;
	class EditorContext;

	struct EditorAutomationSpecification
	{
		std::string BindAddress = "127.0.0.1"; // Loopback only (RpcServer refuses anything else)
		uint16_t Port = 0;                     // 0 picks a free ephemeral port
		// Secret that clients prove in the handshake; empty generates a fresh one (EditorSession::GenerateSessionToken).
		std::string AuthToken;
		// Publish the session (EditorSession::WriteSessionFiles) so StrataCLI and its MCP server can find this editor, and
		// keep it in step with the open project. The directory is EditorSession::GetSessionDirectory (STRATA_SESSION_DIR,
		// else the per-user data directory). Without a published session only holders of the token can connect.
		bool PublishSession = true;
		bool Headless = false; // Recorded in the session
		// How long Stop keeps delivering answers (e.g. to editor.quit, or the cancellation of pending commands).
		std::chrono::milliseconds ShutdownGracePeriod = std::chrono::milliseconds(2000);
	};

	// Serves the editor to tools and AI agents: JSON-RPC 2.0 on loopback (RpcServer), authenticated with the session token.
	//
	// Every command of the registry is a method with the command's name, description and parameter schema, so clients
	// discover them through rpc.listMethods (StrataCLI's MCP server turns them into tools). Commands registered later (or
	// registered again with another description or schema) are picked up by the next Update. Requests run on the main
	// thread from Update, through the EditorCommandRunner: a command that finishes over several frames answers when it
	// completes, and a client that disconnects in the meantime loses only the answer (the command still finishes).
	// Failures become JSON-RPC errors by kind (see ToRpcResult). Each request is logged at trace level.
	//
	// The context, the registry and the runner must outlive the automation. Main thread only.
	class EditorAutomation
	{
	public:
		EditorAutomation(EditorContext& context, const EditorCommandRegistry& commands, EditorCommandRunner& runner);
		// Stops the server (see Stop).
		~EditorAutomation();

		EditorAutomation(const EditorAutomation&) = delete;
		EditorAutomation& operator=(const EditorAutomation&) = delete;

		// Starts serving (stopping a previous session first) and publishes the session. Returns false with the reason when
		// the server cannot listen or the session cannot be published.
		bool Start(const EditorAutomationSpecification& specification, std::string* outError = nullptr);
		// Removes the session files, then stops the server, delivering the answers that are owed for up to the shutdown
		// grace period. Pending commands belong to the runner: cancel them first (EditorCommandRunner::CancelAll) so their
		// clients are told.
		void Stop();
		bool IsRunning() const;

		// Once per frame: offers commands registered since the last frame, keeps the session in step with the open project
		// and runs the requests that arrived.
		void Update();

		uint16_t GetPort() const;
		uint32_t GetClientCount() const;
		// Requests whose command has not finished yet.
		size_t GetPendingRequestCount() const;
		// Requests answered since Start.
		uint64_t GetCompletedRequestCount() const;
		// The published session (nullopt when not running or not published). Holds the token: never log it.
		const std::optional<EditorSessionInfo>& GetSession() const { return m_Session; }
		// The "automation" section of editor.status.
		nlohmann::json DescribeStatus() const;

		// The JSON-RPC answer to a command result: the value on success; for failures, by kind, MethodNotFound (unknown
		// command), InvalidParams (data: {"command", "parameters": the command's parameter schema}), OperationFailed (a valid
		// request that could not be carried out), Cancelled, or InternalError.
		static RpcResult ToRpcResult(const EditorCommandResult& result, const std::string& command, const nlohmann::json& parameterSchema);
	private:
		struct CommandMethod
		{
			std::string Name;
			std::string Description;
			nlohmann::json Parameters; // The command's schema as registered, to notice changes
		};

		// Bookkeeping shared with the completions of pending commands, which may outlive this object.
		struct RequestState
		{
			uint64_t Frame = 0;
			uint64_t NextRequestId = 1;
			uint64_t CompletedRequests = 0;
			size_t PendingRequests = 0;
		};

		void SyncMethods();
		void UnregisterMethods();
		void HandleRequest(const Ref<const CommandMethod>& method, const nlohmann::json& params, const Ref<RpcResponder>& responder);
		void UpdateSession();
		std::string GetProjectPath() const;
	private:
		EditorContext& m_Context;
		const EditorCommandRegistry& m_Commands;
		EditorCommandRunner& m_Runner;

		EditorAutomationSpecification m_Specification;
		RpcServer m_Server;
		std::map<std::string, Ref<const CommandMethod>> m_Methods; // Commands offered as methods, by name
		std::optional<uint64_t> m_SyncedRevision;                    // Registry revision m_Methods reflects
		Ref<RequestState> m_State;
		std::optional<EditorSessionInfo> m_Session;
	};

}
