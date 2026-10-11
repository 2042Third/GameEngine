#pragma once

#include <Strata/Core/Base.h>
#include <Strata/Input/GamepadCodes.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Strata
{

	enum class SimulatedControlType : uint8_t
	{
		Key = 0,
		MouseButton,
		GamepadButton,
		GamepadAxis
	};

	// One command's hold of a key, mouse button, gamepad button or gamepad axis (see SimulatedInput).
	struct SimulatedHold
	{
		SimulatedControlType Type = SimulatedControlType::Key;
		uint16_t Code = 0;        // The key or mouse button; for gamepad controls SimulatedInput::MakeGamepadCode
		float Value = 0.0f;       // A gamepad axis' value
		uint64_t FirstFrame = 0;  // The input frame (Input::GetFrameIndex) the hold starts in
		uint64_t LastFrame = 0;   // A tap's last input frame; 0 for a hold that lasts until it is released
		bool Ended = false;
		bool Interrupted = false; // A tap that ended before its frames: released, replaced (an axis), input.releaseAll or the end of play
		uint64_t EndFrame = 0;    // Once ended: the input frame in which the game sees the end
	};

	// The editor's simulated keyboard, mouse buttons and gamepads (the input.* commands) on top of Input's simulated device.
	// Each command holds what it pressed: holds of one button stack (two taps of a key overlap), the button goes down with the
	// first and up with the last, and a tap ends only its own hold. A gamepad axis has one value: a new hold of an axis
	// replaces the one before (ending it, interrupted when it was a tap), and the axis returns to the device's value when its
	// hold ends. Frames are input frames (Input::GetFrameIndex), which stop while the game is paused (Input::SetSuspended),
	// so a tap lasts its number of game updates even when the game is stepped. Holds outlive the client that made them (the
	// editor's status bar lists them, with a button that releases them); play mode starting or stopping forgets them. Main
	// thread only.
	class SimulatedInput
	{
	public:
		// The code of a gamepad button or axis in SimulatedHold::Code: the gamepad in the high byte, the control in the low.
		static constexpr uint16_t MakeGamepadCode(uint32_t gamepad, uint32_t control) { return static_cast<uint16_t>((gamepad << 8) | (control & 0xFF)); }
		static constexpr uint32_t GetGamepad(uint16_t code) { return code >> 8; }
		static constexpr uint32_t GetGamepadControl(uint16_t code) { return code & 0xFF; }

		// Holds a key, mouse button or gamepad button for `frames` input frames (a tap), or with 0 until Release or ReleaseAll.
		// The button goes down in the next input frame, unless another hold keeps it down already.
		Ref<const SimulatedHold> Press(SimulatedControlType type, uint16_t code, uint32_t frames);
		// Holds a gamepad axis at `value` (clamped to the axis' range) for `frames` input frames, or with 0 until released or
		// replaced. The value applies in the next input frame.
		Ref<const SimulatedHold> HoldAxis(uint32_t gamepad, GamepadAxis axis, float value, uint32_t frames);
		// Ends every hold of the control; it goes up (an axis: back to the device's value) in the next input frame. Returns
		// false if no hold had it.
		bool Release(SimulatedControlType type, uint16_t code);
		// Ends every hold and lets go of every simulated control, also of presses still queued (Input::ReleaseAllSimulated).
		void ReleaseAll();
		// Forgets every hold without releasing anything (play mode started or stopped and Input::ClearSimulated dropped the
		// simulated device's state).
		void Reset();
		// Ends the taps whose last frame has passed. Once per frame, after the game's update and before the next input frame.
		void Update();

		// The controls of a type the holds keep down, in code order.
		std::vector<uint16_t> GetHeld(SimulatedControlType type) const;
		// The value a hold keeps a gamepad axis at (`code` from MakeGamepadCode), 0 without one.
		float GetAxisValue(uint16_t code) const;
		bool HasHolds() const { return !m_Holds.empty(); }
		// "Space, W, Mouse Left, Gamepad 1 A, Gamepad 1 LeftX 0.50": what the holds keep down, for people (the editor's
		// status bar; gamepads counted from 1). Empty without holds.
		std::string DescribeHolds() const;
	private:
		bool IsHeld(SimulatedControlType type, uint16_t code) const;
		// Marks a hold ended, removes it and lets the control go when no other hold keeps it down.
		void End(const Ref<SimulatedHold>& hold, bool interrupted);
	private:
		std::vector<Ref<SimulatedHold>> m_Holds; // Holds that have not ended, oldest first
	};

}
