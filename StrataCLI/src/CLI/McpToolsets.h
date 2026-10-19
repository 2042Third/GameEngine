#pragma once

#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata::CLI::McpToolsets
{

	// The toolset every MCP session starts with: the commands a game is made and tested with.
	constexpr std::string_view c_Core = "core";
	// Asks for every toolset (StrataCLI mcp --toolsets all).
	constexpr std::string_view c_All = "all";

	// The commands of the core toolset.
	std::span<const std::string_view> GetCoreCommands();
	bool IsCore(std::string_view method);
	// The toolset of a command's group: exact names first (asset.stats goes to streaming, editor.perf to perf), then the
	// table by prefix (scene., entity., component., prefab., selection. and edit. go to scene...), and a group the table
	// does not know gets a toolset named after its prefix, so new groups map without edits here.
	std::string GetGroupToolset(std::string_view method);
	// The toolsets a command belongs to: core when it is in the core list, and its group's.
	std::vector<std::string> GetToolsets(std::string_view method);
	// The toolsets the table names, with a line about each (strata_describe without arguments lists them with their
	// commands; toolsets of unknown groups are added there).
	struct ToolsetInfo
	{
		std::string_view Name;
		std::string_view Description;
	};
	std::span<const ToolsetInfo> GetKnownToolsets();

	// The first sentence of a description (up to the first ". " or the end), cut to `maxLength` bytes at a word boundary
	// (then ending with an ellipsis, counted as one character); outCut says whether anything was left out.
	std::string FirstSentence(std::string_view text, size_t maxLength, bool* outCut = nullptr);
	// A tool definition that costs a client few tokens: the description's first sentence (at most 160 characters, with
	// "(details: strata_describe)" when something was left out) and the schema's property descriptions cut to their first
	// sentence (at most 100 characters); types, enums, ranges and required stay. strata_describe gives the full text.
	nlohmann::json MakeCompactToolDefinition(std::string_view name, std::string_view description, const nlohmann::json& inputSchema);

}
