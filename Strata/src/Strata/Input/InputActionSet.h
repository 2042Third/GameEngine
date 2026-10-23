#pragma once

#include "Strata/Input/GamepadCodes.h"
#include "Strata/Input/KeyCodes.h"
#include "Strata/Input/MouseCodes.h"

#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Input actions: named controls a game reads instead of fixed keys, so that players can rebind them. A game has one set of
// actions, the project settings section Input.Actions (maps of actions, each with its bindings), which InputActions
// (Strata/Input/InputActions.h) evaluates once per input frame. Docs/DeveloperGuide.md, "Input actions".
namespace Strata
{

	struct SettingsSection;

	// What an action's value is.
	enum class InputActionKind : uint8_t
	{
		Button = 0, // Down or up
		Axis,       // One value from -1 to 1
		Vector2     // Two values (+X right, +Y up), at most 1 long
	};

	const char* InputActionKindToString(InputActionKind kind);
	// "a Button", "an Axis", "a Vector2": for messages.
	const char* DescribeInputActionKind(InputActionKind kind);
	// Ignores case.
	std::optional<InputActionKind> FindInputActionKind(std::string_view name);

	enum class InputControlType : uint8_t
	{
		Key = 0,         // Code: KeyCode
		MouseButton,     // Code: MouseCode
		GamepadButton,   // Code: GamepadButton, on any gamepad
		GamepadHalfAxis, // Code: GamepadAxis, Sign: the half (+1 or -1; triggers only +1)
		GamepadAxis,     // Code: GamepadAxis, the whole axis
		GamepadStick     // Code: 0 the left stick, 1 the right one (two axes, +Y up)
	};

	// A control a binding reads.
	struct InputControl
	{
		InputControlType Type = InputControlType::Key;
		uint16_t Code = 0;
		int8_t Sign = 1;

		// Keys, mouse buttons, gamepad buttons and half axes: what Button actions and the parts of composites take.
		bool IsButtonLike() const { return Type != InputControlType::GamepadAxis && Type != InputControlType::GamepadStick; }

		bool operator==(const InputControl&) const = default;
	};

	// A binding of an action, written as text in the project file, commands, scripts and the players' bindings file:
	//   Key/<key>, Mouse/<button>, Gamepad/<button>      (InputNames' names; a gamepad binding reads every gamepad)
	//   Gamepad/<axis>                                   (the whole axis, e.g. Gamepad/LeftX)
	//   Gamepad/<axis>+ or Gamepad/<axis>-               (half an axis; triggers have only +)
	//   Gamepad/LeftStick, Gamepad/RightStick            (both axes of a stick, +Y up)
	//   1D:<negative>,<positive>                         (an axis from two button-like controls)
	//   2D:<up>,<down>,<left>,<right>                    (a vector from four button-like controls)
	// Names and prefixes ignore case when parsed; ToString writes them as above.
	struct InputBinding
	{
		enum class Shape : uint8_t
		{
			Control = 0,
			Composite1D,
			Composite2D
		};

		// The longest valid text is 90 bytes (a 2D composite of four Gamepad/RightTrigger+).
		static constexpr size_t c_MaxTextBytes = 96;

		Shape Type = Shape::Control;
		// Control: [0]; Composite1D: negative, positive; Composite2D: up, down, left, right.
		std::array<InputControl, 4> Controls = {};

		static std::optional<InputBinding> Parse(std::string_view text, std::string* outError = nullptr);
		std::string ToString() const;

		uint32_t GetControlCount() const { return Type == Shape::Control ? 1 : (Type == Shape::Composite1D ? 2 : 4); }
		// Button takes button-like controls; Axis those, whole axes and 1D composites; Vector2 sticks and 2D composites.
		bool FitsKind(InputActionKind kind) const;

		bool operator==(const InputBinding&) const = default;
	};

	struct InputAction
	{
		std::string Name;
		InputActionKind Kind = InputActionKind::Button;
		std::vector<InputBinding> Bindings;
		// Axis and Vector2: values whose magnitude is within it read 0, the rest is rescaled to reach 1 (round for vectors).
		float DeadZone = 0.15f;

		bool operator==(const InputAction&) const = default;
	};

