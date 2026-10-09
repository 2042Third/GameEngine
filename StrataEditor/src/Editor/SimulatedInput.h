#pragma once

#include <Strata/Core/Base.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Strata
{

	enum class SimulatedButtonType : uint8_t
	{
		Key = 0,
		MouseButton
	};

	// One command's hold of a key or mouse button (see SimulatedInput).
	struct SimulatedHold
	{
		SimulatedButtonType Type = SimulatedButtonType::Key;
		uint16_t Code = 0;
		uint64_t FirstFrame = 0; // The input frame (Input::GetFrameIndex) the hold starts in
		uint64_t LastFrame = 0;  // A tap's last input frame; 0 for a hold that lasts until it is released
		bool Ended = false;
		bool Interrupted = false; // A tap that ended before its frames: released, input.releaseAll or the end of play
		uint64_t EndFrame = 0;    // Once ended: the input frame in which the game sees the end
	};

	// The editor's simulated keyboard and mouse buttons (the input.* commands) on top of Input's simulated device. Each
	// command holds what it pressed: holds of one button stack (two taps of a key overlap), the button goes down with the
	// first and up with the last, and a tap ends only its own hold. Frames are input frames (Input::GetFrameIndex), which
	// stop while the game is paused (Input::SetSuspended), so a tap lasts its number of game updates even when the game is
	// stepped. Holds outlive the client that made them (the editor's status bar lists them, with a button that releases them);
	// play mode starting or stopping forgets them. Main thread only.
	class SimulatedInput
	{
	public:
		// Holds a button for `frames` input frames (a tap), or with 0 until Release or ReleaseAll. The button goes down in the
		// next input frame, unless another hold keeps it down already.
		Ref<const SimulatedHold> Press(SimulatedButtonType type, uint16_t code, uint32_t frames);
		// Ends every hold of the button; it goes up in the next input frame. Returns false if no hold had it.
		bool Release(SimulatedButtonType type, uint16_t code);
		// Ends every hold and lets go of every simulated button, also of presses still queued (Input::ReleaseAllSimulated).
		void ReleaseAll();
		// Forgets every hold without releasing anything (play mode started or stopped and Input::ClearSimulated dropped the
		// simulated device's state).
		void Reset();
		// Ends the taps whose last frame has passed. Once per frame, after the game's update and before the next input frame.
		void Update();

		// The buttons of a type the holds keep down, in code order.
		std::vector<uint16_t> GetHeld(SimulatedButtonType type) const;
		bool HasHolds() const { return !m_Holds.empty(); }
		// "Space, W, Mouse Left": what the holds keep down, for people (the editor's status bar). Empty without holds.
		std::string DescribeHolds() const;
	private:
		bool IsHeld(SimulatedButtonType type, uint16_t code) const;
		// Marks a hold ended, removes it and lets the button go when no other hold keeps it down.
		void End(const Ref<SimulatedHold>& hold, bool interrupted);
	private:
		std::vector<Ref<SimulatedHold>> m_Holds; // Holds that have not ended, oldest first
	};

}
