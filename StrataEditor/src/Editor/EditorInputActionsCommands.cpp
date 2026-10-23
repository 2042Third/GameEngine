#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/ProjectSettingsAction.h"

#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/ProjectSettings.h>
#include <Strata/Core/SettingsRegistry.h>
#include <Strata/Core/StringUtils.h>
#include <Strata/Input/InputActionSet.h>
#include <Strata/Project/Project.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// The project's input actions as its settings hold them (the default set, the UI map, when it sets none).
		InputActionSet GetProjectActions(const Project& project)
		{
			if (const nlohmann::json* value = project.GetSettings().FindSection(InputActionSet::c_SectionId))
			{
				if (std::optional<InputActionSet> set = InputActionSet::FromJson(*value))
					return std::move(*set);
			}
			return InputActionSet::MakeDefault();
		}

		std::string ListMaps(const InputActionSet& set)
		{
			std::string names;
			for (const InputActionMap& map : set.Maps)
				names += fmt::format("{}{}", names.empty() ? "" : ", ", map.Name);
			return names;
		}

		nlohmann::json DescribeAction(const InputActionSet& set, InputActionLocation location)
		{
			const InputAction& action = set.GetAction(location);
			nlohmann::json bindings = nlohmann::json::array();
			for (const InputBinding& binding : action.Bindings)
				bindings.push_back(binding.ToString());
			return {
				{ "name", set.GetQualifiedName(location) },
				{ "kind", InputActionKindToString(action.Kind) },
				{ "deadZone", std::round(static_cast<double>(action.DeadZone) * 1e6) / 1e6 },
				{ "bindings", std::move(bindings) },
				{ "reserved", set.IsReservedAction(location) } };
		}

		nlohmann::json DescribeMap(const InputActionSet& set, uint32_t map)
		{
			nlohmann::json actions = nlohmann::json::array();
			for (uint32_t index = 0; index < set.Maps[map].Actions.size(); index++)
				actions.push_back(DescribeAction(set, { map, index }));
			return {
				{ "name", set.Maps[map].Name },
				{ "enabled", set.Maps[map].Enabled },
				{ "reserved", StringUtils::EqualsIgnoreCase(set.Maps[map].Name, InputActionSet::c_UIMapName) },
				{ "actions", std::move(actions) } };
		}

		// The open project and its section, or the failure that says why there are none.
		std::optional<EditorCommandResult> CheckProject(EditorContext& context)
		{
			if (!context.HasProject())
				return EditorCommandResult::Fail("No project is open: input actions belong to a project (project.open or project.create first)");
			if (!context.GetSettingsRegistry().Find(InputActionSet::c_SectionId))
			{
				return EditorCommandResult::Fail(fmt::format("This editor has no '{}' settings section (the Input module registers it)",
					InputActionSet::c_SectionId));
			}
			return std::nullopt;
		}

		// Makes `set` the project's input actions as one undo step that leaves the scene saved (the default set is no section). The
		// section's Read checks the whole set first (names, limits, bindings): an invalid one changes nothing.
		std::optional<EditorCommandResult> ApplyActions(EditorContext& context, std::string stepName, const InputActionSet& set)
		{
			const Ref<Project>& project = context.GetProject();
			ProjectSettingsValues values = project->GetSettings();
			std::string error;
			if (set == InputActionSet::MakeDefault())
				values.Reset(InputActionSet::c_SectionId);
			else if (!values.Set(InputActionSet::c_SectionId, set.ToJson(), context.GetSettingsRegistry(), &error))
				return EditorCommandResult::InvalidParameters(error);
			if (!ChangeProjectSection(context, std::move(stepName), InputActionSet::c_SectionId, values.GetStoredEntry(InputActionSet::c_SectionId), &error))
				return EditorCommandResult::Fail(error);
			return std::nullopt;
		}

		// A name parameter, checked as map and action names are (InputActionSet::ValidateName).
		std::optional<std::string> ReadName(CommandArguments& arguments, std::string_view parameter)
		{
			const std::string name = arguments.GetString(parameter, "");
			if (!arguments.IsValid())
				return std::nullopt;
			if (std::optional<std::string> problem = InputActionSet::ValidateName(name))
			{
				arguments.SetError(fmt::format("Parameter '{}': {}", parameter, *problem));
				return std::nullopt;
			}
			return name;
		}

		bool IsUIMap(const InputActionMap& map)
		{
			return StringUtils::EqualsIgnoreCase(map.Name, InputActionSet::c_UIMapName);
		}

		// The bindings parameter: binding texts, each parsed and fitting `kind`.
		std::optional<std::vector<InputBinding>> ReadBindings(const nlohmann::json& texts, InputActionKind kind, std::string& error)
		{
			if (!texts.is_array())
			{
				error = "Parameter 'bindings': an array of binding texts";
				return std::nullopt;
			}
			if (texts.size() > InputActionSet::c_MaxBindings)
			{
				error = fmt::format("Parameter 'bindings': an action has at most {} bindings ({} given)", InputActionSet::c_MaxBindings, texts.size());
				return std::nullopt;
			}
			std::vector<InputBinding> bindings;
			for (const nlohmann::json& text : texts)
			{
				if (!text.is_string())
				{
					error = "Parameter 'bindings': every binding is a string";
					return std::nullopt;
				}
				std::string problem;
				const std::optional<InputBinding> binding = InputBinding::Parse(text.get<std::string>(), &problem);
				if (!binding)
				{
					error = fmt::format("Parameter 'bindings': '{}': {}", StringUtils::MakeValidUTF8(text.get<std::string>()), problem);
					return std::nullopt;
				}
				if (!binding->FitsKind(kind))
				{
					error = fmt::format("Parameter 'bindings': {} action cannot use '{}' (Button: keys, mouse and gamepad buttons, half axes; Axis: "
						"those, whole axes and 1D composites; Vector2: sticks and 2D composites)", DescribeInputActionKind(kind), binding->ToString());
					return std::nullopt;
				}
				bindings.push_back(*binding);
			}
			return bindings;
		}

		const char* c_BindingGrammar = "Key/<key>, Mouse/<button>, Gamepad/<button> (InputNames' names, any gamepad); Gamepad/<axis> (whole axis), "
			"Gamepad/<axis>+ or - (half axis; triggers only +), Gamepad/LeftStick or RightStick; 1D:<negative>,<positive> and "
			"2D:<up>,<down>,<left>,<right> of keys, mouse and gamepad buttons and half axes";

	}

	void RegisterInputActionsCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "inputActions.setMap",
			"Adds, changes or removes a map of the project's input actions (the project settings section Input.Actions; project.settings shows "
			"them all): a map unknown so far is added (enabled says whether its actions work when the game starts, default true); an existing one "
			"is renamed (rename) or enabled at start or not (enabled), or removed with its actions (remove: true). The map UI is reserved for the "
			"game UI's navigation and cannot be renamed or removed. Names are 1 to 64 letters, digits, spaces, '_', '-' and '.', unique ignoring "
			"case; at most 32 maps. One undo step that leaves the scene saved; writes the project file in the background. Allowed while playing: "
			"the running game reads the change in its next frame. Returns map ({name, enabled, reserved, actions}) and created, or removed.",
			ObjectSchema({
				{ "map", StringSchema("The map's name") },
				{ "rename", StringSchema("A new name for the map") },
				{ "enabled", BoolSchema("Whether the map's actions work when the game starts (scripts enable and disable maps while it runs)") },
				{ "remove", BoolSchema("Removes the map and its actions (no other change may be given)") } }, { "map" }),
			[](EditorContext& context, const nlohmann::json& parameters) -> EditorCommandResult
			{
				CommandArguments arguments(parameters);
				const std::string name = arguments.GetString("map");
				const bool remove = arguments.GetBool("remove", false);
				const bool enabled = arguments.GetBool("enabled", true);
				const bool hasEnabled = arguments.Has("enabled");
				const bool hasRename = arguments.Has("rename");
				std::optional<std::string> rename;
				if (hasRename)
					rename = ReadName(arguments, "rename");
				if (arguments.IsValid() && remove && (hasRename || hasEnabled))
					arguments.SetError("Parameter 'remove': a map that is removed takes no other change");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (std::optional<EditorCommandResult> failure = CheckProject(context))
					return std::move(*failure);

				InputActionSet set = GetProjectActions(*context.GetProject());
				const std::optional<uint32_t> index = set.FindMap(name);
				if (remove)
				{
					if (!index)
						return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'map': there is no map '{}' (maps: {})", name, ListMaps(set)));
					if (IsUIMap(set.Maps[*index]))
						return EditorCommandResult::InvalidParameters("Parameter 'map': the map UI is reserved (the game UI navigates with its actions) and cannot be removed");
					const std::string removed = set.Maps[*index].Name;
					set.Maps.erase(set.Maps.begin() + *index);
					if (std::optional<EditorCommandResult> failure = ApplyActions(context, "Remove Input Map", set))
						return std::move(*failure);
					return EditorCommandResult::Ok({ { "removed", removed } });
				}

				std::string stepName = "Set Input Map";
				std::string mapName = name;
				const bool created = !index.has_value();
				if (created)
				{
					if (hasRename)
						return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'map': there is no map '{}' to rename (maps: {})", name, ListMaps(set)));
					if (std::optional<std::string> problem = InputActionSet::ValidateName(name))
						return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'map': {}", *problem));
					if (set.Maps.size() >= InputActionSet::c_MaxMaps)
						return EditorCommandResult::InvalidParameters(fmt::format("The project has {} maps, the most a game can have", InputActionSet::c_MaxMaps));
					set.Maps.push_back({ name, enabled, {} });
					stepName = "Add Input Map";
				}
				else
				{
					InputActionMap& map = set.Maps[*index];
					if (rename)
					{
						if (IsUIMap(map))
							return EditorCommandResult::InvalidParameters("Parameter 'rename': the map UI is reserved (the game UI reads its actions by name) and cannot be renamed");
						const std::optional<uint32_t> other = set.FindMap(*rename);
						if (other && *other != *index)
							return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'rename': there is a map named '{}' already", set.Maps[*other].Name));
						map.Name = *rename;
						stepName = "Rename Input Map";
					}
					if (hasEnabled)
						map.Enabled = enabled;
					mapName = map.Name;
				}
				if (std::optional<EditorCommandResult> failure = ApplyActions(context, stepName, set))
					return std::move(*failure);
				const InputActionSet applied = GetProjectActions(*context.GetProject());
				const std::optional<uint32_t> appliedIndex = applied.FindMap(mapName);
				if (!appliedIndex)
					return EditorCommandResult::Fail(fmt::format("The map '{}' is not in the project's input actions after the change", mapName));
				return EditorCommandResult::Ok({ { "map", DescribeMap(applied, *appliedIndex) }, { "created", created } });
			} });

		registry.Register({ "inputActions.setAction",
			"Adds, changes, moves or removes an action of the project's input actions (the project settings section Input.Actions). The action is "
			"named \"<Map>/<Action>\" or by a bare name no other map has; an unknown one is added to its map (the map in its name, or map; "
			"inputActions.setMap adds maps) with kind (Button, the default; Axis: one value from -1 to 1; Vector2: two, +Y up), deadZone (Axis and "
			"Vector2: 0 to 0.9, default 0.15) and bindings. An existing one takes the changes given: rename, kind (its bindings must fit the new "
			"kind, or give new ones), deadZone, bindings (replaces the whole list, at most 8), map (moves it), or remove: true. Bindings are texts: "
			"Key/<key>, Mouse/<button>, Gamepad/<button>; Gamepad/<axis>, Gamepad/<axis>+ or -, Gamepad/LeftStick or RightStick; "
			"1D:<negative>,<positive> and 2D:<up>,<down>,<left>,<right> of button-like controls. The UI map's Submit, Cancel, Up, Down, Left and "
			"Right (the game UI's navigation) take new bindings but cannot be renamed, moved, removed or made another kind. One undo step that "
			"leaves the scene saved; writes the project file in the background. Allowed while playing: the running game reads the change in its "
			"next frame. Returns action ({name, kind, deadZone, bindings, reserved}) and created, or removed.",
			ObjectSchema({
				{ "action", StringSchema("The action: \"<Map>/<Action>\", or a bare name no other map has") },
				{ "map", StringSchema("The map to add the action to, or to move it to") },
				{ "rename", StringSchema("A new name for the action (unique in its map, ignoring case)") },
				{ "kind", { { "enum", { "Button", "Axis", "Vector2" } }, { "description", "Button, Axis or Vector2" } } },
				{ "deadZone", { { "type", "number" }, { "minimum", 0 }, { "maximum", InputActionSet::c_MaxDeadZone },
					{ "description", "Axis and Vector2: values within it read 0, the rest is rescaled to reach 1 (round for vectors)" } } },
				{ "bindings", { { "type", "array" }, { "maxItems", InputActionSet::c_MaxBindings }, { "description", c_BindingGrammar },
					{ "items", { { "type", "string" }, { "maxLength", InputBinding::c_MaxTextBytes } } } } },
				{ "remove", BoolSchema("Removes the action (no other change may be given)") } }, { "action" }),
			[](EditorContext& context, const nlohmann::json& parameters) -> EditorCommandResult
			{
				CommandArguments arguments(parameters);
				const std::string name = arguments.GetString("action");
				const std::string mapParameter = arguments.GetString("map", "");
				const bool remove = arguments.GetBool("remove", false);
				const std::string kindText = arguments.GetString("kind", "");
				std::optional<std::string> rename;
				if (arguments.Has("rename"))
					rename = ReadName(arguments, "rename");
				std::optional<InputActionKind> kind;
				if (arguments.IsValid() && arguments.Has("kind"))
				{
					kind = FindInputActionKind(kindText);
					if (!kind)
						arguments.SetError("Parameter 'kind': Button, Axis or Vector2");
				}
				std::optional<float> deadZone;
				if (const nlohmann::json* zone = JsonUtils::Find(parameters, "deadZone"); zone && arguments.IsValid())
				{
					const double value = zone->is_number() ? zone->get<double>() : -1.0;
					if (!(value >= 0.0 && value <= InputActionSet::c_MaxDeadZone))
						arguments.SetError(fmt::format("Parameter 'deadZone': a number from 0 to {}", InputActionSet::c_MaxDeadZone));
					deadZone = static_cast<float>(value);
				}
				const nlohmann::json* bindingTexts = JsonUtils::Find(parameters, "bindings");
				const bool changes = arguments.Has("map") || rename || kind || deadZone || bindingTexts;
				if (arguments.IsValid() && remove && changes)
					arguments.SetError("Parameter 'remove': an action that is removed takes no other change");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (std::optional<EditorCommandResult> failure = CheckProject(context))
					return std::move(*failure);

				InputActionSet set = GetProjectActions(*context.GetProject());
				std::optional<uint32_t> targetMap;
				if (arguments.Has("map"))
				{
					targetMap = set.FindMap(mapParameter);
					if (!targetMap)
					{
						return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'map': there is no map '{}' (maps: {}; inputActions.setMap adds one)",
							mapParameter, ListMaps(set)));
					}
				}

				std::string findError;
				const std::optional<InputActionLocation> location = set.FindAction(name, &findError);
				InputActionLocation result;
				bool created = false;
				std::string stepName = "Set Input Action";
				if (!location)
				{
					// An unknown action is added, unless the name is ambiguous or the request only makes sense for an existing one.
					const bool ambiguous = findError.find("is ambiguous") != std::string::npos;
					if (ambiguous || remove || rename)
						return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'action': {}", findError));
					std::string actionName = name;
					const size_t slash = name.find('/');
					if (slash != std::string::npos)
					{
						const std::optional<uint32_t> named = set.FindMap(name.substr(0, slash));
						if (!named)
						{
							return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'action': there is no map '{}' (maps: {}; inputActions.setMap adds one)",
								name.substr(0, slash), ListMaps(set)));
						}
						if (targetMap && *targetMap != *named)
							return EditorCommandResult::InvalidParameters("Parameter 'map': the action's name names another map");
						targetMap = named;
						actionName = name.substr(slash + 1);
					}
					if (!targetMap)
					{
						return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'map': name the map to add '{}' to, or give the action as "
							"\"<Map>/<Action>\" (maps: {})", name, ListMaps(set)));
					}
					if (std::optional<std::string> problem = InputActionSet::ValidateName(actionName))
						return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'action': {}", *problem));
					if (set.GetActionCount() >= InputActionSet::c_MaxActions)
						return EditorCommandResult::InvalidParameters(fmt::format("The project has {} actions, the most a game can have", InputActionSet::c_MaxActions));
					InputAction action;
					action.Name = actionName;
					action.Kind = kind.value_or(InputActionKind::Button);
					action.DeadZone = deadZone.value_or(InputActionSet::c_DefaultDeadZone);
					if (bindingTexts)
					{
						std::string error;
						std::optional<std::vector<InputBinding>> bindings = ReadBindings(*bindingTexts, action.Kind, error);
						if (!bindings)
							return EditorCommandResult::InvalidParameters(error);
						action.Bindings = std::move(*bindings);
					}
					set.Maps[*targetMap].Actions.push_back(std::move(action));
					result = { *targetMap, static_cast<uint32_t>(set.Maps[*targetMap].Actions.size() - 1) };
					created = true;
					stepName = "Add Input Action";
				}
				else
				{
					const bool reserved = set.IsReservedAction(*location);
					const std::string qualified = set.GetQualifiedName(*location);
					if (remove)
					{
						if (reserved)
							return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'action': {} is the game UI's and cannot be removed", qualified));
						std::vector<InputAction>& actions = set.Maps[location->Map].Actions;
						actions.erase(actions.begin() + location->Action);
						if (std::optional<EditorCommandResult> failure = ApplyActions(context, "Remove Input Action", set))
							return std::move(*failure);
						return EditorCommandResult::Ok({ { "removed", qualified } });
					}
					InputAction action = set.GetAction(*location);
					if (rename && !StringUtils::EqualsIgnoreCase(*rename, action.Name))
					{
						if (reserved)
							return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'rename': {} is the game UI's, which reads it by name", qualified));
						stepName = "Rename Input Action";
					}
					if (rename)
						action.Name = *rename;
					if (kind && *kind != action.Kind)
					{
						if (reserved)
							return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'kind': {} is a Button: the game UI navigates with it", qualified));
						action.Kind = *kind;
					}
					if (deadZone)
						action.DeadZone = *deadZone;
					if (bindingTexts)
					{
						std::string error;
						std::optional<std::vector<InputBinding>> bindings = ReadBindings(*bindingTexts, action.Kind, error);
						if (!bindings)
							return EditorCommandResult::InvalidParameters(error);
						action.Bindings = std::move(*bindings);
					}
					for (const InputBinding& binding : action.Bindings)
					{
						if (!binding.FitsKind(action.Kind))
						{
							return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'kind': {} action cannot use its binding '{}': give 'bindings' "
								"that fit it", DescribeInputActionKind(action.Kind), binding.ToString()));
						}
					}

					const uint32_t to = targetMap.value_or(location->Map);
					if (to != location->Map && reserved)
						return EditorCommandResult::InvalidParameters(fmt::format("Parameter 'map': {} is the game UI's and stays in the map UI", qualified));
					const std::vector<InputAction>& siblings = set.Maps[to].Actions;
					for (uint32_t index = 0; index < siblings.size(); index++)
					{
						const bool itself = to == location->Map && index == location->Action;
						if (!itself && StringUtils::EqualsIgnoreCase(siblings[index].Name, action.Name))
						{
							return EditorCommandResult::InvalidParameters(fmt::format("The map {} has an action named '{}' already", set.Maps[to].Name,
								siblings[index].Name));
						}
					}
					if (to == location->Map)
					{
						set.Maps[to].Actions[location->Action] = std::move(action);
						result = *location;
					}
					else
					{
						std::vector<InputAction>& from = set.Maps[location->Map].Actions;
						from.erase(from.begin() + location->Action);
						set.Maps[to].Actions.push_back(std::move(action));
						result = { to, static_cast<uint32_t>(set.Maps[to].Actions.size() - 1) };
						stepName = "Move Input Action";
					}
				}

				const std::string qualified = set.GetQualifiedName(result);
				if (std::optional<EditorCommandResult> failure = ApplyActions(context, stepName, set))
					return std::move(*failure);
				const InputActionSet applied = GetProjectActions(*context.GetProject());
				const std::optional<InputActionLocation> appliedLocation = applied.FindAction(qualified);
				if (!appliedLocation)
					return EditorCommandResult::Fail(fmt::format("The action '{}' is not in the project's input actions after the change", qualified));
				return EditorCommandResult::Ok({ { "action", DescribeAction(applied, *appliedLocation) }, { "created", created } });
			} });

		registry.Register({ "inputActions.resetPlayerBindings",
			"Deletes the players' bindings of the project's game: the file <project>/.strata/Saves/InputBindings.json that a rebinding menu "
			"wrote with Input::SaveBindings while the game played here, and that every play session starts with, so that the project's "
			"bindings apply again (while the file rebinds an action, inputActions.setAction's bindings do not reach play mode for it; the Input "
			"Actions page marks those actions). While playing, the running game also gets the project's bindings back from its next frame. "
			"Not undoable, like the game's saves. Returns reset: the actions whose players' bindings were dropped.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&) -> EditorCommandResult
			{
				if (!context.HasProject())
					return EditorCommandResult::Fail("No project is open: the players' bindings belong to a project's game (project.open or project.create first)");
				nlohmann::json reset = nlohmann::json::array();
				for (const auto& [action, bindings] : context.GetPlayerBindings().Actions)
					reset.push_back(action);
				std::string error;
				if (!context.ResetPlayerBindings(&error))
					return EditorCommandResult::Fail(error);
				return EditorCommandResult::Ok({ { "reset", std::move(reset) } });
			}, EditorCommandKind::Session });
	}

}