	struct InputActionMap
	{
		std::string Name;
		bool Enabled = true; // When a game starts; scripts enable and disable maps while it runs
		std::vector<InputAction> Actions;

		bool operator==(const InputActionMap&) const = default;
	};

	// Where an action is in a set.
	struct InputActionLocation
	{
		uint32_t Map = 0;
		uint32_t Action = 0;

		bool operator==(const InputActionLocation&) const = default;
	};

	// A game's input actions: the value of the project settings section Input.Actions. Map names are unique in the set and
	// action names within their map (ignoring case); an action is named "<Map>/<Action>", or by its bare name when no other
	// map has an action of that name. Every set has the reserved map UI with the six actions the game UI navigates with
	// (Submit, Cancel, Up, Down, Left, Right: Buttons), whose bindings can change but not their names, kinds or map.
	struct InputActionSet
	{
		static constexpr const char* c_SectionId = "Input.Actions";
		static constexpr const char* c_UIMapName = "UI";
		static constexpr size_t c_MaxMaps = 32;
		static constexpr size_t c_MaxActions = 256; // In the whole set, the UI map's included
		static constexpr size_t c_MaxBindings = 8;  // Per action
		static constexpr size_t c_MaxNameLength = 64;
		static constexpr float c_DefaultDeadZone = 0.15f;
		static constexpr float c_MaxDeadZone = 0.9f;

		std::vector<InputActionMap> Maps;

		// The set of a project that defines none: the UI map with its default bindings.
		static InputActionSet MakeDefault();
		// The UI map's actions with their default bindings, in UIAction order.
		static const std::vector<InputAction>& GetDefaultUIActions();

		// The set a section value describes, checked whole (names, limits, kinds, bindings, dead zones, no unknown members),
		// with the UI map and its actions added where they are missing; nullopt with the reason otherwise.
		static std::optional<InputActionSet> FromJson(const nlohmann::json& value, std::string* outError = nullptr);
		nlohmann::json ToJson() const;
		// The section Input.Actions (RegisterInputModule registers it): Read is FromJson then ToJson.
		static SettingsSection MakeSection();

		// Why a map or action name is invalid (1 to 64 ASCII letters, digits, spaces, '_', '-' and '.', no leading or trailing
		// space), or nullopt.
		static std::optional<std::string> ValidateName(std::string_view name);
		// Whether a reserved action (the UI map's six) is at that location.
		bool IsReservedAction(InputActionLocation location) const;

		// Ignoring case.
		std::optional<uint32_t> FindMap(std::string_view name) const;
		// "<Map>/<Action>", or a bare name no other map has (ignoring case); nullopt with the reason ("no action 'Jump'",
		// "'Left' is ambiguous: Gameplay/Left, UI/Left") otherwise.
		std::optional<InputActionLocation> FindAction(std::string_view name, std::string* outError = nullptr) const;
		std::string GetQualifiedName(InputActionLocation location) const;
		const InputAction& GetAction(InputActionLocation location) const { return Maps[location.Map].Actions[location.Action]; }
		size_t GetActionCount() const;

		bool operator==(const InputActionSet&) const = default;
	};

	// The bindings players chose (rebinding menus): per action, by its qualified name, the list that replaces the project's.
	// Kept in <save directory>/InputBindings.json (InputActions::SaveBindings):
	// {"Strata": {"Format": "InputBindings", "Version": 1}, "Actions": {"Gameplay/Jump": ["Key/Space"]}}.
	struct InputPlayerBindings
	{
		static constexpr const char* c_FileName = "InputBindings.json";
		static constexpr uint64_t c_MaxFileBytes = 64 * 1024;
		static constexpr uint32_t c_FormatVersion = 1;

		// Keyed by qualified name as the set spells it.
		std::map<std::string, std::vector<InputBinding>> Actions;

		// Reads a file's text against the set: entries of actions the set does not have and bindings their action's kind
		// cannot use are dropped and described in outDropped (the game changed since they were saved), an entry left with no
		// binding by that is dropped too. Nullopt, with the reason, for text that is no bindings file at all.
		static std::optional<InputPlayerBindings> Read(std::string_view text, const InputActionSet& set, std::vector<std::string>* outDropped = nullptr,
			std::string* outError = nullptr);
		std::string Write() const;

		bool operator==(const InputPlayerBindings&) const = default;
	};

}
