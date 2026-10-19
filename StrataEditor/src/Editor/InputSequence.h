#pragma once

#include "Editor/SimulatedInput.h"

#include <Strata/Core/Base.h>
#include <Strata/Core/UUID.h>
#include <Strata/Input/GamepadCodes.h>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Strata
{

	class EditorCommandRegistry;
	class EditorContext;
	struct ComponentInfo;

	// A script of input for the running game, timed in its fixed steps (input.sequence): the game is paused (and resumed
	// afterwards if it ran), and every frame runs exactly one step with the events of that step given to it, so what the
	// game does is the same whatever the frame rate. Holds the sequence makes are its own (released at its end unless
	// asked to keep them); taps last their number of steps exactly. Observations sample component properties, script fields
	// or world transforms every few steps and at the end. EditorContext drives it after the game's update each frame.
	// Main thread only.
	class InputSequence
	{
	public:
		enum class EventType : uint8_t
		{
			Button = 0, // A key, mouse button or gamepad button: press, release or tap
			Axis,       // A gamepad axis value (null: back to the device's)
			MouseMove,
			Scroll,
			Command     // An input.* command run with wait: false
		};

		enum class ButtonAction : uint8_t
		{
			Tap = 0,
			Press,
			Release
		};

		struct Event
		{
			uint32_t Step = 0;  // Fixed steps from the sequence's start (0-based)
			size_t Index = 0;   // Its place in the request's events (results name it)
			EventType Type = EventType::Button;
			SimulatedControlType Control = SimulatedControlType::Key;
			uint16_t Code = 0;  // Key, mouse button, or SimulatedInput::MakeGamepadCode
			ButtonAction Action = ButtonAction::Tap;
			uint32_t TapSteps = 1;
			uint32_t Gamepad = 0;
			GamepadAxis Axis = GamepadAxis::LeftX;
			std::optional<float> AxisValue;
			glm::vec2 Vector = glm::vec2(0.0f); // Pointer position or scroll offset
			std::string Command;
			nlohmann::json Parameters;
		};

		enum class ObservationType : uint8_t
		{
			Component = 0, // A component's values, or one property
			ScriptField,
			World          // The entity's world transform
		};

		struct Observation
		{
			ObservationType Type = ObservationType::Component;
			UUID Entity = UUID::Null();
			const ComponentInfo* Info = nullptr;
			std::string Property;  // Component: one property (empty: all)
			std::string ClassName; // Script field
			std::string Field;
			uint32_t Every = 0;    // Sampled after every this many steps; 0: only at the end
		};

		struct Result
		{
			bool Finished = false;
			bool Failed = false;
			std::string Error;
			uint32_t Steps = 0;
			uint32_t StepsRun = 0;
			uint64_t FrameIndex = 0;
			double Time = 0.0;
			nlohmann::json Seen = nlohmann::json::array();    // [{step, event}]: the events the game was given, by index
			nlohmann::json Samples = nlohmann::json::array(); // [{step, values: [{observe, value}]}]
			bool Interrupted = false; // A tap of the sequence was ended by something else (a release, input.releaseAll)
		};

		// `events` sorted by step (stable: one step's events apply in the order given); `commands` runs command events.
		InputSequence(std::vector<Event> events, uint32_t steps, std::vector<Observation> observations, bool keepHeld, const EditorCommandRegistry& commands);

		// Pauses the running game (remembering whether it ran) and gives the first step its events. False, with the reason,
		// when a command event fails.
		bool Start(EditorContext& context, std::string* outError);
		// After the game's update each frame: counts the step that ran, samples, and gives the next step its events (or
		// finishes: releases its holds unless keepHeld, resumes the game if it ran).
		void Update(EditorContext& context);
		// Ends it as failed (play mode stopped): nothing more runs.
		void Fail(EditorContext& context, std::string error);

		bool IsFinished() const { return m_Result.Finished; }
		const Result& GetResult() const { return m_Result; }
	private:
		bool ApplyEvents(EditorContext& context, std::string* outError);
		void Sample(EditorContext& context, bool final);
		void Finish(EditorContext& context);
		nlohmann::json Observe(EditorContext& context, const Observation& observation) const;
	private:
		std::vector<Event> m_Events;
		size_t m_NextEvent = 0;
		std::vector<Observation> m_Observations;
		bool m_KeepHeld = false;
		const EditorCommandRegistry& m_Commands;
		bool m_WasRunning = false;
		uint64_t m_Session = 0;
		uint64_t m_ExpectedSteps = 0; // The game's fixed step count once the step given its events ran
		std::vector<Ref<const SimulatedHold>> m_Holds;
		Result m_Result;
	};

}
