#pragma once

#include "CLI/FakeEditor.h"
#include "CLI/McpServer.h"
#include "Strata/Network/JsonRpc.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// An MCP client for tests of the MCP server (CLI.Mcp, CLI.McpToolsets): it drives an McpServer through its lines as a
// client would and records everything the server writes.
namespace Strata::Tests
{

	// The tools the MCP server lists whatever the editor serves.
	inline const std::vector<std::string> c_GenericMcpTools = { "strata_status", "strata_launch_editor", "strata_describe", "strata_toolsets", "strata_call" };

	// A server that finds no editor (no port), or connects to the fake or test editor on `port` with the fake editor's token.
	inline CLI::McpServerSpecification MakeMcpSpecification(const std::filesystem::path& sessionDirectory, std::optional<uint16_t> port = std::nullopt)
	{
		CLI::McpServerSpecification specification;
		specification.Connection.SessionDirectory = sessionDirectory;
		specification.Connection.Port = port;
		specification.Connection.Token = port ? c_FakeEditorToken : "";
		specification.Connection.ConnectTimeout = std::chrono::milliseconds(3000);
		specification.CallTimeout = std::chrono::milliseconds(5000);
		specification.LaunchTimeout = std::chrono::milliseconds(2000);
		return specification;
	}

	// Drives an McpServer like an MCP client and records everything it writes.
	class McpTestClient
	{
	public:
		explicit McpTestClient(CLI::McpServerSpecification specification)
			: m_Server(std::move(specification), [this](const std::string& line) { m_Output.push_back(line); })
		{
		}

		void SendLine(std::string_view line)
		{
			m_Server.HandleLine(line);
		}

		// Sends a request and returns its response (null if none was written).
		nlohmann::json Request(const std::string& method, nlohmann::json params = nlohmann::json::object())
		{
			const int64_t id = m_NextId++;
			const size_t firstNew = m_Output.size();
			m_Server.HandleLine(JsonRpc::Serialize(JsonRpc::MakeRequest(id, method, std::move(params))));
			for (size_t index = firstNew; index < m_Output.size(); index++)
			{
				std::optional<nlohmann::json> message = JsonRpc::Parse(m_Output[index]);
				if (message && message->is_object() && message->contains("id") && (*message)["id"] == id)
					return *message;
			}
			return nullptr;
		}

		void Notify(const std::string& method)
		{
			m_Server.HandleLine(JsonRpc::Serialize(JsonRpc::MakeNotification(method, nullptr)));
		}

		void Initialize()
		{
			nlohmann::json response = Request("initialize", nlohmann::json {
				{ "protocolVersion", "2025-06-18" },
				{ "capabilities", nlohmann::json::object() },
				{ "clientInfo", { { "name", "StrataTests" }, { "version", "1.0" } } } });
			REQUIRE(response.contains("result"));
			Notify("notifications/initialized");
		}

		nlohmann::json CallTool(const std::string& name, nlohmann::json arguments = nlohmann::json::object())
		{
			nlohmann::json response = Request("tools/call", nlohmann::json { { "name", name }, { "arguments", std::move(arguments) } });
			REQUIRE(response.contains("result"));
			return response["result"];
		}

		std::vector<std::string> ListToolNames()
		{
			nlohmann::json response = Request("tools/list");
			REQUIRE(response.contains("result"));
			std::vector<std::string> names;
			for (const nlohmann::json& tool : response["result"]["tools"])
				names.push_back(tool["name"].get<std::string>());
			return names;
		}

		size_t CountNotifications(const std::string& method) const
		{
			return static_cast<size_t>(std::count_if(m_Output.begin(), m_Output.end(), [&](const std::string& line)
			{
				std::optional<nlohmann::json> message = JsonRpc::Parse(line);
				return message && message->is_object() && !message->contains("id") && message->value("method", "") == method;
			}));
		}

		CLI::McpServer& GetServer() { return m_Server; }
		const std::vector<std::string>& GetOutput() const { return m_Output; }
	private:
		std::vector<std::string> m_Output;
		CLI::McpServer m_Server;
		int64_t m_NextId = 1;
	};

}
