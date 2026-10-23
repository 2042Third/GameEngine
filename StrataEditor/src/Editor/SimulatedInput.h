#pragma once

#include <Strata/Core/Base.h>
#include <Strata/Input/GamepadCodes.h>

#include <glm/glm.hpp>

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
		GamepadAxis,
		Action // An input action of the game, held whatever its bindings (InputActions::SimulateAction)
	};

	// One command's hold of a key, mouse button, gamepad button, gamepad axis or input action (see SimulatedInput).
	struct SimulatedHold
	{
		SimulatedControlType Type = SimulatedControlType::Key;
		uint16_t Code = 0;        // The key or mouse button; for gamepad controls SimulatedInput::MakeGamepadCode
		float Value = 0.0f;       // A gamepad axis' value
		std::string Action;       // An input action's qualified name
		glm::vec2 ActionValue = glm::vec2(0.0f); // Its value (a Button's is 1)
		uint64_t FirstFrame = 0;  // The input frame (Input::GetFrameIndex) the hold starts in
		// The game's input steps (EditorContext::GetInputStepCount: its fixed steps, and its frames at time scale 0) a tap covers,
		// from the first one that sees it down to the last; LastStep 0 for a hold that lasts until it is released.
		uint64_t FirstStep = 0;
		uint64_t LastStep = 0;
		bool Ended = false;
		bool Interrupted = false; // A tap that ended before its steps: released, replaced (an axis), input.releaseAll or the end of play
		uint64_t EndFrame = 0;    // Once ended: the input frame in which the game sees the end

		bool IsTap() const { return LastStep != 0; }
	};

	// The editor's simulated keyboard, mouse buttons, gamepads and input actions (the input.* commands) on top of Input's
	// simulated device and InputActions' simulated actions. Each command holds what it pressed: holds of one button stack
	// (two taps of a key overlap), the button goes down with the first and up with the last, and a tap ends only its own
	// hold. A gamepad axis and an input action have one value: a new hold of one replaces the one before (ending it,
	// interrupted when it was a tap) without letting it go, and an axis returns to the device's value, an action goes up,
	// when its hold ends. A tap lasts a number of the game's fixed steps (its physics and FixedUpdate time base), whatever the frame
	// rate: it goes down in the next input frame (Input::GetFrameIndex), and after the frame in which its last step ran it
	// goes up in the next one. With one step per frame (stepping, input.sequence) it covers exactly its steps; unpaused, a
	// frame may run several, so the release may come up to MaxFixedStepsPerFrame - 1 steps later. Input frames stop while the
	// game is paused (Input::SetSuspended), so input given then waits for the next step. Holds outlive the client that made them (the
	// editor's status bar lists them, with a button that releases them); play mode starting or stopping forgets them. Main
	// thread only.
	class SimulatedInput
	{
	public:
		// The code of a gamepad button or axis in SimulatedHold::Code: the gamepad in the high byte, the control in the low.
		static constexpr uint16_t MakeGamepadCode(uint32_t gamepad, uint32_t control) { return static_cast<uint16_t>((gamepad << 8) | (control & 0xFF)); }
		static constexpr uint32_t GetGamepad(uint16_t code) { return code >> 8; }
		static constexpr uint32_t GetGamepadControl(uint16_t code) { return code & 0xFF; }

		// Holds a key, mouse button or gamepad button for `steps` fixed steps of the game (a tap), or with 0 until Release or
		// ReleaseAll. `fixedSteps` is the game's input step count now (EditorContext::GetInputStepCount): the tap covers the next
		// `steps` steps. The button goes down in the next input frame, unless another hold keeps it down already.
		Ref<const SimulatedHold> Press(SimulatedControlType type, uint16_t code, uint32_t steps, uint64_t fixedSteps);
		// Holds a gamepad axis at `value` (clamped to the axis' range) for `steps` fixed steps, or with 0 until released or
		// replaced. The value applies in the next input frame.
		Ref<const SimulatedHold> HoldAxis(uint32_t gamepad, GamepadAxis axis, float value, uint32_t steps, uint64_t fixedSteps);
		// Holds an input action (its qualified name) at `value` for `steps` fixed steps, or with 0 until released or replaced,
		// whatever its bindings. It is down, with that value, from the next input frame.
		Ref<const SimulatedHold> HoldAction(const std::string& action, const glm::vec2& value, uint32_t steps, uint64_t fixedSteps);
		// Ends every hold of the control; it goes up (an axis: back to the device's value) in the next input frame. Returns
		// false if no hold had it.
		bool Release(SimulatedControlType type, uint16_t code);
		// Ends the hold of an input action (names match ignoring case); it goes up in the next input frame. False without one.
		bool ReleaseAction(const std::string& action);
		// Ends one hold (not interrupted when it was a press, interrupted when it was a tap that had steps left): its control goes
		// up (an axis: back to the device's value) in the next input frame unless another hold keeps it. False when it ended
		// already.
		bool EndHold(const Ref<const SimulatedHold>& hold);
		// Ends every hold and lets go of every simulated control, also of presses still queued (Input::ReleaseAllSimulated).
		void ReleaseAll();
		// Forgets every hold without releasing anything (play mode started or stopped and Input::ClearSimulated dropped the
		// simulated device's state).
		void Reset();
		// Ends the taps whose last step has run (`fixedSteps`, the game's step count after its update). Once per frame, after
		// the game's update and before the next input frame.
		void Update(uint64_t fixedSteps);

		// The controls of a type the holds keep down, in code order (not for actions: GetHeldActions).
		std::vector<uint16_t> GetHeld(SimulatedControlType type) const;
		// The holds of input actions, oldest first.
		std::vector<Ref<const SimulatedHold>> GetHeldActions() const;
		// The value a hold keeps a gamepad axis at (`code` from MakeGamepadCode), 0 without one.
		float GetAxisValue(uint16_t code) const;
		bool HasHolds() const { return !m_Holds.empty(); }
		// The holds that have not ended, oldest first (input.state reports them with their steps).
		std::vector<Ref<const SimulatedHold>> GetHolds() const { return { m_Holds.begin(), m_Holds.end() }; }
		// How many controls the holds keep down: the ones DescribeHolds names.
		size_t GetHeldCount() const;
		// "Space, W, Mouse Left, Gamepad 1 A, Gamepad 1 LeftX 0.50, Gameplay/Jump": what the holds keep down, for people (the
		// editor's status bar; gamepads counted from 1). Empty without holds.
		std::string DescribeHolds() const;
	private:
		bool IsHeld(SimulatedControlType type, uint16_t code) const;
		bool IsActionHeld(const std::string& action) const;
		// Marks a hold ended, removes it and lets the control go when no other hold keeps it down.
		void End(const Ref<SimulatedHold>& hold, bool interrupted);
	private:
		std::vector<Ref<SimulatedHold>> m_Holds; // Holds that have not ended, oldest first
	};

}
