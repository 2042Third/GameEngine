#pragma once

#include "Strata/Input/InputActionSet.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// An action as of the current input frame.
	struct InputActionState
	{
		bool Down = false;
		bool Pressed = false;  // Down from this input frame on (a Button pressed and released within the frame reports both)
		bool Released = false; // Up from this input frame on
		glm::vec2 Value = glm::vec2(0.0f); // Button: (1 while down, 0); Axis: (value, 0); Vector2: the vector
	};

	// The devices a binding capture listens to.
	struct InputCaptureDevices
	{
		bool Keyboard = true;
		bool Mouse = true;
		bool Gamepad = true;
	};

	// The game's input actions as of the current input frame: the project's set (the settings section Input.Actions, read
	// through IProjectSettings; the default set, the UI map alone, without a provider or a section), the players' bindings,
	// which maps are enabled, simulated actions and binding capture. Main thread only, like Input.
	//
	// Update runs once per frame after the frame's input is in (Application's Input stage; code that runs a game in-process
	// calls it after Input::BeginFrame and its simulated input). It evaluates the actions only when an input frame started
	// (Input::GetFrameIndex advanced), so transitions last one input frame and survive suspended frames, as keys' do. Actions
	// read input through Input's gameplay queries, so the game UI's keyboard and pointer capture hide their bindings too.
	//
	// Values: a Button is down while any binding is (half axes from 0.5, until below 0.35); an Axis takes the binding of the
	// largest magnitude, a Vector2 the largest length (2D composites normalized), both apply the action's dead zone and are
	// down from 0.5 until below 0.35. A map disabled while one of its actions is held lets go of it in the next input frame,
	// which reports its Released as a key let go would, and its actions read as up after that. Every action reads as up while
	// a binding capture runs. Docs/DeveloperGuide.md, "Input actions".
	class InputActions
	{
	public:
		static constexpr float c_Press = 0.5f;
		static constexpr float c_Release = 0.35f;

		// Reads the project's set again when the provider's settings changed, advances a capture (also while input frames are
		// suspended) and evaluates the actions when an input frame started.
		static void Update();
		// Back to the start: the set is read again at the next Update, and the players' bindings, map enables, simulated
		// actions, the capture and every action's state are dropped. Input::Reset calls it.
		static void Reset();
		// What a game session leaves behind, dropped when a session starts or ends (the editor's play mode): maps enabled as the
		// set says, the players' bindings, simulated actions and the capture.
		static void ResetSession();

		// The project's set in use (without the players' bindings).
		static const InputActionSet& GetSet();
		// Changes whenever the set in use or the players' bindings change.
		static uint64_t GetVersion();

		// An action by "<Map>/<Action>" or an unambiguous bare name (InputActionSet::FindAction); nullopt with the reason.
		static std::optional<InputActionState> GetState(std::string_view action, std::string* outError = nullptr);
		static bool IsDown(std::string_view action);
		static bool IsPressed(std::string_view action);
		static bool IsReleased(std::string_view action);
		static glm::vec2 GetValue(std::string_view action);
		// "<Map>/<Action>" as the set spells it, or nullopt with the reason.
		static std::optional<std::string> GetQualifiedName(std::string_view action, std::string* outError = nullptr);

		// For code that reads the same actions every update (the game UI): an action's index in the set in use, valid until
		// GetVersion changes, and the state at an index as GetState reports it (an index out of range reads as up).
		static std::optional<uint32_t> FindIndex(std::string_view action, std::string* outError = nullptr);
		static InputActionState GetStateAt(uint32_t index);
		// Changes whenever what an action reports may have changed: an input frame evaluated, the set or the players' bindings
		// replaced, a map enabled or disabled, a capture begun or ended. Never 0.
		static uint64_t GetStateVersion();

		// Maps start enabled as the set says. Enabling or disabling one applies from the next input frame on: disabling it lets
		// go of its held actions there (each reports Released once), and they read as up until the map is enabled again.
		static bool SetMapEnabled(std::string_view map, bool enabled, std::string* outError = nullptr);
		// Nullopt for a map the set does not have.
		static std::optional<bool> IsMapEnabled(std::string_view map);

		// The bindings an action uses: the players' when they replaced the project's.
		static std::optional<std::vector<InputBinding>> GetBindings(std::string_view action, std::string* outError = nullptr);
		// Replaces an action's bindings for the players (a rebinding menu), from the next input frame on. False, changing
		// nothing, for an unknown action, more than InputActionSet::c_MaxBindings bindings or one the action's kind cannot use.
		static bool SetBindings(std::string_view action, std::span<const InputBinding> bindings, std::string* outError = nullptr);
		// Restores the project's bindings of an action, or of every action for an empty name.
		static bool ResetBindings(std::string_view action, std::string* outError = nullptr);
		// The players' bindings that apply (those of actions the set has), by qualified name.
		static InputPlayerBindings GetPlayerBindings();
		// Writes the players' bindings to <IGameSaves directory>/InputBindings.json, atomically. False, with the reason, when
		// there is no save directory or the write failed.
		static bool SaveBindings(std::string* outError = nullptr);
		// Replaces the players' bindings with those of the bindings file (none when there is no file or no save directory):
		// what the program running a game calls when the game starts. Entries of unknown actions and bindings their kind
		// cannot use are dropped with one warning; a file that is no bindings file is ignored with a warning.
		static void LoadBindings();
		// The bindings file as LoadBindings would apply it to the set in use, without applying it or warning (the editor shows
		// what the next play session starts with): empty without a file, a save directory, or with a file LoadBindings ignores.
		static InputPlayerBindings ReadSavedBindings();
		// Removes the bindings file, so that sessions from then on start with the project's bindings; the bindings in use stay.
		// True when there is no file afterwards (also when there was none); false, with the reason, without a save directory
		// or when the file cannot be removed.
		static bool DeleteSavedBindings(std::string* outError = nullptr);

		// Holds an action down (or lets it go) from the next input frame, whatever its bindings: tools that play the game. While
		// held, its value is `value` (Button: 1). False for an unknown action.
		static bool SimulateAction(std::string_view action, bool down, const glm::vec2& value = glm::vec2(1.0f, 0.0f), std::string* outError = nullptr);
		// Lets go of every simulated action in the next input frame.
		static void ReleaseAllSimulated();
		// Drops the simulated actions at once, without releases.
		static void ClearSimulated();

		// Listens for the next control the player presses on the devices given (a key, a mouse button, a gamepad button, or an
		// axis pushed beyond 0.5 from rest), whether or not input is enabled and also while input frames are suspended. While it
		// listens every action reads as up, and the control it captured reads as up for actions until an input frame went by with
		// it released, so the press that is captured triggers nothing (also when it is released while input frames are
		// suspended). False when no device is given.
		static bool BeginCapture(const InputCaptureDevices& devices);
		static bool IsCapturing();
		// The captured control's binding text ("Key/Space", "Gamepad/LeftX-") once, after which the capture is over; nullopt
		// while it listens or when none ran.
		static std::optional<std::string> TakeCapturedBinding();
		static void CancelCapture();
	};

}
