#include "CLI/McpToolsets.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Strata::CLI::McpToolsets
{

	namespace
	{

		constexpr size_t c_MaxDescriptionLength = 160;
		constexpr size_t c_MaxPropertyDescriptionLength = 100;
		constexpr std::string_view c_DetailsNote = " (details: strata_describe)";

		constexpr std::string_view c_CoreCommands[] = {
			"editor.status", "editor.batch", "log.read",
			"project.create", "project.open",
			"scene.open", "scene.save", "scene.info", "scene.hierarchy",
			"entity.create", "entity.get", "entity.find", "entity.delete",
			"component.add", "component.set",
			"prefab.instantiate",
			"asset.list", "asset.info", "asset.import",
			"material.create",
			"script.build", "script.status",
			"play.start", "play.stop", "play.step",
			"input.sequence",
			"viewport.capture" };

		// Exact names win over prefixes.
		constexpr std::pair<std::string_view, std::string_view> c_ExactToolsets[] = {
			{ "asset.stats", "streaming" },
			{ "asset.setBudget", "streaming" },
			{ "editor.perf", "perf" },
			{ "project.settings", "settings" },
			{ "project.setSettings", "settings" },
			{ "project.setCollisionLayers", "settings" },
			{ "project.setGameInfo", "settings" },
			{ "project.save", "settings" } };

		constexpr std::pair<std::string_view, std::string_view> c_PrefixToolsets[] = {
			{ "scene.", "scene" }, { "entity.", "scene" }, { "component.", "scene" }, { "prefab.", "scene" }, { "selection.", "scene" },
			{ "edit.", "scene" },
			{ "asset.", "assets" }, { "material.", "assets" }, { "project.", "assets" },
			{ "play.", "play" }, { "input.", "play" }, { "audio.", "play" },
			{ "script.", "scripts" },
			{ "animation.", "animation" },
			{ "viewport.", "viewport" }, { "camera.", "viewport" },
			{ "editor.", "editor" }, { "log.", "editor" },
			{ "ui.", "ui" },
			{ "world.", "world" }, { "physics.", "world" },
			{ "preferences.", "settings" }, { "inputActions.", "settings" } };

		constexpr ToolsetInfo c_Toolsets[] = {
			{ "core", "Making and testing a game: project, scene, entities, components, prefabs, assets, scripts, play, input sequences, captures" },
			{ "scene", "Scenes, entities, components, prefabs, the selection and undo" },
			{ "assets", "Assets, materials, imports, projects and exports" },
			{ "play", "Play mode, stepping, simulated input and sound" },
			{ "scripts", "Script classes, builds, fields and the external code editor" },
			{ "animation", "Animation controllers and previews" },
			{ "viewport", "The editor camera, view settings and captures" },
			{ "streaming", "Asset residency statistics and budgets" },
			{ "perf", "Where frames go: the frame profiler" },
			{ "editor", "The editor itself: status, activity, panels, dialogs, logs" },
			{ "ui", "Game UI: canvases and widgets" },
			{ "world", "Worlds, their cells, and physics statistics" },
			{ "settings", "Project settings, collision layers, input actions, the game's identity and the editor's preferences" } };

		size_t FindSentenceEnd(std::string_view text)
		{
			for (size_t index = 0; index + 1 < text.size(); index++)
			{
				// ". " ends a sentence unless it is part of a name ("e.g. ", a decimal); good enough for descriptions written
				// for agents, whose first sentences end with ". ".
				if (text[index] == '.' && text[index + 1] == ' ')
				{
					const std::string_view before = text.substr(0, index + 1);
					if (before.ends_with("e.g.") || before.ends_with("i.e.") || before.ends_with("etc."))
						continue;
					return index + 1;
				}
			}
			return text.size();
		}

		void CompactProperties(nlohmann::json& schema)
		{
			const auto properties = schema.find("properties");
			if (properties == schema.end() || !properties->is_object())
				return;
			for (auto& [name, property] : properties->items())
			{
				if (!property.is_object())
					continue;
				if (const auto description = property.find("description"); description != property.end() && description->is_string())
					*description = FirstSentence(description->get<std::string>(), c_MaxPropertyDescriptionLength);
				// Nested objects (a box's min and max) are described the same way.
				if (property.contains("properties"))
					CompactProperties(property);
				if (const auto items = property.find("items"); items != property.end() && items->is_object())
				{
					if (const auto itemDescription = items->find("description"); itemDescription != items->end() && itemDescription->is_string())
						*itemDescription = FirstSentence(itemDescription->get<std::string>(), c_MaxPropertyDescriptionLength);
					CompactProperties(*items);
				}
			}
		}

	}

	std::span<const std::string_view> GetCoreCommands()
	{
		return c_CoreCommands;
	}

	bool IsCore(std::string_view method)
	{
		return std::find(std::begin(c_CoreCommands), std::end(c_CoreCommands), method) != std::end(c_CoreCommands);
	}

	std::string GetGroupToolset(std::string_view method)
	{
		for (const auto& [name, toolset] : c_ExactToolsets)
		{
			if (method == name)
				return std::string(toolset);
		}
		for (const auto& [prefix, toolset] : c_PrefixToolsets)
		{
			if (method.starts_with(prefix))
				return std::string(toolset);
		}
		const size_t dot = method.find('.');
		return std::string(dot == std::string_view::npos ? method : method.substr(0, dot));
	}

	std::vector<std::string> GetToolsets(std::string_view method)
	{
		std::vector<std::string> toolsets;
		if (IsCore(method))
			toolsets.emplace_back(c_Core);
		toolsets.push_back(GetGroupToolset(method));
		return toolsets;
	}

	std::span<const ToolsetInfo> GetKnownToolsets()
	{
		return c_Toolsets;
	}

	std::string FirstSentence(std::string_view text, size_t maxLength, bool* outCut)
	{
		const size_t end = FindSentenceEnd(text);
		std::string_view sentence = text.substr(0, end);
		if (outCut)
			*outCut = end < text.size() || sentence.size() > maxLength;
		if (sentence.size() <= maxLength)
			return std::string(sentence);
		if (maxLength == 0)
			return {};
		// Cut at the last space that leaves room for the ellipsis, else in the middle of the word (never inside a UTF-8
		// sequence).
		size_t length = sentence.rfind(' ', maxLength - 1);
		if (length == std::string_view::npos || length == 0)
		{
			length = maxLength - 1;
			while (length > 0 && (static_cast<unsigned char>(sentence[length]) & 0xC0) == 0x80)
				length--;
		}
		return std::string(sentence.substr(0, length)) + "\xE2\x80\xA6"; // An ellipsis: the rest is in strata_describe
	}

	nlohmann::json MakeCompactToolDefinition(std::string_view name, std::string_view description, const nlohmann::json& inputSchema)
	{
		bool cut = false;
		std::string text = FirstSentence(description, c_MaxDescriptionLength, &cut);
		if (cut)
			text += c_DetailsNote;
		nlohmann::json schema = inputSchema;
		CompactProperties(schema);
		return { { "name", std::string(name) }, { "description", std::move(text) }, { "inputSchema", std::move(schema) } };
	}

}
