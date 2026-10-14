#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Input/GamepadCodes.h"
#include "Strata/Input/KeyCodes.h"
#include "Strata/Input/MouseCodes.h"

#include <glm/glm.hpp>

#include <optional>

namespace Strata
{

	class InputWindow;

	// Polling-style input state, updated from window events once per frame. Main thread only.
	//
	// "Down" queries report the current state; "Pressed"/"Released" report transitions that happened
	// during the current frame (a press and release within one frame reports both). While input is
	// disabled (e.g. the editor viewport is not focused) every query reports nothing from the devices;
	// simulated input (see SimulateKey) still comes through.
	class Input
	{
	public:
		static bool IsKeyDown(KeyCode key);
		static bool IsKeyPressed(KeyCode key);
		static bool IsKeyReleased(KeyCode key);

		static bool IsMouseButtonDown(MouseCode button);
		static bool IsMouseButtonPressed(MouseCode button);
		static bool IsMouseButtonReleased(MouseCode button);
		// Pixels relative to the top-left corner of the input viewport (the whole window by default); the simulated
		// pointer's position once a tool moved it (SimulateMouseMove).
		static glm::vec2 GetMousePosition();
		static glm::vec2 GetMouseDelta();
		static glm::vec2 GetScrollDelta();

		// A gamepad is connected while the device is (whether or not input is enabled) or while simulated input uses it.
		static bool IsGamepadConnected(uint32_t gamepad);
		static bool IsGamepadButtonDown(uint32_t gamepad, GamepadButton button);
		static bool IsGamepadButtonPressed(uint32_t gamepad, GamepadButton button);
		static bool IsGamepadButtonReleased(uint32_t gamepad, GamepadButton button);
		// The axis as the device (or simulated input) reports it, without a dead zone; ranges in GamepadCodes.h.
		static float GetGamepadAxis(uint32_t gamepad, GamepadAxis axis);

		// The cursor of the window Input reads, set directly (the editor's fly camera hides and confines it while flying).
		static void SetCursorMode(CursorMode mode);
		static CursorMode GetCursorMode();
		// The cursor mode the game asks for (scripts' Input::SetCursorMode). It applies to the window while input is enabled
		// (always in a game; in the editor while the focused game view has the input), and the window's cursor is Normal while
		// input is disabled, so a game that locked the cursor gives it back when it loses the input and takes it again when
		// it gets the input back. Reset makes it Normal again.
		static void SetGameCursorMode(CursorMode mode);
		static CursorMode GetGameCursorMode();

		static glm::vec2 GetViewportSize();

		//////////////////////////////////////////////////////////////////////////
		// Engine integration (application, window backends, editor, automation)
		//////////////////////////////////////////////////////////////////////////

		// Starts a new input frame: clears per-frame transitions and deltas and applies queued simulated events. While input
		// frames are suspended it only ends the frame the game has seen.
		static void BeginFrame();
		static void Reset();

		// Input frames follow the game's updates: while the game does not update (the editor's play mode is paused without a
		// pending step), suspend them, so that transitions, movement and queued simulated events wait for the game's next
		// update instead of passing unseen. Device events keep updating the state meanwhile and are reported in that update.
		static void SetSuspended(bool suspended);
		static bool IsSuspended();
		// Input frames started so far (BeginFrame calls while not suspended). Simulated input counts its frames in them.
		static uint64_t GetFrameIndex();

		static void SetEnabled(bool enabled);
		static bool IsEnabled();
		static void SetWindow(InputWindow* window);
		// Region of the window that receives game input, in window pixels, and the resolution the game renders it at (zero:
		// the region's size). Mouse positions and movements reach the game in pixels of that resolution (an editor's Game view
		// that shows a 1920 x 1080 image in a smaller panel).
		static void SetViewport(const glm::vec2& origin, const glm::vec2& size, const glm::vec2& resolution = glm::vec2(0.0f));
		// The resolution of SetViewport (its size when none was given).
		static glm::vec2 GetViewportResolution();

		static void ProcessKey(KeyCode key, bool down);
		static void ProcessMouseButton(MouseCode button, bool down);
		static void ProcessMouseMove(const glm::vec2& windowPosition);
		static void ProcessScroll(const glm::vec2& offset);
		static void ProcessGamepad(uint32_t gamepad, bool connected, const float* axes, const bool* buttons);

		//////////////////////////////////////////////////////////////////////////
		// Simulated input (editor automation, tests)
		//////////////////////////////////////////////////////////////////////////

		// A virtual keyboard and mouse for tools that play the game (the editor's input.* commands). Its events are
		// queued and apply at the start of the next input frame (BeginFrame), before that frame's updates, so every
		// transition is seen for one whole frame like a device event. Simulated input is merged with the devices': a key is
		// down while either holds it, and one source pressing or releasing a key the other held through the frame is no
		// transition. It reaches the game even while device input is disabled: a tool drives the game on purpose, whether
		// or not the editor's game view has the focus. Invalid codes and non-finite values are ignored.
		static void SimulateKey(KeyCode key, bool down);
		static void SimulateMouseButton(MouseCode button, bool down);
		// Moves the simulated pointer to a position relative to the top-left corner of the input viewport, in pixels.
		// From then on GetMousePosition reports it, and GetMouseDelta includes the moves after the first.
		static void SimulateMouseMove(const glm::vec2& viewportPosition);
		static void SimulateScroll(const glm::vec2& offset);
		// A virtual gamepad (the editor's input.gamepadButton and input.gamepadAxis): its button events merge with the device's
		// like keys, and an axis it holds replaces the device's value until ReleaseSimulatedGamepadAxis. A gamepad simulated
		// input used counts as connected until ClearSimulated. Axis values are clamped to the axis' range.
		static void SimulateGamepadButton(uint32_t gamepad, GamepadButton button, bool down);
		static void SimulateGamepadAxis(uint32_t gamepad, GamepadAxis axis, float value);
		static void ReleaseSimulatedGamepadAxis(uint32_t gamepad, GamepadAxis axis);
		// Lets go of every simulated key, mouse button, gamepad button and gamepad axis in the next input frame, also of presses
		// and axes still queued (queued button and axis events are dropped; pointer moves and scrolling stay queued).
		static void ReleaseAllSimulated();
		// Drops the simulated state and the queued events at once, without reporting releases (the game session ended).
		static void ClearSimulated();
		// Whether simulated events wait for the next input frame.
		static bool HasQueuedSimulatedInput();

		// The simulated state as of the current frame (events still queued are not included).
		static bool IsSimulatedKeyDown(KeyCode key);
		static bool IsSimulatedMouseButtonDown(MouseCode button);
		static std::optional<glm::vec2> GetSimulatedMousePosition();
		static bool IsSimulatedGamepadConnected(uint32_t gamepad);
		static bool IsSimulatedGamepadButtonDown(uint32_t gamepad, GamepadButton button);
		static std::optional<float> GetSimulatedGamepadAxis(uint32_t gamepad, GamepadAxis axis);
	};

}
