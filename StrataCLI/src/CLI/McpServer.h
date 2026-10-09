#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/Process.h"
#include "Strata/Network/RpcServer.h"
#include "CLI/EditorConnection.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <istream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata::CLI
{

	// Protocol versions this server speaks, oldest first. The newest one is offered when a client asks for an
	// unknown version.
	inline constexpr std::string_view c_McpProtocolVersions[] = { "2024-11-05", "2025-03-26", "2025-06-18" };
	constexpr std::string_view c_McpLatestProtocolVersion = "2025-06-18";

	struct McpServerSpecification
	{
		EditorConnectionOptions Connection;
		std::optional<std::string> EditorPath; // For strata_launch_editor (see ResolveEditorPath)
		std::chrono::milliseconds CallTimeout = std::chrono::milliseconds(120000);
		std::chrono::milliseconds LaunchTimeout = std::chrono::milliseconds(60000);
		// Editors started by strata_launch_editor close themselves after this long without a connected client (the MCP
		// server stays connected while it runs), so they do not outlive the agent session. Zero: never.
		std::chrono::seconds LaunchIdleTimeout = std::chrono::seconds(600);
	};

	// Model Context Protocol server exposing the Strata editor to AI agents as tools.
	//
	// Transport-agnostic: feed it one newline-delimited JSON-RPC message per HandleLine call; every outgoing
	// message (responses and notifications) is passed to the output function as a single line without the
	// terminator. Not thread-safe: call HandleLine and Tick from one thread.
	//
	// Tools: the generic strata_status, strata_launch_editor, strata_list_methods and strata_call are always
	// available. While an editor is connected, each editor RPC method is also exposed as its own tool, named
	// after the method with characters outside [A-Za-z0-9_-] replaced by '_' (entity.create -> entity_create),
	// using the method's description and params schema. rpc.* built-ins are not exposed. When that set
	// changes (an editor connected or disconnected), notifications/tools/list_changed is sent. Once connected,
	// the server stays with that editor (see EditorConnection): if it goes away, the server reports itself
	// disconnected instead of switching to another project's editor, until strata_launch_editor picks one.
	class McpServer
	{
	public:
		using OutputFunction = std::function<void(const std::string& message)>;

		McpServer(McpServerSpecification specification, OutputFunction output);

		McpServer(const McpServer&) = delete;
		McpServer& operator=(const McpServer&) = delete;

		void HandleLine(std::string_view line);
		// Refreshes the editor connection and announces tool list changes. Call periodically while idle.
		void Tick();

		bool IsInitialized() const { return m_Initialized; }
		const std::string& GetProtocolVersion() const { return m_ProtocolVersion; }
		EditorConnection& GetConnection() { return m_Connection; }
	private:
		std::optional<nlohmann::json> HandleMessage(const nlohmann::json& message);
		nlohmann::json HandleInitialize(const nlohmann::json& params);
		nlohmann::json HandleToolsList();
		nlohmann::json HandleToolsCall(const nlohmann::json& params);

		nlohmann::json CallStatusTool();
		nlohmann::json CallLaunchEditorTool(const nlohmann::json& arguments);
		nlohmann::json CallListMethodsTool();
		nlohmann::json CallGenericCallTool(const nlohmann::json& arguments);

		// Reconnects if needed and refreshes the editor's method list.
		void RefreshEditorState();
		nlohmann::json BuildEditorTools() const;
		void AnnounceToolChanges();
		// The session of a still running editor without a project that this server started with the same options.
		std::optional<EditorSessionInfo> FindReusableEditor(bool headless, bool noGpu);
		void ReapLaunchedEditors();
		void Send(const nlohmann::json& message);
	private:
		struct LaunchedEditor
		{
			Scope<Process> EditorProcess;
			bool Headless = false;
			bool NoGpu = false;
		};

		McpServerSpecification m_Specification;
		OutputFunction m_Output;
		EditorConnection m_Connection;

		bool m_Initialized = false; // The client sent notifications/initialized
		std::string m_ProtocolVersion;

		std::vector<RpcMethodInfo> m_EditorMethods; // Empty while disconnected
		std::map<std::string, std::string> m_ToolToMethod;
		// Editor tool set the client last saw (via tools/list or a change notification); nullopt until then.
		std::optional<std::string> m_AnnouncedEditorTools;

		std::vector<LaunchedEditor> m_LaunchedEditors; // Oldest first
	};

	// Maps an editor method name to an MCP tool name.
	std::string MakeToolName(std::string_view methodName);
	// Ensures schema is a JSON Schema object for a params object (MCP requires "type": "object").
	nlohmann::json MakeToolInputSchema(const nlohmann::json& schema);
	// Converts an RPC result to an MCP tools/call result (see McpServer for the mapping).
	nlohmann::json MakeToolResult(const RpcResult& result);
	// An isError tools/call result carrying message.
	nlohmann::json MakeToolError(const std::string& message);

	// Serves MCP over a newline-delimited input stream until it ends, calling server.Tick() whenever the input
	// is idle for tickInterval. Input is read on a helper thread so ticks keep running while it blocks.
	void RunMcpServer(McpServer& server, std::istream& input, std::chrono::milliseconds tickInterval = std::chrono::milliseconds(2000));

}
