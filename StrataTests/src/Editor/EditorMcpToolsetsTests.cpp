// The MCP toolsets against the editor's real command registry: in the editor's tests (src/Editor), which builds without
// the editor leave out; the toolset table's own tests are in CLI/McpServerTests.cpp.
#if defined(STRATA_TESTS_HAVE_CLI)

#include <doctest/doctest.h>

#include "CLI/FakeEditor.h"
#include "CLI/McpTestClient.h"
#include "CLI/McpToolsets.h"
#include "Editor/EditorCommands.h"
#include "Strata/Core/JsonUtils.h"
#include "Strata/Network/RpcServer.h"
#include "TestHelpers.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace Strata;
using namespace Strata::CLI;

TEST_SUITE("CLI.McpToolsets")
{
	TEST_CASE("The editor's commands are few tools by default, all through toolsets, and any through strata_call")
	{
		// The editor's real commands (and the window's editor.showPanel) as the RPC methods a fake editor serves.
		EditorCommandRegistry registry;
		std::vector<RpcMethodInfo> methods;
		for (const EditorCommand* command : registry.GetAll())
			methods.push_back({ command->Name, command->Description, command->Parameters });
		methods.push_back({ "editor.showPanel", "Opens a panel of the editor's window and brings it to the front.", nlohmann::json { { "type", "object" },
			{ "properties", nlohmann::json::object() } } });
		Tests::PumpedRpcServer editor;
		for (const RpcMethodInfo& method : methods)
		{
			editor.GetServer().RegisterMethod(method, [name = method.Name](const nlohmann::json&)
			{
				return RpcResult::Success(nlohmann::json { { "ran", name } });
			});
		}
		RpcServerSpecification serverSpecification;
		serverSpecification.AuthToken = Tests::c_FakeEditorToken;
		REQUIRE(editor.Start(serverSpecification));
		Tests::McpTestClient client(Tests::MakeMcpSpecification(Tests::CreateTemporaryDirectory("McpToolsets"), editor.GetPort()));
		client.Initialize();

		// Baked lighting belongs with the scene it changes.
		CHECK(McpToolsets::GetGroupToolset("lighting.bakeProbes") == "scene");
		// Every command maps to a toolset the table names, and every core command exists.
		std::set<std::string> known;
		for (const McpToolsets::ToolsetInfo& toolset : McpToolsets::GetKnownToolsets())
			known.insert(std::string(toolset.Name));
		for (const RpcMethodInfo& method : methods)
			CHECK_MESSAGE(known.contains(McpToolsets::GetGroupToolset(method.Name)), method.Name, " is in no toolset of the table");
		for (const std::string_view core : McpToolsets::GetCoreCommands())
			CHECK_MESSAGE(registry.Find(core) != nullptr, core, " is in the core toolset but not a command");

		// The default (core) list stays within what clients take in.
		nlohmann::json response = client.Request("tools/list");
		const nlohmann::json& tools = response["result"]["tools"];
		const size_t defaultTools = tools.size();
		const size_t defaultBytes = response["result"].dump().size();
		CHECK(defaultTools <= 32);
		CHECK(defaultBytes <= 30720);
		CHECK(defaultTools == Tests::c_GenericMcpTools.size() + McpToolsets::GetCoreCommands().size());

		// strata_call reaches a command outside the core toolset.
		const nlohmann::json call = client.CallTool("strata_call", nlohmann::json { { "method", "camera.get" } });
		CHECK(call["isError"] == false);
		CHECK(call["structuredContent"]["ran"] == "camera.get");

		// strata_describe: the toolsets, one toolset's commands, one command in full.
		const nlohmann::json toolsets = client.CallTool("strata_describe")["structuredContent"];
		CHECK(toolsets["enabled"] == nlohmann::json::array({ "core" }));
		bool sceneListed = false;
		for (const nlohmann::json& toolset : toolsets["toolsets"])
			sceneListed = sceneListed || (toolset["toolset"] == "scene" && toolset["enabled"] == false && toolset["commands"].get<size_t>() > 10);
		CHECK(sceneListed);
		const nlohmann::json scene = client.CallTool("strata_describe", nlohmann::json { { "toolset", "scene" } })["structuredContent"];
		CHECK(scene["toolset"] == "scene");
		CHECK(scene["commands"].size() > 10);
		// The game UI's commands: ui.* make up the ui toolset, and its input commands go with the others into play.
		const auto toolsetCommands = [&client](const char* toolset)
		{
			const nlohmann::json described = client.CallTool("strata_describe", nlohmann::json { { "toolset", toolset } });
			std::set<std::string> commands;
			const nlohmann::json content = described.value("structuredContent", nlohmann::json::object());
			if (const nlohmann::json* listed = JsonUtils::Find(content, "commands"); listed && listed->is_array())
			{
				for (const nlohmann::json& command : *listed)
					commands.insert(command.value("command", std::string()));
			}
			return commands;
		};
		CHECK(toolsetCommands("ui") == std::set<std::string> { "ui.create", "ui.hitTest", "ui.layout" });
		const std::set<std::string> playCommands = toolsetCommands("play");
		CHECK(playCommands.contains("input.click"));
		CHECK(playCommands.contains("input.type"));
		const EditorCommand* entityGet = registry.Find("entity.get");
		REQUIRE(entityGet);
		const nlohmann::json described = client.CallTool("strata_describe", nlohmann::json { { "command", "entity.get" } })["structuredContent"];
		CHECK(described["description"] == entityGet->Description);
		CHECK(described["paramsSchema"] == entityGet->Parameters);
		CHECK(described["toolsets"] == nlohmann::json::array({ "core", "scene" }));
		CHECK(client.CallTool("strata_describe", nlohmann::json { { "command", "entity_get" } })["structuredContent"]["command"] == "entity.get");
		CHECK(client.CallTool("strata_describe", nlohmann::json { { "command", "no.such" } })["isError"] == true);
		CHECK(client.CallTool("strata_describe", nlohmann::json { { "toolset", "nothing" } })["isError"] == true);
		CHECK(client.CallTool("strata_describe", nlohmann::json { { "toolset", "scene" }, { "command", "entity.get" } })["isError"] == true);

		// Enabling a toolset lists its commands and tells the client.
		const nlohmann::json enabled = client.CallTool("strata_toolsets", nlohmann::json { { "enable", { "scene" } } });
		CHECK(enabled["structuredContent"]["enabled"] == nlohmann::json::array({ "core", "scene" }));
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 1);
		const std::vector<std::string> withScene = client.ListToolNames();
		CHECK(std::find(withScene.begin(), withScene.end(), "entity_rename") != withScene.end());
		CHECK(withScene.size() > defaultTools);

		// All toolsets together list every command.
		client.CallTool("strata_toolsets", nlohmann::json { { "enable", { "all" } } });
		const nlohmann::json all = client.Request("tools/list")["result"];
		CHECK(all["tools"].size() == Tests::c_GenericMcpTools.size() + methods.size());
		CHECK(client.CountNotifications("notifications/tools/list_changed") == 2);
	}
}

#endif
