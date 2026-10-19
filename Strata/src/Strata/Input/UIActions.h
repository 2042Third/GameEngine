#pragma once

#include "Strata/Input/GamepadCodes.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace Strata
{

	// The fixed actions the game UI reads (focus navigation, clicking, going back) from the keyboard and every gamepad.
	// Rebinding is not supported yet: the map below is written as data in the shape an input-actions asset will have.
	enum class UIAction : uint8_t
	{
		Submit = 0,
		Cancel,
		Up,
		Down,
		Left,
		Right
	};
	constexpr uint32_t c_UIActionCount = 6;

	const char* GetUIActionName(UIAction action);

	// An action and the controls bound to it: "Key/<key name>" (InputNames), "Gamepad/<button name>" on any gamepad (A is the
	// bottom face button, B the right one), and "Gamepad/<axis name>+" or "-" for a stick pushed that way (beyond
	// UIActionState::c_StickPress, released below c_StickRelease; at most one stick binding per action).
	struct UIActionBinding
	{
		std::string_view Action;
		std::array<std::string_view, 4> Bindings;
	};

	inline constexpr std::array<UIActionBinding, c_UIActionCount> c_UIActionMap = { {
		{ "Submit", { "Key/Enter", "Key/KPEnter", "Key/Space", "Gamepad/A" } },
		{ "Cancel", { "Key/Escape", "Gamepad/B", "", "" } },
		{ "Up", { "Key/Up", "Gamepad/DPadUp", "Gamepad/LeftY+", "" } },
		{ "Down", { "Key/Down", "Gamepad/DPadDown", "Gamepad/LeftY-", "" } },
		{ "Left", { "Key/Left", "Gamepad/DPadLeft", "Gamepad/LeftX-", "" } },
		{ "Right", { "Key/Right", "Gamepad/DPadRight", "Gamepad/LeftX+", "" } },
	} };

	// The UI actions of the current input frame, read from Input through c_UIActionMap once per update: down, pressed
	// (this update) and repeated (pressed, then again after c_RepeatDelay seconds held and every c_RepeatInterval after that).
	// Deterministic for given input and deltas (the game UI passes unscaled time, so menus repeat at time scale 0). Main
	// thread only.
	class UIActionState
	{
	public:
		static constexpr float c_RepeatDelay = 0.4f;
		static constexpr float c_RepeatInterval = 0.1f;
		static constexpr float c_StickPress = 0.5f;
		static constexpr float c_StickRelease = 0.35f;

		void Update(float deltaSeconds);
		void Reset();

		bool IsDown(UIAction action) const { return m_Actions[Index(action)].Down; }
		bool IsPressed(UIAction action) const { return m_Actions[Index(action)].Pressed; }
		bool IsRepeated(UIAction action) const { return m_Actions[Index(action)].Repeated; }
	private:
		struct ActionRecord
		{
			bool Down = false;
			bool Pressed = false;
			bool Repeated = false;
			float HeldTime = 0.0f;
			float NextRepeat = 0.0f;
		};

		static size_t Index(UIAction action) { return static_cast<size_t>(action); }
	private:
		std::array<ActionRecord, c_UIActionCount> m_Actions = {};
		// Per gamepad and action: whether its stick binding holds it (the hysteresis; an action has at most one).
		std::array<std::array<bool, c_UIActionCount>, c_MaxGamepads> m_StickHeld = {};
	};

}
