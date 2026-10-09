#pragma once

#include <cstdint>

namespace Strata
{

	// Values match the GLFW gamepad mapping (Xbox-style layout).
	enum class GamepadButton : uint8_t
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

	enum class GamepadAxis : uint8_t
	{
		LeftX = 0,
		LeftY = 1,
		RightX = 2,
		RightY = 3,
		LeftTrigger = 4,
		RightTrigger = 5
	};

	constexpr uint32_t c_MaxGamepads = 4;
	constexpr uint32_t c_GamepadButtonCount = 15;
	constexpr uint32_t c_GamepadAxisCount = 6;

}
