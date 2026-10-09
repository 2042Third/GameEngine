#include "CLI/McpServer.h"

#include "CLI/EditorLauncher.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Version.h"
#include "Strata/Network/JsonRpc.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace Strata::CLI
{

	namespace
	{

		constexpr const char* c_StatusTool = "strata_status";
		constexpr const char* c_LaunchEditorTool = "strata_launch_editor";
		constexpr const char* c_ListMethodsTool = "strata_list_methods";
		constexpr const char* c_CallTool = "strata_call";
		constexpr const char* c_ListMethodsMethod = "rpc.listMethods";
		constexpr std::string_view c_ReservedMethodPrefix = "rpc.";
		constexpr std::chrono::milliseconds c_ListMethodsTimeout = std::chrono::milliseconds(10000);
		constexpr size_t c_MaxToolNameLength = 64;

		constexpr const char* c_Instructions =
			"Controls the Strata game engine editor. Call strata_status to check whether an editor is connected; "
			"if none is, call strata_launch_editor (with a project directory to open one, or without to create one with "
			"project_create). While an editor is connected, each of its commands is available as a tool (command name with "
			"'.' replaced by '_', e.g. entity_create), and strata_call invokes any method by name. editor_status gives an "
			"overview of the editor, editor_commands lists every command with its parameters, and component_list the "
			"component types. Entities and assets are referenced by 16-digit hexadecimal IDs (assets also by path). "
			"Commands that take frames (editor_wait, captures) answer when they finish. Changes made while playing are "
			"discarded by play_stop. Save with scene_save or scene_saveAs before editor_quit.";

		std::string PrettyPrint(const nlohmann::json& value)
		{
			return value.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
		}

		nlohmann::json MakeTextContent(std::string text)
		{
			return nlohmann::json { { "type", "text" }, { "text", std::move(text) } };
		}

		nlohmann::json MakeTool(std::string_view name, std::string_view description, nlohmann::json inputSchema)
		{
			return nlohmann::json { { "name", std::string(name) }, { "description", std::string(description) }, { "inputSchema", std::move(inputSchema) } };
		}

		nlohmann::json MakeEmptyObjectSchema()
		{
			return nlohmann::json { { "type", "object" }, { "properties", nlohmann::json::object() } };
		}

		nlohmann::json GetGenericTools()
		{
			nlohmann::json tools = nlohmann::json::array();
			tools.push_back(MakeTool(c_StatusTool,
				"Reports whether a Strata editor is connected, with its session (project, editor version, port) and the known editor sessions.",
				MakeEmptyObjectSchema()));

			nlohmann::json launchSchema = MakeEmptyObjectSchema();
			launchSchema["properties"]["project"] = nlohmann::json { { "type", "string" },
				{ "description", "Directory of an existing project to open; omit it to start without a project (then create one with project_create)" } };
			launchSchema["properties"]["headless"] = nlohmann::json { { "type", "boolean" }, { "description", "Run without a window (rendering stays available offscreen)" } };
			launchSchema["properties"]["noGpu"] = nlohmann::json { { "type", "boolean" },
				{ "description", "Run without a window and without a graphics device (no rendering or viewport capture), e.g. on machines without a GPU" } };
			tools.push_back(MakeTool(c_LaunchEditorTool,
				"Starts the Strata editor (for a project, reusing an editor that already has it open) and connects to it. The editor's methods then become "
				"available as tools. The editor keeps running until editor_quit.",
				std::move(launchSchema)));

			tools.push_back(MakeTool(c_ListMethodsTool,
				"Lists every method of the connected editor with its description and JSON Schema of its parameters.",
				MakeEmptyObjectSchema()));

			nlohmann::json callSchema = MakeEmptyObjectSchema();
			callSchema["properties"]["method"] = nlohmann::json { { "type", "string" }, { "description", "Editor method name, e.g. entity.create" } };
			callSchema["properties"]["params"] = nlohmann::json { { "type", "object" }, { "description", "Method parameters" } };
			callSchema["required"] = nlohmann::json::array({ "method" });
			tools.push_back(MakeTool(c_CallTool,
				"Calls any method of the connected editor by name with a params object.",
				std::move(callSchema)));
			return tools;
		}

		bool IsGenericTool(std::string_view name)
		{
			return name == c_StatusTool || name == c_LaunchEditorTool || name == c_ListMethodsTool || name == c_CallTool;
		}

		std::vector<RpcMethodInfo> ParseMethodList(const nlohmann::json& result)
		{
			std::vector<RpcMethodInfo> methods;
			const auto list = result.is_object() ? result.find("methods") : result.end();
			if (list == result.end() || !list->is_array())
				return methods;

			for (const nlohmann::json& entry : *list)
			{
				const auto name = entry.is_object() ? entry.find("name") : entry.end();
				if (name == entry.end() || !name->is_string())
					continue;

				RpcMethodInfo& method = methods.emplace_back();
				method.Name = name->get<std::string>();
				if (const auto description = entry.find("description"); description != entry.end() && description->is_string())
					method.Description = description->get<std::string>();
				if (const auto schema = entry.find("paramsSchema"); schema != entry.end())
					method.ParamsSchema = *schema;
			}
			return methods;
		}

		// Reads an optional string argument. Returns false (with error set) if it is present with another type.
		bool GetStringArgument(const nlohmann::json& arguments, const char* name, std::optional<std::string>& value, std::string& error)
		{
			const auto it = arguments.find(name);
			if (it == arguments.end() || it->is_null())
				return true;
			if (!it->is_string())
			{
				error = fmt::format("Argument '{}' must be a string", name);
				return false;
			}
			value = it->get<std::string>();
			return true;
		}

		// Reads an optional boolean argument (value keeps its default when absent). Returns false (with error set) if it
		// is present with another type.
		bool GetBoolArgument(const nlohmann::json& arguments, const char* name, bool& value, std::string& error)
		{
			const auto it = arguments.find(name);
			if (it == arguments.end() || it->is_null())
				return true;
			if (!it->is_boolean())
			{
				error = fmt::format("Argument '{}' must be a boolean", name);
				return false;
			}
			value = it->get<bool>();
			return true;
		}

	}

	////////////////////////////////////////////////////////////////////////////////
	// Tool mapping helpers
	////////////////////////////////////////////////////////////////////////////////

	std::string MakeToolName(std::string_view methodName)
	{
		std::string name;
		name.reserve(std::min(methodName.size(), c_MaxToolNameLength));
		for (char character : methodName)
		{
			if (name.size() == c_MaxToolNameLength)
				break;
			const bool valid = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character == '_' || character == '-';
			name.push_back(valid ? character : '_');
		}
		return name;
	}

	nlohmann::json MakeToolInputSchema(const nlohmann::json& schema)
	{
		if (!schema.is_object())
			return MakeEmptyObjectSchema();

		const auto type = schema.find("type");
		if (type != schema.end() && !(type->is_string() && type->get_ref<const std::string&>() == "object"))
			return MakeEmptyObjectSchema(); // Non-object params cannot be expressed as MCP tool arguments

		nlohmann::json inputSchema = schema;
		inputSchema["type"] = "object";
		if (!inputSchema.contains("properties"))
			inputSchema["properties"] = nlohmann::json::object();
		return inputSchema;
	}

	nlohmann::json MakeToolResult(const RpcResult& result)
	{
		if (result.IsError())
		{
			const RpcError& error = result.GetError();
			std::string text = fmt::format("Error {}: {}", error.Code, error.Message);
			if (!error.Data.is_null())
				text += "\n" + PrettyPrint(error.Data);
			return MakeToolError(text);
		}

		const nlohmann::json& value = result.GetValue();
		nlohmann::json content = nlohmann::json::array();
		nlohmann::json toolResult = nlohmann::json::object();
		if (!value.is_object())
		{
			content.push_back(MakeTextContent(PrettyPrint(value)));
			toolResult["content"] = std::move(content);
			toolResult["isError"] = false;
			return toolResult;
		}

		// Methods return images (e.g. viewport captures) as {"Image": {"MimeType", "Data" (base64)}}. They become
		// image content; the remaining fields are the text and structured content, without the large payload.
		nlohmann::json remaining = value;
		std::optional<nlohmann::json> image;
		if (const auto imageField = remaining.find("Image"); imageField != remaining.end() && imageField->is_object())
		{
			const auto mimeType = imageField->find("MimeType");
			const auto data = imageField->find("Data");
			if (mimeType != imageField->end() && mimeType->is_string() && data != imageField->end() && data->is_string())
			{
				image = nlohmann::json { { "type", "image" }, { "data", *data }, { "mimeType", *mimeType } };
				remaining.erase(imageField);
			}
		}

		content.push_back(MakeTextContent(PrettyPrint(remaining)));
		if (image)
			content.push_back(std::move(*image));
		toolResult["content"] = std::move(content);
		toolResult["structuredContent"] = std::move(remaining);
		toolResult["isError"] = false;
		return toolResult;
	}

	nlohmann::json MakeToolError(const std::string& message)
	{
		nlohmann::json content = nlohmann::json::array();
		content.push_back(MakeTextContent(message));
		return nlohmann::json { { "content", std::move(content) }, { "isError", true } };
	}

	////////////////////////////////////////////////////////////////////////////////
	// McpServer
	////////////////////////////////////////////////////////////////////////////////

	McpServer::McpServer(McpServerSpecification specification, OutputFunction output)
		: m_Specification(std::move(specification)), m_Output(std::move(output)), m_Connection(m_Specification.Connection)
	{
	}

	void McpServer::HandleLine(std::string_view line)
	{
		const size_t first = line.find_first_not_of(" \t\r\n");
		if (first == std::string_view::npos)
			return;
		const size_t last = line.find_last_not_of(" \t\r\n");
		const std::string_view text = line.substr(first, last - first + 1);

		std::optional<nlohmann::json> message = JsonRpc::Parse(text);
		if (!message)
		{
			Send(JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::ParseError, "Parse error: the message is not valid JSON"));
			return;
		}

		if (message->is_array())
		{
			// Batches were part of MCP 2025-03-26; answer them for clients still using that revision.
			if (message->empty())
			{
				Send(JsonRpc::MakeError(nullptr, JsonRpc::ErrorCode::InvalidRequest, "Empty batch"));
				return;
			}

			nlohmann::json responses = nlohmann::json::array();
			for (const nlohmann::json& element : *message)
			{
				if (std::optional<nlohmann::json> response = HandleMessage(element))
					responses.push_back(std::move(*response));
			}
			if (!responses.empty())
				Send(responses);
		}
		else if (std::optional<nlohmann::json> response = HandleMessage(*message))
		{
			Send(*response);
		}

		AnnounceToolChanges();
	}

	void McpServer::Tick()
	{
		ReapLaunchedEditors();
		RefreshEditorState();
		AnnounceToolChanges();
	}

	std::optional<nlohmann::json> McpServer::HandleMessage(const nlohmann::json& message)
	{
		if (JsonRpc::IsResponse(message))
			return std::nullopt; // This server never sends requests, so responses have nothing to match

		const JsonRpc::RequestValidation validation = JsonRpc::ValidateRequest(message);
		if (!validation.Valid)
			return JsonRpc::MakeError(validation.Id, JsonRpc::ErrorCode::InvalidRequest, validation.Error);

		const std::string method = message.find("method")->get<std::string>();
		const auto paramsIt = message.find("params");
		const nlohmann::json params = paramsIt != message.end() ? *paramsIt : nlohmann::json::object();

		if (validation.IsNotification)
		{
			if (method == "notifications/initialized")
				m_Initialized = true;
			return std::nullopt;
		}

		const nlohmann::json& id = validation.Id;
		// Every json access below is guarded; this only contains unexpected failures (e.g. allocation) so one
		// request cannot terminate the server.
		try
		{
			if (method == "initialize")
				return JsonRpc::MakeResult(id, HandleInitialize(params));
			if (method == "ping")
				return JsonRpc::MakeResult(id, nlohmann::json::object());
			if (method == "tools/list")
				return JsonRpc::MakeResult(id, HandleToolsList());
			if (method == "tools/call")
			{
				if (!params.is_object())
					return JsonRpc::MakeError(id, JsonRpc::ErrorCode::InvalidParams, "tools/call expects params {\"name\": string, \"arguments\": object}");
				return JsonRpc::MakeResult(id, HandleToolsCall(params));
			}
		}
		catch (const std::exception& exception)
		{
			ST_ERROR("MCP: '{}' failed: {}", method, exception.what());
			return JsonRpc::MakeError(id, JsonRpc::ErrorCode::InternalError, fmt::format("'{}' failed: {}", method, exception.what()));
		}
		return JsonRpc::MakeError(id, JsonRpc::ErrorCode::MethodNotFound, fmt::format("Method '{}' not found", method));
	}

	nlohmann::json McpServer::HandleInitialize(const nlohmann::json& params)
	{
		std::string requested;
		if (const auto version = params.is_object() ? params.find("protocolVersion") : params.end(); version != params.end() && version->is_string())
			requested = version->get<std::string>();

		const bool supported = std::find(std::begin(c_McpProtocolVersions), std::end(c_McpProtocolVersions), requested) != std::end(c_McpProtocolVersions);
		m_ProtocolVersion = supported ? requested : std::string(c_McpLatestProtocolVersion);

		nlohmann::json result = nlohmann::json::object();
		result["protocolVersion"] = m_ProtocolVersion;
		result["capabilities"] = nlohmann::json { { "tools", { { "listChanged", true } } } };
		result["serverInfo"] = nlohmann::json { { "name", "strata" }, { "version", c_EngineVersion } };
		result["instructions"] = c_Instructions;
		return result;
	}

	nlohmann::json McpServer::HandleToolsList()
	{
		RefreshEditorState();

		nlohmann::json tools = GetGenericTools();
		const nlohmann::json editorTools = BuildEditorTools();
		for (const nlohmann::json& tool : editorTools)
			tools.push_back(tool);

		m_AnnouncedEditorTools = JsonRpc::Serialize(editorTools);
		return nlohmann::json { { "tools", std::move(tools) } };
	}

	nlohmann::json McpServer::HandleToolsCall(const nlohmann::json& params)
	{
		const auto nameIt = params.find("name");
		if (nameIt == params.end() || !nameIt->is_string())
			return MakeToolError("tools/call requires a string \"name\"");
		const std::string name = nameIt->get<std::string>();

		nlohmann::json arguments = nlohmann::json::object();
		if (const auto argumentsIt = params.find("arguments"); argumentsIt != params.end() && !argumentsIt->is_null())
		{
			if (!argumentsIt->is_object())
				return MakeToolError(fmt::format("The arguments of '{}' must be an object", name));
			arguments = *argumentsIt;
		}

		if (name == c_StatusTool)
			return CallStatusTool();
		if (name == c_LaunchEditorTool)
			return CallLaunchEditorTool(arguments);
		if (name == c_ListMethodsTool)
			return CallListMethodsTool();
		if (name == c_CallTool)
			return CallGenericCallTool(arguments);

		auto method = m_ToolToMethod.find(name);
		if (method == m_ToolToMethod.end() || !m_Connection.IsConnected())
		{
			RefreshEditorState();
			method = m_ToolToMethod.find(name);
		}
		if (method == m_ToolToMethod.end())
		{
			if (!m_Connection.IsConnected())
				return MakeToolError(fmt::format("Unknown tool '{}': no Strata editor is connected ({}). Use strata_launch_editor to start one.", name, m_Connection.GetLastError()));
			return MakeToolError(fmt::format("Unknown tool '{}'", name));
		}

		const RpcResult result = m_Connection.Call(method->second, arguments, m_Specification.CallTimeout);
		if (!m_Connection.IsConnected())
			RefreshEditorState(); // The editor went away; drop its tools (announced after the response)
		return MakeToolResult(result);
	}

	nlohmann::json McpServer::CallStatusTool()
	{
		RefreshEditorState();

		nlohmann::json status = m_Connection.DescribeStatus();
		status["editorTools"] = m_ToolToMethod.size();
		nlohmann::json sessions = nlohmann::json::array();
		if (const std::optional<std::filesystem::path> sessionDirectory = m_Connection.GetSessionDirectory())
		{
			for (const EditorSessionInfo& session : EditorSession::FindSessions(*sessionDirectory))
				sessions.push_back(DescribeSession(session));
		}
		status["knownSessions"] = std::move(sessions);
		return MakeToolResult(RpcResult::Success(std::move(status)));
	}

	nlohmann::json McpServer::CallLaunchEditorTool(const nlohmann::json& arguments)
	{
		std::string error;
		std::optional<std::string> project;
		if (!GetStringArgument(arguments, "project", project, error))
			return MakeToolError(error);

		bool headless = false;
		bool noGpu = false;
		if (!GetBoolArgument(arguments, "headless", headless, error) || !GetBoolArgument(arguments, "noGpu", noGpu, error))
			return MakeToolError(error);

		std::filesystem::path projectDirectory;
		if (project && !project->empty())
		{
			std::error_code pathError;
			projectDirectory = std::filesystem::absolute(FileSystem::FromUTF8(*project), pathError);
			if (pathError)
				projectDirectory = FileSystem::FromUTF8(*project);
			projectDirectory = projectDirectory.lexically_normal();
		}

		// Reuse an editor that already has the project open instead of starting a second one.
		EditorConnectionOptions probeOptions = m_Specification.Connection;
		probeOptions.Port.reset();
		probeOptions.Token.clear();
		probeOptions.ProjectDirectory = projectDirectory;
		probeOptions.SessionDirectory = m_Connection.GetOptions().SessionDirectory;
		EditorConnection probe(probeOptions);
		if (!projectDirectory.empty() && probe.EnsureConnected() && probe.GetEndpoint() && probe.GetEndpoint()->Session)
		{
			const EditorSessionInfo session = *probe.GetEndpoint()->Session;
			probe.Disconnect();
			if (m_Connection.ConnectToSession(session))
			{
				RefreshEditorState();
				return MakeToolResult(RpcResult::Success(nlohmann::json {
					{ "launched", false },
					{ "message", "An editor already has this project open; connected to it" },
					{ "session", DescribeSession(session) },
					{ "editorTools", m_ToolToMethod.size() } }));
			}
		}

		EditorLaunchSpecification launch;
		launch.EditorPath = ResolveEditorPath(m_Specification.EditorPath);
		launch.ProjectDirectory = projectDirectory;
		launch.Headless = headless;
		launch.NoGpu = noGpu;
		launch.WaitTimeout = m_Specification.LaunchTimeout;
		launch.SessionDirectory = m_Connection.GetOptions().SessionDirectory;

		EditorLaunchResult launched = LaunchEditor(launch);
		if (launched.EditorProcess)
			m_LaunchedEditors.push_back(std::move(launched.EditorProcess));
		if (!launched.Success)
			return MakeToolError(launched.Error);

		if (!m_Connection.ConnectToSession(launched.Session))
			return MakeToolError(fmt::format("The editor started but connecting to it failed: {}", m_Connection.GetLastError()));

		RefreshEditorState();
		return MakeToolResult(RpcResult::Success(nlohmann::json {
			{ "launched", true },
			{ "session", DescribeSession(launched.Session) },
			{ "editorTools", m_ToolToMethod.size() } }));
	}

	nlohmann::json McpServer::CallListMethodsTool()
	{
		return MakeToolResult(m_Connection.Call(c_ListMethodsMethod, nlohmann::json::object(), c_ListMethodsTimeout));
	}

	nlohmann::json McpServer::CallGenericCallTool(const nlohmann::json& arguments)
	{
		std::string error;
		std::optional<std::string> method;
		if (!GetStringArgument(arguments, "method", method, error))
			return MakeToolError(error);
		if (!method || method->empty())
			return MakeToolError("Missing required argument 'method' (the editor method name)");

		nlohmann::json params = nlohmann::json::object();
		if (const auto paramsIt = arguments.find("params"); paramsIt != arguments.end() && !paramsIt->is_null())
		{
			if (!paramsIt->is_object() && !paramsIt->is_array())
				return MakeToolError("Argument 'params' must be an object");
			params = *paramsIt;
		}

		const RpcResult result = m_Connection.Call(*method, params, m_Specification.CallTimeout);
		if (!m_Connection.IsConnected())
			RefreshEditorState();
		return MakeToolResult(result);
	}

	void McpServer::RefreshEditorState()
	{
		std::vector<RpcMethodInfo> methods;
		if (m_Connection.EnsureConnected())
		{
			const RpcResult result = m_Connection.Call(c_ListMethodsMethod, nlohmann::json::object(), c_ListMethodsTimeout);
			if (result.IsSuccess())
				methods = ParseMethodList(result.GetValue());
			else
				ST_WARN("MCP: listing the editor's methods failed: {}", result.GetError().Message);
		}

		m_EditorMethods.clear();
		m_ToolToMethod.clear();
		for (RpcMethodInfo& method : methods)
		{
			if (method.Name.starts_with(c_ReservedMethodPrefix))
				continue;

			const std::string toolName = MakeToolName(method.Name);
			if (toolName.empty() || IsGenericTool(toolName) || m_ToolToMethod.contains(toolName))
			{
				ST_WARN("MCP: editor method '{}' is not exposed as a tool (its name '{}' is taken or empty)", method.Name, toolName);
				continue;
			}
			m_ToolToMethod.emplace(toolName, method.Name);
			m_EditorMethods.push_back(std::move(method));
		}
	}

	nlohmann::json McpServer::BuildEditorTools() const
	{
		nlohmann::json tools = nlohmann::json::array();
		for (const RpcMethodInfo& method : m_EditorMethods)
		{
			const std::string description = method.Description.empty() ? fmt::format("Calls the editor method '{}'.", method.Name) : method.Description;
			tools.push_back(MakeTool(MakeToolName(method.Name), description, MakeToolInputSchema(method.ParamsSchema)));
		}
		return tools;
	}

	void McpServer::AnnounceToolChanges()
	{
		if (!m_Initialized || !m_AnnouncedEditorTools)
			return;

		std::string current = JsonRpc::Serialize(BuildEditorTools());
		if (current == *m_AnnouncedEditorTools)
			return;

		m_AnnouncedEditorTools = std::move(current);
		Send(JsonRpc::MakeNotification("notifications/tools/list_changed", nullptr));
	}

	void McpServer::ReapLaunchedEditors()
	{
		std::erase_if(m_LaunchedEditors, [](const Scope<Process>& process) { return !process->IsRunning(); });
	}

	void McpServer::Send(const nlohmann::json& message)
	{
		if (m_Output)
			m_Output(JsonRpc::Serialize(message));
	}

	////////////////////////////////////////////////////////////////////////////////
	// RunMcpServer
	////////////////////////////////////////////////////////////////////////////////

	void RunMcpServer(McpServer& server, std::istream& input, std::chrono::milliseconds tickInterval)
	{
		struct InputQueue
		{
			std::mutex Mutex;
			std::condition_variable Condition;
			std::deque<std::string> Lines;
			bool Finished = false;
		};

		InputQueue queue;
		std::thread reader([&queue, &input]()
		{
			Platform::SetCurrentThreadName("McpInput");
			std::string line;
			while (std::getline(input, line))
			{
				{
					std::scoped_lock<std::mutex> lock(queue.Mutex);
					queue.Lines.push_back(std::move(line));
				}
				queue.Condition.notify_one();
				line.clear();
			}

			{
				std::scoped_lock<std::mutex> lock(queue.Mutex);
				queue.Finished = true;
			}
			queue.Condition.notify_one();
		});

		auto nextTick = std::chrono::steady_clock::now() + tickInterval;
		while (true)
		{
			std::deque<std::string> lines;
			bool finished = false;
			{
				std::unique_lock<std::mutex> lock(queue.Mutex);
				queue.Condition.wait_until(lock, nextTick, [&queue]() { return !queue.Lines.empty() || queue.Finished; });
				lines.swap(queue.Lines);
				finished = queue.Finished;
			}

			for (const std::string& line : lines)
				server.HandleLine(line);
			if (finished && lines.empty())
				break;

			if (std::chrono::steady_clock::now() >= nextTick)
			{
				server.Tick();
				nextTick = std::chrono::steady_clock::now() + tickInterval;
			}
		}
		reader.join();
	}

}
