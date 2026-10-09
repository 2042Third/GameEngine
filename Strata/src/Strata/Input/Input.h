#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Input/GamepadCodes.h"
#include "Strata/Input/KeyCodes.h"
#include "Strata/Input/MouseCodes.h"

#include <glm/glm.hpp>

namespace Strata
{

	class Window;

	// Polling-style input state, updated from window events once per frame. Main thread only.
	//
	// "Down" queries report the current state; "Pressed"/"Released" report transitions that happened
	// during the current frame (a press and release within one frame reports both). While input is
	// disabled (e.g. the editor viewport is not focused) every query reports nothing.
	class Input
	{
	public:
		static bool IsKeyDown(KeyCode key);
		static bool IsKeyPressed(KeyCode key);
		static bool IsKeyReleased(KeyCode key);

		static bool IsMouseButtonDown(MouseCode button);
		static bool IsMouseButtonPressed(MouseCode button);
		static bool IsMouseButtonReleased(MouseCode button);
		// Pixels relative to the top-left corner of the input viewport (the whole window by default).
		static glm::vec2 GetMousePosition();
		static glm::vec2 GetMouseDelta();
		static glm::vec2 GetScrollDelta();

		static bool IsGamepadConnected(uint32_t gamepad);
		static bool IsGamepadButtonDown(uint32_t gamepad, GamepadButton button);
		static bool IsGamepadButtonPressed(uint32_t gamepad, GamepadButton button);
		static float GetGamepadAxis(uint32_t gamepad, GamepadAxis axis);

		static void SetCursorMode(CursorMode mode);
		static CursorMode GetCursorMode();

		static glm::vec2 GetViewportSize();

		//////////////////////////////////////////////////////////////////////////
		// Engine integration (application, window backends, editor, automation)
		//////////////////////////////////////////////////////////////////////////

		// Starts a new input frame: clears per-frame transitions and deltas.
		static void BeginFrame();
		static void Reset();

		static void SetEnabled(bool enabled);
		static bool IsEnabled();
		static void SetWindow(Window* window);
		// Region of the window that receives game input, in window pixels.
		static void SetViewport(const glm::vec2& origin, const glm::vec2& size);

		static void ProcessKey(KeyCode key, bool down);
		static void ProcessMouseButton(MouseCode button, bool down);
		static void ProcessMouseMove(const glm::vec2& windowPosition);
		static void ProcessScroll(const glm::vec2& offset);
		static void ProcessGamepad(uint32_t gamepad, bool connected, const float* axes, const bool* buttons);
	};

}
