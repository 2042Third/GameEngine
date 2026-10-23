#include "stpch.h"
#include "Strata/Input/InputActionSet.h"

#include "Strata/Core/JsonUtils.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Core/StringUtils.h"
#include "Strata/Input/InputNames.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Strata
{

	namespace
	{

		constexpr std::string_view c_KeyPrefix = "Key/";
		constexpr std::string_view c_MousePrefix = "Mouse/";
		constexpr std::string_view c_GamepadPrefix = "Gamepad/";
		constexpr std::string_view c_Composite1DPrefix = "1D:";
		constexpr std::string_view c_Composite2DPrefix = "2D:";
		constexpr const char* c_StickNames[] = { "LeftStick", "RightStick" };

		bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix)
		{
			return text.size() >= prefix.size() && StringUtils::EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
		}

		std::optional<InputControl> ParseControl(std::string_view text, std::string& error)
		{
			InputControl control;
			if (StartsWithIgnoreCase(text, c_KeyPrefix))
			{
				const std::optional<KeyCode> key = InputNames::FindKey(text.substr(c_KeyPrefix.size()));
				if (!key)
				{
					error = fmt::format("'{}' is no key", text.substr(c_KeyPrefix.size()));
					return std::nullopt;
				}
				control.Type = InputControlType::Key;
				control.Code = *key;
				return control;
			}
			if (StartsWithIgnoreCase(text, c_MousePrefix))
			{
				const std::optional<MouseCode> button = InputNames::FindMouseButton(text.substr(c_MousePrefix.size()));
				if (!button)
				{
					error = fmt::format("'{}' is no mouse button", text.substr(c_MousePrefix.size()));
					return std::nullopt;
				}
				control.Type = InputControlType::MouseButton;
				control.Code = *button;
				return control;
			}
			if (StartsWithIgnoreCase(text, c_GamepadPrefix))
			{
				const std::string_view name = text.substr(c_GamepadPrefix.size());
				for (uint16_t stick = 0; stick < 2; stick++)
				{
					if (StringUtils::EqualsIgnoreCase(name, c_StickNames[stick]))
					{
						control.Type = InputControlType::GamepadStick;
						control.Code = stick;
						return control;
					}
				}
				if (const std::optional<GamepadButton> button = InputNames::FindGamepadButton(name))
				{
					control.Type = InputControlType::GamepadButton;
					control.Code = static_cast<uint16_t>(*button);
					return control;
				}
				const bool half = name.ends_with('+') || name.ends_with('-');
				const std::string_view axisName = half ? name.substr(0, name.size() - 1) : name;
				const std::optional<GamepadAxis> axis = InputNames::FindGamepadAxis(axisName);
				if (!axis)
				{
					error = fmt::format("'{}' is no gamepad button, axis, half axis (an axis and + or -) or stick (LeftStick, RightStick)", name);
					return std::nullopt;
				}
				control.Code = static_cast<uint16_t>(*axis);
				if (!half)
				{
					control.Type = InputControlType::GamepadAxis;
					return control;
				}
				control.Type = InputControlType::GamepadHalfAxis;
				control.Sign = name.back() == '+' ? 1 : -1;
				if (control.Sign < 0 && GetGamepadAxisMinimum(*axis) >= 0.0f)
				{
					error = fmt::format("'{}' has no - half: a trigger goes from 0 to 1 (use {}+)", InputNames::GetGamepadAxisName(*axis),
						InputNames::GetGamepadAxisName(*axis));
					return std::nullopt;
				}
				return control;
			}
			error = fmt::format("'{}' is no control: bindings start with Key/, Mouse/, Gamepad/, 1D: or 2D:", text);
			return std::nullopt;
		}

		std::string ControlToString(const InputControl& control)
		{
			switch (control.Type)
			{
				case InputControlType::Key:
				{
					const char* name = InputNames::GetKeyName(control.Code);
					return fmt::format("{}{}", c_KeyPrefix, name ? name : "?");
				}
				case InputControlType::MouseButton:
				{
					const char* name = InputNames::GetMouseButtonName(control.Code);
					return fmt::format("{}{}", c_MousePrefix, name ? name : "?");
				}
				case InputControlType::GamepadButton:
				{
					const char* name = InputNames::GetGamepadButtonName(static_cast<GamepadButton>(control.Code));
					return fmt::format("{}{}", c_GamepadPrefix, name ? name : "?");
				}
				case InputControlType::GamepadHalfAxis:
				case InputControlType::GamepadAxis:
				{
					const char* name = InputNames::GetGamepadAxisName(static_cast<GamepadAxis>(control.Code));
					const char* half = control.Type == InputControlType::GamepadAxis ? "" : (control.Sign < 0 ? "-" : "+");
					return fmt::format("{}{}{}", c_GamepadPrefix, name ? name : "?", half);
				}
				case InputControlType::GamepadStick:
					return fmt::format("{}{}", c_GamepadPrefix, control.Code < 2 ? c_StickNames[control.Code] : "?");
			}
			return "?";
		}

		std::string Describe(const InputActionMap& map, const InputAction* action)
		{
			return action ? fmt::format("action '{}/{}'", map.Name, action->Name) : fmt::format("map '{}'", map.Name);
		}

		std::vector<InputAction> MakeDefaultUIActions()
		{
			// The bindings the game UI navigated with before actions could be rebound, in UIAction order.
			const std::pair<const char*, std::vector<const char*>> c_Defaults[] = {
				{ "Submit", { "Key/Enter", "Key/KPEnter", "Key/Space", "Gamepad/A" } },
				{ "Cancel", { "Key/Escape", "Gamepad/B" } },
				{ "Up", { "Key/Up", "Gamepad/DPadUp", "Gamepad/LeftY+" } },
				{ "Down", { "Key/Down", "Gamepad/DPadDown", "Gamepad/LeftY-" } },
				{ "Left", { "Key/Left", "Gamepad/DPadLeft", "Gamepad/LeftX-" } },
				{ "Right", { "Key/Right", "Gamepad/DPadRight", "Gamepad/LeftX+" } },
			};
			std::vector<InputAction> actions;
			for (const auto& [name, bindings] : c_Defaults)
			{
				InputAction action;
				action.Name = name;
				for (const char* text : bindings)
				{
					const std::optional<InputBinding> binding = InputBinding::Parse(text);
					ST_CORE_VERIFY(binding.has_value(), "The UI map's default binding '{}' is invalid", text);
					action.Bindings.push_back(*binding);
				}
				actions.push_back(std::move(action));
			}
			return actions;
		}

		// The dead zone as the project file stores it: rounded to six decimals, so 0.15 is written as 0.15.
		double StoreDeadZone(float deadZone)
		{
			return std::round(static_cast<double>(deadZone) * 1e6) / 1e6;
		}

		nlohmann::json MakeSchema()
		{
			const std::string namePattern = "^[A-Za-z0-9_.-]([A-Za-z0-9 _.-]*[A-Za-z0-9_.-])?$";
			const nlohmann::json name = {
				{ "type", "string" },
				{ "minLength", 1 },
				{ "maxLength", InputActionSet::c_MaxNameLength },
				{ "pattern", namePattern } };
			nlohmann::json actionName = name;
			actionName["description"] = "Unique in its map, ignoring case. Scripts name the action \"<Map>/<Action>\", or by this name alone when no other map "
				"has an action of that name";
			nlohmann::json mapName = name;
			mapName["description"] = "Unique in the set, ignoring case. The map UI is reserved: the game UI navigates with its actions Submit, Cancel, Up, "
				"Down, Left and Right (Buttons that cannot be renamed or removed; their bindings can change)";
			const nlohmann::json action = {
				{ "type", "object" },
				{ "properties", {
					{ "Name", actionName },
					{ "Kind", { { "enum", { "Button", "Axis", "Vector2" } },
						{ "description", "Button: down or up. Axis: one value from -1 to 1. Vector2: two values (+X right, +Y up). Default Button" } } },
					{ "DeadZone", { { "type", "number" }, { "minimum", 0 }, { "maximum", InputActionSet::c_MaxDeadZone },
						{ "description", "Axis and Vector2: values within it read 0, the rest is rescaled to reach 1 (round for sticks). Default 0.15" } } },
					{ "Bindings", {
						{ "type", "array" },
						{ "maxItems", InputActionSet::c_MaxBindings },
						{ "description", "The controls that drive the action: Key/<key>, Mouse/<button>, Gamepad/<button> (InputNames' names, any gamepad); "
							"Gamepad/<axis> (whole axis), Gamepad/<axis>+ or - (half axis; triggers only +), Gamepad/LeftStick or RightStick; "
							"1D:<negative>,<positive> and 2D:<up>,<down>,<left>,<right> of button-like controls (keys, mouse and gamepad buttons, half "
							"axes). Button takes button-like controls; Axis those, whole axes and 1D composites; Vector2 sticks and 2D composites" },
						{ "items", { { "type", "string" }, { "maxLength", InputBinding::c_MaxTextBytes } } } } } } },
				{ "required", nlohmann::json::array({ "Name" }) },
				{ "additionalProperties", false } };
			return {
				{ "type", "object" },
				{ "properties", {
					{ "Maps", {
						{ "type", "array" },
						{ "maxItems", InputActionSet::c_MaxMaps },
						{ "description", "The game's action maps; at most 256 actions in all (the UI map's six included)" },
						{ "items", {
							{ "type", "object" },
							{ "properties", {
								{ "Name", mapName },
								{ "Enabled", { { "type", "boolean" }, { "description", "Whether its actions work when the game starts (default true)" } } },
								{ "Actions", { { "type", "array" }, { "items", action } } } } },
							{ "required", nlohmann::json::array({ "Name" }) },
							{ "additionalProperties", false } } } } } } },
				{ "required", nlohmann::json::array({ "Maps" }) },
				{ "additionalProperties", false } };
		}

		std::optional<InputAction> ReadAction(const nlohmann::json& value, const InputActionMap& map, std::string& error)
		{
			if (!value.is_object())
			{
				error = fmt::format("An action of map '{}' is not an object", map.Name);
				return std::nullopt;
			}
			InputAction action;
			const nlohmann::json* name = JsonUtils::Find(value, "Name");
			if (!name || !name->is_string())
			{
				error = fmt::format("An action of map '{}' has no Name", map.Name);
				return std::nullopt;
			}
			action.Name = name->get<std::string>();
			if (std::optional<std::string> problem = InputActionSet::ValidateName(action.Name))
			{
				error = fmt::format("Map '{}': action name '{}': {}", map.Name, StringUtils::MakeValidUTF8(action.Name), *problem);
				return std::nullopt;
			}
			for (const auto& [key, member] : value.items())
			{
				if (key != "Name" && key != "Kind" && key != "DeadZone" && key != "Bindings")
				{
					error = fmt::format("{}: '{}' is not a member of an action (Name, Kind, DeadZone, Bindings)", Describe(map, &action),
						StringUtils::MakeValidUTF8(key));
					return std::nullopt;
				}
			}
			if (const nlohmann::json* kind = JsonUtils::Find(value, "Kind"))
			{
				const std::optional<InputActionKind> parsed = kind->is_string() ? FindInputActionKind(kind->get<std::string>()) : std::nullopt;
				if (!parsed)
				{
					error = fmt::format("{}: Kind must be Button, Axis or Vector2", Describe(map, &action));
					return std::nullopt;
				}
				action.Kind = *parsed;
			}
			if (const nlohmann::json* deadZone = JsonUtils::Find(value, "DeadZone"))
			{
				const double zone = deadZone->is_number() ? deadZone->get<double>() : -1.0;
				if (!(zone >= 0.0 && zone <= InputActionSet::c_MaxDeadZone))
				{
					error = fmt::format("{}: DeadZone must be a number from 0 to {}", Describe(map, &action), InputActionSet::c_MaxDeadZone);
					return std::nullopt;
				}
				action.DeadZone = static_cast<float>(zone);
			}
			if (const nlohmann::json* bindings = JsonUtils::Find(value, "Bindings"))
			{
				if (!bindings->is_array())
				{
					error = fmt::format("{}: Bindings must be an array of binding texts", Describe(map, &action));
					return std::nullopt;
				}
				if (bindings->size() > InputActionSet::c_MaxBindings)
				{
					error = fmt::format("{} has {} bindings (at most {})", Describe(map, &action), bindings->size(), InputActionSet::c_MaxBindings);
					return std::nullopt;
				}
				for (const nlohmann::json& text : *bindings)
				{
					if (!text.is_string())
					{
						error = fmt::format("{}: a binding is not a string", Describe(map, &action));
						return std::nullopt;
					}
					std::string problem;
					const std::optional<InputBinding> binding = InputBinding::Parse(text.get<std::string>(), &problem);
					if (!binding)
					{
						error = fmt::format("{}: binding '{}': {}", Describe(map, &action), StringUtils::MakeValidUTF8(text.get<std::string>()), problem);
						return std::nullopt;
					}
					if (!binding->FitsKind(action.Kind))
					{
						error = fmt::format("{}: {} action cannot use '{}'", Describe(map, &action), DescribeInputActionKind(action.Kind), binding->ToString());
						return std::nullopt;
					}
					action.Bindings.push_back(*binding);
				}
			}
			return action;
		}

		std::optional<InputActionMap> ReadMap(const nlohmann::json& value, size_t& actionCount, std::string& error)
		{
			if (!value.is_object())
			{
				error = "A map is not an object";
				return std::nullopt;
			}
			InputActionMap map;
			const nlohmann::json* name = JsonUtils::Find(value, "Name");
			if (!name || !name->is_string())
			{
				error = "A map has no Name";
				return std::nullopt;
			}
			map.Name = name->get<std::string>();
			if (std::optional<std::string> problem = InputActionSet::ValidateName(map.Name))
			{
				error = fmt::format("Map name '{}': {}", StringUtils::MakeValidUTF8(map.Name), *problem);
				return std::nullopt;
			}
			for (const auto& [key, member] : value.items())
			{
				if (key != "Name" && key != "Enabled" && key != "Actions")
				{
					error = fmt::format("{}: '{}' is not a member of a map (Name, Enabled, Actions)", Describe(map, nullptr), StringUtils::MakeValidUTF8(key));
					return std::nullopt;
				}
			}
			if (const nlohmann::json* enabled = JsonUtils::Find(value, "Enabled"))
			{
				if (!enabled->is_boolean())
				{
					error = fmt::format("{}: Enabled must be true or false", Describe(map, nullptr));
					return std::nullopt;
				}
				map.Enabled = enabled->get<bool>();
			}
			if (const nlohmann::json* actions = JsonUtils::Find(value, "Actions"))
			{
				if (!actions->is_array())
				{
					error = fmt::format("{}: Actions must be an array", Describe(map, nullptr));
					return std::nullopt;
				}
				for (const nlohmann::json& entry : *actions)
				{
					if (++actionCount > InputActionSet::c_MaxActions)
					{
						error = fmt::format("The set has more than {} actions", InputActionSet::c_MaxActions);
						return std::nullopt;
					}
					std::optional<InputAction> action = ReadAction(entry, map, error);
					if (!action)
						return std::nullopt;
					const bool taken = std::any_of(map.Actions.begin(), map.Actions.end(),
						[&action](const InputAction& other) { return StringUtils::EqualsIgnoreCase(other.Name, action->Name); });
					if (taken)
					{
						error = fmt::format("Map '{}' has two actions named '{}' (names are unique in their map, ignoring case)", map.Name, action->Name);
						return std::nullopt;
					}
					map.Actions.push_back(std::move(*action));
				}
			}
			return map;
		}

	}

	const char* InputActionKindToString(InputActionKind kind)
	{
		switch (kind)
		{
			case InputActionKind::Button: return "Button";
			case InputActionKind::Axis: return "Axis";
			case InputActionKind::Vector2: return "Vector2";
		}
		return "Unknown";
	}

	const char* DescribeInputActionKind(InputActionKind kind)
	{
		switch (kind)
		{
			case InputActionKind::Button: return "a Button";
			case InputActionKind::Axis: return "an Axis";
			case InputActionKind::Vector2: return "a Vector2";
		}
		return "an unknown";
	}

	std::optional<InputActionKind> FindInputActionKind(std::string_view name)
	{
		for (const InputActionKind kind : { InputActionKind::Button, InputActionKind::Axis, InputActionKind::Vector2 })
		{
			if (StringUtils::EqualsIgnoreCase(name, InputActionKindToString(kind)))
				return kind;
		}
		return std::nullopt;
	}

	std::optional<InputBinding> InputBinding::Parse(std::string_view text, std::string* outError)
	{
		std::string error;
		auto fail = [&error, outError]() -> std::optional<InputBinding>
		{
			if (outError)
				*outError = std::move(error);
			return std::nullopt;
		};
		if (text.empty())
		{
			error = "a binding is empty";
			return fail();
		}
		if (text.size() > c_MaxTextBytes)
		{
			error = fmt::format("a binding is at most {} bytes", c_MaxTextBytes);
			return fail();
		}

		InputBinding binding;
		const bool composite1D = StartsWithIgnoreCase(text, c_Composite1DPrefix);
		const bool composite2D = StartsWithIgnoreCase(text, c_Composite2DPrefix);
		if (!composite1D && !composite2D)
		{
			const std::optional<InputControl> control = ParseControl(text, error);
			if (!control)
				return fail();
			binding.Controls[0] = *control;
			return binding;
		}

		binding.Type = composite1D ? Shape::Composite1D : Shape::Composite2D;
		const uint32_t count = binding.GetControlCount();
		std::string_view parts = text.substr(c_Composite1DPrefix.size());
		for (uint32_t index = 0; index < count; index++)
		{
			const size_t comma = parts.find(',');
			const bool last = index + 1 == count;
			if (last != (comma == std::string_view::npos))
			{
				error = composite1D ? "a 1D composite has two parts: 1D:<negative>,<positive>" : "a 2D composite has four parts: 2D:<up>,<down>,<left>,<right>";
				return fail();
			}
			const std::string_view part = last ? parts : parts.substr(0, comma);
			const std::optional<InputControl> control = ParseControl(part, error);
			if (!control)
				return fail();
			if (!control->IsButtonLike())
			{
				error = fmt::format("'{}' cannot be part of a composite: parts are keys, mouse buttons, gamepad buttons or half axes", part);
				return fail();
			}
			binding.Controls[index] = *control;
			if (!last)
				parts = parts.substr(comma + 1);
		}
		return binding;
	}

	std::string InputBinding::ToString() const
	{
		if (Type == Shape::Control)
			return ControlToString(Controls[0]);
		std::string text(Type == Shape::Composite1D ? c_Composite1DPrefix : c_Composite2DPrefix);
		for (uint32_t index = 0; index < GetControlCount(); index++)
		{
			if (index > 0)
				text += ',';
			text += ControlToString(Controls[index]);
		}
		return text;
	}

	bool InputBinding::FitsKind(InputActionKind kind) const
	{
		const InputControl& control = Controls[0];
		switch (kind)
		{
			case InputActionKind::Button:
				return Type == Shape::Control && control.IsButtonLike();
			case InputActionKind::Axis:
				return Type == Shape::Composite1D || (Type == Shape::Control && (control.IsButtonLike() || control.Type == InputControlType::GamepadAxis));
			case InputActionKind::Vector2:
				return Type == Shape::Composite2D || (Type == Shape::Control && control.Type == InputControlType::GamepadStick);
		}
		return false;
	}

	InputActionSet InputActionSet::MakeDefault()
	{
		InputActionSet set;
		set.Maps.push_back({ c_UIMapName, true, GetDefaultUIActions() });
		return set;
	}

	const std::vector<InputAction>& InputActionSet::GetDefaultUIActions()
	{
		static const std::vector<InputAction> s_Actions = MakeDefaultUIActions();
		return s_Actions;
	}

	std::optional<InputActionSet> InputActionSet::FromJson(const nlohmann::json& value, std::string* outError)
	{
		std::string error;
		auto fail = [&error, outError]() -> std::optional<InputActionSet>
		{
			if (outError)
				*outError = std::move(error);
			return std::nullopt;
		};
		if (!value.is_object())
		{
			error = "Input actions are an object with Maps";
			return fail();
		}
		for (const auto& [key, member] : value.items())
		{
			if (key != "Maps")
			{
				error = fmt::format("'{}' is not a member of the input actions (they have Maps)", StringUtils::MakeValidUTF8(key));
				return fail();
			}
		}
		const nlohmann::json* maps = JsonUtils::Find(value, "Maps");
		if (!maps || !maps->is_array())
		{
			error = "Input actions have Maps, an array of maps";
			return fail();
		}
		if (maps->size() > c_MaxMaps)
		{
			error = fmt::format("The set has {} maps (at most {})", maps->size(), c_MaxMaps);
			return fail();
		}

		InputActionSet set;
		size_t actionCount = 0;
		for (const nlohmann::json& entry : *maps)
		{
			std::optional<InputActionMap> map = ReadMap(entry, actionCount, error);
			if (!map)
				return fail();
			if (set.FindMap(map->Name))
			{
				error = fmt::format("Two maps are named '{}' (names are unique in the set, ignoring case)", map->Name);
				return fail();
			}
			set.Maps.push_back(std::move(*map));
		}

		// The reserved UI map: added with its defaults where missing, its actions kept Buttons named as the game UI reads them.
		std::optional<uint32_t> ui = set.FindMap(c_UIMapName);
		if (!ui)
		{
			if (set.Maps.size() >= c_MaxMaps)
			{
				error = fmt::format("The set has {} maps and no room for the reserved map UI (at most {} maps)", set.Maps.size(), c_MaxMaps);
				return fail();
			}
			set.Maps.insert(set.Maps.begin(), InputActionMap { c_UIMapName, true, {} });
			ui = 0;
		}
		InputActionMap& uiMap = set.Maps[*ui];
		uiMap.Name = c_UIMapName;
		for (const InputAction& reserved : GetDefaultUIActions())
		{
			auto found = std::find_if(uiMap.Actions.begin(), uiMap.Actions.end(),
				[&reserved](const InputAction& action) { return StringUtils::EqualsIgnoreCase(action.Name, reserved.Name); });
			if (found == uiMap.Actions.end())
			{
				if (++actionCount > c_MaxActions)
				{
					error = fmt::format("The set has no room for the UI map's action {} (at most {} actions, the UI map's six included)", reserved.Name,
						c_MaxActions);
					return fail();
				}
				uiMap.Actions.push_back(reserved);
				continue;
			}
			if (found->Kind != InputActionKind::Button)
			{
				error = fmt::format("The UI map's action {} is a Button: the game UI navigates with it", reserved.Name);
				return fail();
			}
			found->Name = reserved.Name;
		}
		return set;
	}

	nlohmann::json InputActionSet::ToJson() const
	{
		nlohmann::json maps = nlohmann::json::array();
		for (const InputActionMap& map : Maps)
		{
			nlohmann::json actions = nlohmann::json::array();
			for (const InputAction& action : map.Actions)
			{
				nlohmann::json bindings = nlohmann::json::array();
				for (const InputBinding& binding : action.Bindings)
					bindings.push_back(binding.ToString());
				actions.push_back({ { "Name", action.Name }, { "Kind", InputActionKindToString(action.Kind) }, { "DeadZone", StoreDeadZone(action.DeadZone) },
					{ "Bindings", std::move(bindings) } });
			}
			maps.push_back({ { "Name", map.Name }, { "Enabled", map.Enabled }, { "Actions", std::move(actions) } });
		}
		return { { "Maps", std::move(maps) } };
	}

	SettingsSection InputActionSet::MakeSection()
	{
		SettingsSection section;
		section.Id = c_SectionId;
		section.Title = "Input Actions";
		section.Category = "Input";
		section.Description = "The game's input actions: maps of named actions (Button, Axis or Vector2), each with the keys, mouse buttons and gamepad "
			"controls bound to it. Scripts read actions by name and players can rebind them; the map UI holds the actions the game UI navigates with. "
			"Edit them with inputActions.setMap and inputActions.setAction.";
		section.Schema = MakeSchema();
		section.MakeDefault = []() { return MakeDefault().ToJson(); };
		section.Read = [](const nlohmann::json& value, uint32_t, std::string* outError) -> std::optional<nlohmann::json>
		{
			const std::optional<InputActionSet> set = FromJson(value, outError);
			if (!set)
				return std::nullopt;
			return set->ToJson();
		};
		return section;
	}

	std::optional<std::string> InputActionSet::ValidateName(std::string_view name)
	{
		if (name.empty())
			return std::string("a name is empty");
		if (name.size() > c_MaxNameLength)
			return fmt::format("a name is at most {} characters", c_MaxNameLength);
		if (name.front() == ' ' || name.back() == ' ')
			return std::string("a name does not start or end with a space");
		for (const char character : name)
		{
			const bool valid = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9')
				|| character == ' ' || character == '_' || character == '-' || character == '.';
			if (!valid)
				return std::string("a name has only letters, digits, spaces, '_', '-' and '.'");
		}
		return std::nullopt;
	}

	bool InputActionSet::IsReservedAction(InputActionLocation location) const
	{
		if (location.Map >= Maps.size() || location.Action >= Maps[location.Map].Actions.size())
			return false;
		if (!StringUtils::EqualsIgnoreCase(Maps[location.Map].Name, c_UIMapName))
			return false;
		const std::string& name = Maps[location.Map].Actions[location.Action].Name;
		const std::vector<InputAction>& reserved = GetDefaultUIActions();
		return std::any_of(reserved.begin(), reserved.end(), [&name](const InputAction& action) { return StringUtils::EqualsIgnoreCase(action.Name, name); });
	}

	std::optional<uint32_t> InputActionSet::FindMap(std::string_view name) const
	{
		for (uint32_t index = 0; index < Maps.size(); index++)
		{
			if (StringUtils::EqualsIgnoreCase(Maps[index].Name, name))
				return index;
		}
		return std::nullopt;
	}

	std::optional<InputActionLocation> InputActionSet::FindAction(std::string_view name, std::string* outError) const
	{
		auto fail = [outError](std::string message) -> std::optional<InputActionLocation>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};
		if (name.empty())
			return fail("An action name is empty");

		const size_t slash = name.find('/');
		if (slash != std::string_view::npos)
		{
			const std::optional<uint32_t> map = FindMap(name.substr(0, slash));
			if (map)
			{
				const std::string_view actionName = name.substr(slash + 1);
				const std::vector<InputAction>& actions = Maps[*map].Actions;
				for (uint32_t index = 0; index < actions.size(); index++)
				{
					if (StringUtils::EqualsIgnoreCase(actions[index].Name, actionName))
						return InputActionLocation { *map, index };
				}
			}
			return fail(fmt::format("There is no action '{}'", StringUtils::MakeValidUTF8(name)));
		}

		// A bare name names the one action of that name in the whole set: a game's Left and the UI map's never mix.
		std::vector<InputActionLocation> matches;
		for (uint32_t map = 0; map < Maps.size(); map++)
		{
			for (uint32_t index = 0; index < Maps[map].Actions.size(); index++)
			{
				if (StringUtils::EqualsIgnoreCase(Maps[map].Actions[index].Name, name))
					matches.push_back({ map, index });
			}
		}
		if (matches.size() == 1)
			return matches.front();
		if (matches.empty())
			return fail(fmt::format("There is no action '{}'", StringUtils::MakeValidUTF8(name)));
		std::vector<std::string> qualified;
		for (const InputActionLocation& match : matches)
			qualified.push_back(GetQualifiedName(match));
		std::sort(qualified.begin(), qualified.end(), [](const std::string& a, const std::string& b) { return StringUtils::ToLower(a) < StringUtils::ToLower(b); });
		std::string list;
		for (const std::string& entry : qualified)
			list += fmt::format("{}{}", list.empty() ? "" : ", ", entry);
		return fail(fmt::format("'{}' is ambiguous: {}", name, list));
	}

	std::string InputActionSet::GetQualifiedName(InputActionLocation location) const
	{
		return fmt::format("{}/{}", Maps[location.Map].Name, Maps[location.Map].Actions[location.Action].Name);
	}

	size_t InputActionSet::GetActionCount() const
	{
		size_t count = 0;
		for (const InputActionMap& map : Maps)
			count += map.Actions.size();
		return count;
	}

	std::optional<InputPlayerBindings> InputPlayerBindings::Read(std::string_view text, const InputActionSet& set, std::vector<std::string>* outDropped,
		std::string* outError)
	{
		auto fail = [outError](std::string message) -> std::optional<InputPlayerBindings>
		{
			if (outError)
				*outError = std::move(message);
			return std::nullopt;
		};
		if (text.size() > c_MaxFileBytes)
			return fail(fmt::format("it is larger than {} bytes", c_MaxFileBytes));
		std::string parseError;
		const std::optional<nlohmann::json> document = JsonUtils::Parse(text, &parseError);
		if (!document)
			return fail(fmt::format("it is not JSON ({})", parseError));
		const nlohmann::json* header = JsonUtils::Find(*document, "Strata");
		if (!header || !header->is_object() || JsonUtils::GetString(*header, "Format") != "InputBindings")
			return fail("it is no input bindings file (no Strata.Format \"InputBindings\")");
		const nlohmann::json* version = JsonUtils::Find(*header, "Version");
		if (!version || !version->is_number_unsigned() || version->get<uint64_t>() == 0)
			return fail("its Strata.Version is not a version");
		if (version->get<uint64_t>() > c_FormatVersion)
			return fail(fmt::format("it was written by a later version of the engine (format {}; this one reads {})", version->get<uint64_t>(), c_FormatVersion));
		const nlohmann::json* actions = JsonUtils::Find(*document, "Actions");
		if (!actions || !actions->is_object())
			return fail("it has no Actions object");

		InputPlayerBindings bindings;
		auto drop = [outDropped](std::string reason)
		{
			if (outDropped)
				outDropped->push_back(std::move(reason));
		};
		for (const auto& [key, list] : actions->items())
		{
			const std::string name = StringUtils::MakeValidUTF8(key);
			if (key.find('/') == std::string::npos)
			{
				drop(fmt::format("'{}' is not a map-qualified action name", name));
				continue;
			}
			std::string error;
			const std::optional<InputActionLocation> location = set.FindAction(key, &error);
			if (!location)
			{
				drop(fmt::format("'{}': {}", name, error));
				continue;
			}
			const InputAction& action = set.GetAction(*location);
			if (!list.is_array() || list.size() > InputActionSet::c_MaxBindings)
			{
				drop(fmt::format("'{}' is not a list of at most {} bindings", name, InputActionSet::c_MaxBindings));
				continue;
			}
			std::vector<InputBinding> parsed;
			bool droppedOne = false;
			for (const nlohmann::json& entry : list)
			{
				std::optional<InputBinding> binding = entry.is_string() ? InputBinding::Parse(entry.get<std::string>(), &error) : std::nullopt;
				if (binding && binding->FitsKind(action.Kind))
				{
					parsed.push_back(*binding);
					continue;
				}
				droppedOne = true;
				drop(fmt::format("'{}': binding {} is not one {} action can use", name, entry.is_string() ? StringUtils::MakeValidUTF8(entry.get<std::string>()) : JsonUtils::Dump(entry),
					DescribeInputActionKind(action.Kind)));
			}
			// A list emptied by dropping is no player's choice: the project's bindings apply. (An empty list in the file is one: the
			// player unbound the action.)
			if (droppedOne && parsed.empty())
				continue;
			bindings.Actions[set.GetQualifiedName(*location)] = std::move(parsed);
		}
		return bindings;
	}

	std::string InputPlayerBindings::Write() const
	{
		nlohmann::json actions = nlohmann::json::object();
		for (const auto& [name, list] : Actions)
		{
			nlohmann::json texts = nlohmann::json::array();
			for (const InputBinding& binding : list)
				texts.push_back(binding.ToString());
			actions[name] = std::move(texts);
		}
		const nlohmann::json document = {
			{ "Strata", { { "Format", "InputBindings" }, { "Version", c_FormatVersion } } },
			{ "Actions", std::move(actions) } };
		return JsonUtils::Dump(document, 1, '\t') + "\n";
	}

}
