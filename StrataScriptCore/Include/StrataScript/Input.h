#pragma once

#include "StrataScript/Host.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	using KeyCode = uint16_t;
	using MouseCode = uint16_t;

	// Key codes (US keyboard layout). The values are part of the script ABI and match the engine's.
	namespace Key
	{
		enum : KeyCode
		{
			Space = 32,
			Apostrophe = 39,
			Comma = 44,
			Minus = 45,
			Period = 46,
			Slash = 47,

			D0 = 48,
			D1 = 49,
			D2 = 50,
			D3 = 51,
			D4 = 52,
			D5 = 53,
			D6 = 54,
			D7 = 55,
			D8 = 56,
			D9 = 57,

			Semicolon = 59,
			Equal = 61,

			A = 65,
			B = 66,
			C = 67,
			D = 68,
			E = 69,
			F = 70,
			G = 71,
			H = 72,
			I = 73,
			J = 74,
			K = 75,
			L = 76,
			M = 77,
			N = 78,
			O = 79,
			P = 80,
			Q = 81,
			R = 82,
			S = 83,
			T = 84,
			U = 85,
			V = 86,
			W = 87,
			X = 88,
			Y = 89,
			Z = 90,

			LeftBracket = 91,
			Backslash = 92,
			RightBracket = 93,
			GraveAccent = 96,

			World1 = 161,
			World2 = 162,

			Escape = 256,
			Enter = 257,
			Tab = 258,
			Backspace = 259,
			Insert = 260,
			Delete = 261,
			Right = 262,
			Left = 263,
			Down = 264,
			Up = 265,
			PageUp = 266,
			PageDown = 267,
			Home = 268,
			End = 269,
			CapsLock = 280,
			ScrollLock = 281,
			NumLock = 282,
			PrintScreen = 283,
			Pause = 284,
			F1 = 290,
			F2 = 291,
			F3 = 292,
			F4 = 293,
			F5 = 294,
			F6 = 295,
			F7 = 296,
			F8 = 297,
			F9 = 298,
			F10 = 299,
			F11 = 300,
			F12 = 301,
			F13 = 302,
			F14 = 303,
			F15 = 304,
			F16 = 305,
			F17 = 306,
			F18 = 307,
			F19 = 308,
			F20 = 309,
			F21 = 310,
			F22 = 311,
			F23 = 312,
			F24 = 313,
			F25 = 314,

			KP0 = 320,
			KP1 = 321,
			KP2 = 322,
			KP3 = 323,
			KP4 = 324,
			KP5 = 325,
			KP6 = 326,
			KP7 = 327,
			KP8 = 328,
			KP9 = 329,
			KPDecimal = 330,
			KPDivide = 331,
			KPMultiply = 332,
			KPSubtract = 333,
			KPAdd = 334,
			KPEnter = 335,
			KPEqual = 336,

			LeftShift = 340,
			LeftControl = 341,
			LeftAlt = 342,
			LeftSuper = 343,
			RightShift = 344,
			RightControl = 345,
			RightAlt = 346,
			RightSuper = 347,
			Menu = 348
		};
	}

	namespace Mouse
	{
		enum : MouseCode
		{
			Button0 = 0,
			Button1 = 1,
			Button2 = 2,
			Button3 = 3,
			Button4 = 4,
			Button5 = 5,
			Button6 = 6,
			Button7 = 7,

			ButtonLeft = Button0,
			ButtonRight = Button1,
			ButtonMiddle = Button2
		};
	}

	// Gamepad buttons and axes: the GLFW gamepad mapping, an Xbox-style layout (on other pads, the button in the same place).
	// The values are part of the script ABI and match the engine's.
	enum class GamepadButton : uint32_t
	{
		A = 0,
		B = 1,
		X = 2,
		Y = 3,
		LeftBumper = 4,
		RightBumper = 5,
		Back = 6,
		Start = 7,
		Guide = 8,
		LeftThumb = 9,
		RightThumb = 10,
		DPadUp = 11,
		DPadRight = 12,
		DPadDown = 13,
		DPadLeft = 14
	};

	// Sticks range from -1 to 1 with +X to the right and +Y up; triggers from 0 (released) to 1 (fully pressed).
	enum class GamepadAxis : uint32_t
	{
		LeftX = 0,
		LeftY = 1,
		RightX = 2,
		RightY = 3,
		LeftTrigger = 4,
		RightTrigger = 5
	};

	enum class GamepadStick : uint32_t
	{
		Left = 0,
		Right = 1
	};

	enum class CursorMode : uint32_t
	{
		Normal = StrataScriptCursorMode_Normal,
		Hidden = StrataScriptCursorMode_Hidden, // Hidden over the game's window
		Locked = StrataScriptCursorMode_Locked  // Hidden and kept in the window; GetMouseDelta keeps reporting movement (mouse look)
	};

	// The devices a binding capture listens to (Input::BeginBindingCapture). The values are the ABI's bits: other
	// combinations are static_cast<InputDevices>(bits).
	enum class InputDevices : uint32_t
	{
		Keyboard = StrataScriptInputDevices_Keyboard,
		Mouse = StrataScriptInputDevices_Mouse,
		Gamepad = StrataScriptInputDevices_Gamepad,
		KeyboardAndMouse = StrataScriptInputDevices_Keyboard | StrataScriptInputDevices_Mouse,
		All = StrataScriptInputDevices_Keyboard | StrataScriptInputDevices_Mouse | StrataScriptInputDevices_Gamepad
	};

	// Keyboard, mouse and gamepad state of the current frame. "Down" is the current state; "Pressed"/"Released" report a
	// transition during this frame. While the game view has no input focus every query reports nothing.
	class Input
	{
	public:
		// Stick and trigger values below this are noise of a stick at rest (GetGamepadAxis, GetGamepadStick).
		static constexpr float c_DefaultDeadZone = 0.15f;

		static bool IsKeyDown(KeyCode key)
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host && host->IsKeyDown(Detail::GetContext(), key);
		}

		static bool IsKeyPressed(KeyCode key)
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host && host->IsKeyPressed(Detail::GetContext(), key);
		}

		static bool IsKeyReleased(KeyCode key)
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host && host->IsKeyReleased(Detail::GetContext(), key);
		}

		static bool IsMouseButtonDown(MouseCode button)
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host && host->IsMouseButtonDown(Detail::GetContext(), button);
		}

		static bool IsMouseButtonPressed(MouseCode button)
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host && host->IsMouseButtonPressed(Detail::GetContext(), button);
		}

		static bool IsMouseButtonReleased(MouseCode button)
		{
			const StrataScriptHostAPI* host = Detail::GetHost();
			return host && host->IsMouseButtonReleased(Detail::GetContext(), button);
		}

		// Pixels relative to the top-left corner of the game viewport.
		static glm::vec2 GetMousePosition()
		{
			float position[2] = { 0.0f, 0.0f };
			if (const StrataScriptHostAPI* host = Detail::GetHost())
				host->GetMousePosition(Detail::GetContext(), position);
			return glm::vec2(position[0], position[1]);
		}

		// Mouse movement in pixels during this frame.
		static glm::vec2 GetMouseDelta()
		{
			float delta[2] = { 0.0f, 0.0f };
			if (const StrataScriptHostAPI* host = Detail::GetHost())
				host->GetMouseDelta(Detail::GetContext(), delta);
			return glm::vec2(delta[0], delta[1]);
		}

		// Scroll wheel movement during this frame.
		static glm::vec2 GetScrollDelta()
		{
			float delta[2] = { 0.0f, 0.0f };
			if (const StrataScriptHostAPI* host = Detail::GetHost())
				host->GetScrollDelta(Detail::GetContext(), delta);
			return glm::vec2(delta[0], delta[1]);
		}

		// Gamepads are numbered 0 to 3, in the order they were connected.
		static bool IsGamepadConnected(uint32_t gamepad = 0)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(IsGamepadConnected);
			return host && host->IsGamepadConnected(Detail::GetContext(), gamepad);
		}

		static bool IsGamepadButtonDown(GamepadButton button, uint32_t gamepad = 0)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(IsGamepadButtonDown);
			return host && host->IsGamepadButtonDown(Detail::GetContext(), gamepad, static_cast<uint32_t>(button));
		}

		static bool IsGamepadButtonPressed(GamepadButton button, uint32_t gamepad = 0)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(IsGamepadButtonPressed);
			return host && host->IsGamepadButtonPressed(Detail::GetContext(), gamepad, static_cast<uint32_t>(button));
		}

		static bool IsGamepadButtonReleased(GamepadButton button, uint32_t gamepad = 0)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(IsGamepadButtonReleased);
			return host && host->IsGamepadButtonReleased(Detail::GetContext(), gamepad, static_cast<uint32_t>(button));
		}

		// An axis with a dead zone: values within it read 0, the rest is rescaled so that the axis still reaches 1 (a dead zone
		// of 0 reads the device's value).
		static float GetGamepadAxis(GamepadAxis axis, uint32_t gamepad = 0, float deadZone = c_DefaultDeadZone)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GetGamepadAxis);
			const float value = host ? host->GetGamepadAxis(Detail::GetContext(), gamepad, static_cast<uint32_t>(axis)) : 0.0f;
			const float magnitude = value < 0.0f ? -value : value;
			if (!(deadZone > 0.0f))
				return value;
			if (magnitude <= deadZone || deadZone >= 1.0f)
				return 0.0f;
			const float scaled = (magnitude - deadZone) / (1.0f - deadZone);
			return value < 0.0f ? -scaled : scaled;
		}

		// A stick as a vector (+X right, +Y up) with a round dead zone: small tilts in any direction read zero, diagonals keep
		// their direction, and the length reaches 1 at full tilt (it never exceeds 1). What character movement wants.
		static glm::vec2 GetGamepadStick(GamepadStick stick, uint32_t gamepad = 0, float deadZone = c_DefaultDeadZone)
		{
			const bool left = stick == GamepadStick::Left;
			const glm::vec2 value(GetGamepadAxis(left ? GamepadAxis::LeftX : GamepadAxis::RightX, gamepad, 0.0f),
				GetGamepadAxis(left ? GamepadAxis::LeftY : GamepadAxis::RightY, gamepad, 0.0f));
			const float length = glm::length(value);
			if (!(length > deadZone) || deadZone >= 1.0f)
				return glm::vec2(0.0f);
			const float scaled = glm::min((length - glm::max(deadZone, 0.0f)) / (1.0f - glm::max(deadZone, 0.0f)), 1.0f);
			return value / length * scaled;
		}

		// Hides the cursor or locks it to the game (first-person cameras read GetMouseDelta). It applies while the game has the
		// input: in the editor while the game view has the focus (Shift+F1 frees it, clicking the game view takes it again,
		// stopping play restores Normal).
		static void SetCursorMode(CursorMode mode)
		{
			if (const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(SetCursorMode))
				host->SetCursorMode(Detail::GetContext(), static_cast<uint32_t>(mode));
		}

		static CursorMode GetCursorMode()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GetCursorMode);
			return host ? static_cast<CursorMode>(host->GetCursorMode(Detail::GetContext())) : CursorMode::Normal;
		}

		// The text typed during this frame, as UTF-8 (keyboard layouts and input methods applied; at most 1,024 bytes): what
		// a name entry or a chat box appends. Empty while the game has no input focus.
		static std::string GetTypedText()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(GetTypedText);
			if (!host)
				return {};
			return Detail::ReadHostString([&](char* buffer, uint64_t capacity) { return host->GetTypedText(Detail::GetContext(), buffer, capacity); });
		}

		// Input actions: the project's named controls (Project Settings > Input Actions), which players can rebind. Name an
		// action "<Map>/<Action>" ("Gameplay/Jump"), or by its bare name ("Jump") when no other map has an action of that name;
		// unknown and ambiguous names read as up and the engine logs a warning. Read actions rather than keys wherever a player
		// could want another control. The state is the current frame's, like keys': read Pressed and Released in OnUpdate.
		static bool IsActionDown(std::string_view action)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(InputIsActionDown);
			return host && host->InputIsActionDown(Detail::GetContext(), Detail::ToABIString(action));
		}

		static bool IsActionPressed(std::string_view action)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(InputIsActionPressed);
			return host && host->InputIsActionPressed(Detail::GetContext(), Detail::ToABIString(action));
		}

		static bool IsActionReleased(std::string_view action)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(InputIsActionReleased);
			return host && host->InputIsActionReleased(Detail::GetContext(), Detail::ToABIString(action));
		}

		// A Vector2 action's vector (+X right, +Y up, length at most 1, its dead zone applied); an Axis action's value in x (-1
		// to 1); a Button action's x is 1 while it is down.
		static glm::vec2 GetActionValue(std::string_view action)
		{
			float value[2] = { 0.0f, 0.0f };
			if (const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(InputGetActionValue))
				host->InputGetActionValue(Detail::GetContext(), Detail::ToABIString(action), value);
			return glm::vec2(value[0], value[1]);
		}

		// An Axis action's value (-1 to 1): the x of GetActionValue.
		static float GetActionAxis(std::string_view action)
		{
			return GetActionValue(action).x;
		}

		// A disabled map's actions read as up (a vehicle's controls while walking). The change applies from the next input
		// frame on, where an action held when its map was disabled reports IsActionReleased once, as a key let go would. Maps
		// start as the project says, in every session. False for a map the project does not have.
		static bool SetActionMapEnabled(std::string_view map, bool enabled)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(InputSetActionMapEnabled);
			return host && host->InputSetActionMapEnabled(Detail::GetContext(), Detail::ToABIString(map), enabled);
		}

		// Nothing for a map the project does not have.
		static std::optional<bool> IsActionMapEnabled(std::string_view map)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(InputIsActionMapEnabled);
			const int32_t enabled = host ? host->InputIsActionMapEnabled(Detail::GetContext(), Detail::ToABIString(map)) : -1;
			if (enabled < 0)
				return std::nullopt;
			return enabled != 0;
		}

		// The bindings an action uses ("Key/Space", "Gamepad/A", "2D:Key/W,Key/S,Key/A,Key/D"...): the players' when they
		// rebound it, else the project's. A rebinding menu shows them.
		static std::vector<std::string> GetActionBindings(std::string_view action)
		{
			std::vector<std::string> bindings;
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(InputGetActionBindings);
			if (!host)
				return bindings;
			const std::string text = Detail::ReadHostString([&](char* buffer, uint64_t capacity)
			{
				return host->InputGetActionBindings(Detail::GetContext(), Detail::ToABIString(action), buffer, capacity);
			});
			size_t start = 0;
			for (size_t end = text.find('\n'); end != std::string::npos; end = text.find('\n', start))
			{
				bindings.push_back(text.substr(start, end - start));
				start = end + 1;
			}
			return bindings;
		}

		// Replaces an action's bindings for the players (at most 8, in the text GetActionBindings returns; none unbinds it),
		// from the next frame on. False, changing nothing, when one is no binding or one the action's kind cannot use (a Button
		// takes keys, mouse and gamepad buttons and half axes such as "Gamepad/LeftX+"; an Axis also whole axes and
		// "1D:<negative>,<positive>"; a Vector2 sticks and "2D:<up>,<down>,<left>,<right>"). Players keep them for the session;
		// SaveBindings keeps them for the next.
		static bool SetActionBindings(std::string_view action, const std::vector<std::string>& bindings)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(InputSetActionBindings);
			if (!host)
				return false;
			std::string text;
			for (const std::string& binding : bindings)
			{
				// One binding per line: a line break inside one would make two.
				if (binding.find('\n') != std::string::npos)
					return false;
				text += binding;
				text += '\n';
			}
			return host->InputSetActionBindings(Detail::GetContext(), Detail::ToABIString(action), Detail::ToABIString(text));
		}

		// Restores the project's bindings of an action, or of every action without a name (a menu's "Reset to defaults").
		static bool ResetActionBindings(std::string_view action = {})
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(InputResetActionBindings);
			return host && host->InputResetActionBindings(Detail::GetContext(), Detail::ToABIString(action));
		}

		// Keeps the players' bindings for later sessions (a file of the engine's in the game's save directory, never among the
		// save files): call it when a rebinding menu closes. False when the game has no save directory or the write failed.
		static bool SaveBindings()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(InputSaveBindings);
			return host && host->InputSaveBindings(Detail::GetContext());
		}

		// A rebinding menu's "press a control": listens for the next control the player presses on the devices given (a key, a
		// mouse button, a gamepad button, or a stick or trigger pushed beyond half its range). While it listens every action
		// reads as up, and the control captured reads as up for actions until it is let go, so the press triggers nothing (not
		// even the menu's Cancel). There is one capture: a later one replaces it, and play stopping cancels it. False when the
		// engine lacks it.
		static bool BeginBindingCapture(InputDevices devices = InputDevices::All)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(InputBeginBindingCapture);
			return host && host->InputBeginBindingCapture(Detail::GetContext(), static_cast<uint32_t>(devices));
		}

		// The captured control's binding ("Key/K", "Mouse/Right", "Gamepad/LeftX+"), once: the capture is then over, and
		// SetActionBindings takes the text. Nothing while it listens, after it was cancelled or replaced, or when none runs:
		// poll it every frame until a binding comes (and give up after a while with CancelBindingCapture).
		static std::optional<std::string> PollBindingCapture()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(InputPollBindingCapture);
			if (!host)
				return std::nullopt;
			std::string binding = Detail::ReadHostString([&](char* buffer, uint64_t capacity)
			{
				return host->InputPollBindingCapture(Detail::GetContext(), buffer, capacity);
			});
			if (binding.empty())
				return std::nullopt;
			return binding;
		}

		// Ends the capture BeginBindingCapture started (a menu's timeout or its Cancel button). False when it was not listening.
		static bool CancelBindingCapture()
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(InputCancelBindingCapture);
			return host && host->InputCancelBindingCapture(Detail::GetContext());
		}
	};

}
