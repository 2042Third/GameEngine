#pragma once

#include "StrataScript/Host.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

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
	};

}
