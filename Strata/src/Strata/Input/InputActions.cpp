#include "stpch.h"
#include "Strata/Input/InputActions.h"

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/StringUtils.h"
#include "Strata/Input/Input.h"
#include "Strata/Input/InputNames.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bitset>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Strata
{

	namespace
	{

		struct CompiledBinding
		{
			InputBinding::Shape Shape = InputBinding::Shape::Control;
			uint32_t FirstControl = 0; // Into ActionsState::Controls: 1, 2 or 4 of them
		};

		// An action of the set in use, with its state.
		struct ActionRecord
		{
			std::string Key; // The qualified name in lower case: what moves state across rebuilds
			uint32_t Map = 0;
			InputActionKind Kind = InputActionKind::Button;
			float DeadZone = InputActionSet::c_DefaultDeadZone;
			uint32_t FirstBinding = 0;
			uint32_t BindingCount = 0;
			InputActionState State;
			bool ValueHeld = false; // Axis and Vector2: beyond c_Press, until below c_Release
			bool SimulatedDown = false;
			bool SimulatedPressed = false; // In the input frame being evaluated (a tap within it reports both)
			glm::vec2 SimulatedValue = glm::vec2(0.0f);
		};

		struct SimulatedEvent
		{
			uint32_t Action = 0;
			bool Down = false;
			glm::vec2 Value = glm::vec2(0.0f);
		};

		// The gamepads as the game reads them, taken once per evaluation.
		struct GamepadSnapshot
		{
			bool Connected[c_MaxGamepads] = {};
			float Axes[c_MaxGamepads][c_GamepadAxisCount] = {};
			std::bitset<c_GamepadButtonCount> Down[c_MaxGamepads];
			std::bitset<c_GamepadButtonCount> Pressed[c_MaxGamepads];
		};

		struct CaptureState
		{
			bool Active = false;
			InputCaptureDevices Devices;
			// The controls held since the capture began (or since the last call): only a control that goes down is captured.
			std::bitset<c_MaxKeyCode> Keys;
			std::bitset<c_MaxMouseButtons> MouseButtons;
			std::bitset<c_GamepadButtonCount> GamepadButtons[c_MaxGamepads];
			bool HalfAxes[c_MaxGamepads][c_GamepadAxisCount][2] = {}; // [..][0] the + half, [1] the - half
			std::optional<std::string> Result;
		};

		struct ActionsState
		{
			bool Parsed = false;
			const IProjectSettings* Provider = nullptr;
			uint64_t ProviderVersion = 0;
			InputActionSet Set;
			uint64_t Version = 0;
			uint64_t StateVersion = 1; // GetStateVersion

			std::vector<bool> MapEnabled;
			std::vector<uint32_t> MapFirstAction;
			std::vector<InputControl> Controls;
			// Per control, per gamepad: whether a half axis holds a Button action (pressed beyond c_Press, held until below
			// c_Release).
			std::vector<std::array<bool, c_MaxGamepads>> HalfAxisHeld;
			std::vector<CompiledBinding> Bindings;
			std::vector<ActionRecord> Actions;
			// Lower-case qualified and bare names to their action; -1 for a bare name several maps have.
			std::unordered_map<std::string, int32_t> Names;
			std::string LookupKey; // Kept to lower-case query names without allocating

			InputPlayerBindings Players;
			std::vector<SimulatedEvent> SimulatedQueue;
			uint64_t EvaluatedFrame = 0;
			CaptureState Capture;
			std::optional<InputControl> Suppressed; // What a capture took: up for actions until an input frame went by with it released
		};

		ActionsState s_State;

		// Moves the action records of a new set or of new players' bindings into place, keeping the state (and simulated holds)
		// of actions that keep their qualified name, so an edit while a game runs does not press held actions again.
		void Compile(InputActionSet set, bool keepMapEnables)
		{
			std::unordered_map<std::string, uint32_t> previousIndex;
			for (uint32_t index = 0; index < s_State.Actions.size(); index++)
				previousIndex.emplace(s_State.Actions[index].Key, index);
			std::unordered_map<std::string, bool> previousEnabled;
			if (keepMapEnables)
			{
				for (uint32_t map = 0; map < s_State.Set.Maps.size() && map < s_State.MapEnabled.size(); map++)
					previousEnabled.emplace(StringUtils::ToLower(s_State.Set.Maps[map].Name), s_State.MapEnabled[map]);
			}
			std::unordered_map<std::string, const std::vector<InputBinding>*> players;
			for (const auto& [name, bindings] : s_State.Players.Actions)
				players.emplace(StringUtils::ToLower(name), &bindings);

			std::vector<ActionRecord> previous = std::move(s_State.Actions);
			const std::vector<InputControl> previousControls = std::move(s_State.Controls);
			const std::vector<CompiledBinding> previousBindings = std::move(s_State.Bindings);
			const std::vector<std::array<bool, c_MaxGamepads>> previousHalfAxisHeld = std::move(s_State.HalfAxisHeld);
			s_State.Set = std::move(set);
			s_State.Actions.clear();
			s_State.Controls.clear();
			s_State.Bindings.clear();
			s_State.Names.clear();
			s_State.MapEnabled.clear();
			s_State.MapFirstAction.clear();
			std::vector<int32_t> moved(previous.size(), -1);
			for (uint32_t map = 0; map < s_State.Set.Maps.size(); map++)
			{
				const InputActionMap& actionMap = s_State.Set.Maps[map];
				const auto enabled = previousEnabled.find(StringUtils::ToLower(actionMap.Name));
				s_State.MapEnabled.push_back(enabled != previousEnabled.end() ? enabled->second : actionMap.Enabled);
				s_State.MapFirstAction.push_back(static_cast<uint32_t>(s_State.Actions.size()));
				for (uint32_t index = 0; index < actionMap.Actions.size(); index++)
				{
					const InputAction& action = actionMap.Actions[index];
					ActionRecord record;
					record.Key = StringUtils::ToLower(s_State.Set.GetQualifiedName({ map, index }));
					record.Map = map;
					record.Kind = action.Kind;
					record.DeadZone = action.DeadZone;
					record.FirstBinding = static_cast<uint32_t>(s_State.Bindings.size());

					// The players' list replaces the project's when every binding still fits the action (its kind may have changed).
					const std::vector<InputBinding>* bindings = &action.Bindings;
					if (const auto player = players.find(record.Key); player != players.end())
					{
						const bool fits = std::all_of(player->second->begin(), player->second->end(),
							[&action](const InputBinding& binding) { return binding.FitsKind(action.Kind); });
						if (fits)
							bindings = player->second;
					}
					for (const InputBinding& binding : *bindings)
					{
						s_State.Bindings.push_back({ binding.Type, static_cast<uint32_t>(s_State.Controls.size()) });
						for (uint32_t control = 0; control < binding.GetControlCount(); control++)
							s_State.Controls.push_back(binding.Controls[control]);
					}
					record.BindingCount = static_cast<uint32_t>(s_State.Bindings.size()) - record.FirstBinding;

					if (const auto found = previousIndex.find(record.Key); found != previousIndex.end())
					{
						const ActionRecord& old = previous[found->second];
						if (old.Kind == record.Kind)
						{
							record.State = old.State;
							record.ValueHeld = old.ValueHeld;
							record.SimulatedDown = old.SimulatedDown;
							record.SimulatedValue = old.SimulatedValue;
							moved[found->second] = static_cast<int32_t>(s_State.Actions.size());
						}
					}
					const int32_t recordIndex = static_cast<int32_t>(s_State.Actions.size());
					s_State.Names[record.Key] = recordIndex;
					const std::string bare = StringUtils::ToLower(action.Name);
					const auto [bareEntry, inserted] = s_State.Names.emplace(bare, recordIndex);
					if (!inserted)
						bareEntry->second = -1;
					s_State.Actions.push_back(std::move(record));
				}
			}
			s_State.HalfAxisHeld.assign(s_State.Controls.size(), {});
			// Half axes holding a Button keep their hysteresis where the action keeps its bindings: a stick resting between the
			// release and the press thresholds keeps it held through an edit of another action.
			for (uint32_t index = 0; index < previous.size(); index++)
			{
				if (moved[index] < 0)
					continue;
				const ActionRecord& old = previous[index];
				const ActionRecord& record = s_State.Actions[static_cast<size_t>(moved[index])];
				if (record.Kind != InputActionKind::Button || old.BindingCount != record.BindingCount)
					continue;
				bool same = true;
				for (uint32_t binding = 0; binding < record.BindingCount && same; binding++)
				{
					const CompiledBinding& before = previousBindings[old.FirstBinding + binding];
					const CompiledBinding& now = s_State.Bindings[record.FirstBinding + binding];
					same = before.Shape == now.Shape && previousControls[before.FirstControl] == s_State.Controls[now.FirstControl];
				}
				if (!same)
					continue;
				// Buttons take single controls (FitsKind), so each binding has one control slot.
				for (uint32_t binding = 0; binding < record.BindingCount; binding++)
				{
					s_State.HalfAxisHeld[s_State.Bindings[record.FirstBinding + binding].FirstControl] =
						previousHalfAxisHeld[previousBindings[old.FirstBinding + binding].FirstControl];
				}
			}

			// Simulated events still queued follow their actions.
			std::vector<SimulatedEvent> queue;
			for (SimulatedEvent event : s_State.SimulatedQueue)
			{
				if (event.Action < moved.size() && moved[event.Action] >= 0)
				{
					event.Action = static_cast<uint32_t>(moved[event.Action]);
					queue.push_back(event);
				}
			}
			s_State.SimulatedQueue = std::move(queue);
			s_State.Version++;
			s_State.StateVersion++;
		}

		// Reads the project's set again when the provider or its settings changed.
		void Refresh()
		{
			const IProjectSettings* provider = EngineServices::Get<IProjectSettings>();
			const uint64_t version = provider ? provider->GetVersion() : 0;
			if (s_State.Parsed && provider == s_State.Provider && version == s_State.ProviderVersion)
				return;
			s_State.Provider = provider;
			s_State.ProviderVersion = version;

			InputActionSet set = InputActionSet::MakeDefault();
			if (const nlohmann::json* value = provider ? provider->FindSection(InputActionSet::c_SectionId) : nullptr)
			{
				// The provider holds values its section validated, so this fails only when the section's reader changed.
				std::string error;
				if (std::optional<InputActionSet> parsed = InputActionSet::FromJson(*value, &error))
					set = std::move(*parsed);
				else
					ST_CORE_ERROR("The project's input actions cannot be used ({}); the default actions apply", error);
			}
			if (s_State.Parsed && set == s_State.Set)
				return;
			const bool keepMapEnables = s_State.Parsed;
			s_State.Parsed = true;
			Compile(std::move(set), keepMapEnables);
		}

		void EnsureParsed()
		{
			if (!s_State.Parsed)
				Refresh();
		}

		// The record of an action name, or null with the reason.
		ActionRecord* FindRecord(std::string_view name, std::string* outError)
		{
			EnsureParsed();
			std::string& key = s_State.LookupKey;
			key.assign(name);
			for (char& character : key)
				character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
			const auto found = s_State.Names.find(key);
			if (found != s_State.Names.end() && found->second >= 0)
				return &s_State.Actions[static_cast<size_t>(found->second)];
			// Unknown or ambiguous: the set says why.
			std::string error;
			s_State.Set.FindAction(name, &error);
			if (outError)
				*outError = std::move(error);
			return nullptr;
		}

		// What a query reports: every action reads as up while a capture listens. A disabled map's actions need no masking here:
		// the input frame after the map was disabled lets them go (Evaluate).
		InputActionState GetReported(const ActionRecord& record)
		{
			if (s_State.Capture.Active)
				return {};
			return record.State;
		}

		bool IsSuppressed(InputControlType type, uint16_t code)
		{
			const std::optional<InputControl>& suppressed = s_State.Suppressed;
			if (!suppressed)
				return false;
			if (type == InputControlType::Key || type == InputControlType::MouseButton || type == InputControlType::GamepadButton)
				return suppressed->Type == type && suppressed->Code == code;
			return false;
		}

		// Whether a captured half axis keeps an axis at rest for actions.
		bool IsAxisSuppressed(uint32_t axis)
		{
			const std::optional<InputControl>& suppressed = s_State.Suppressed;
			return suppressed && suppressed->Type == InputControlType::GamepadHalfAxis && suppressed->Code == axis;
		}

		GamepadSnapshot ReadGamepads()
		{
			GamepadSnapshot snapshot;
			for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
			{
				if (!Input::IsGamepadConnected(gamepad))
					continue;
				snapshot.Connected[gamepad] = true;
				for (uint32_t axis = 0; axis < c_GamepadAxisCount; axis++)
					snapshot.Axes[gamepad][axis] = IsAxisSuppressed(axis) ? 0.0f : Input::GetGamepadAxis(gamepad, static_cast<GamepadAxis>(axis));
				for (uint32_t button = 0; button < c_GamepadButtonCount; button++)
				{
					if (IsSuppressed(InputControlType::GamepadButton, static_cast<uint16_t>(button)))
						continue;
					snapshot.Down[gamepad].set(button, Input::IsGamepadButtonDown(gamepad, static_cast<GamepadButton>(button)));
					snapshot.Pressed[gamepad].set(button, Input::IsGamepadButtonPressed(gamepad, static_cast<GamepadButton>(button)));
				}
			}
			return snapshot;
		}

		// A button-like control as a Button action reads it.
		void ReadButton(uint32_t controlIndex, const GamepadSnapshot& gamepads, bool& down, bool& pressed)
		{
			const InputControl& control = s_State.Controls[controlIndex];
			switch (control.Type)
			{
				case InputControlType::Key:
					if (IsSuppressed(control.Type, control.Code))
						return;
					down = down || Input::IsKeyDown(control.Code);
					pressed = pressed || Input::IsKeyPressed(control.Code);
					return;
				case InputControlType::MouseButton:
					if (IsSuppressed(control.Type, control.Code))
						return;
					down = down || Input::IsMouseButtonDown(control.Code);
					pressed = pressed || Input::IsMouseButtonPressed(control.Code);
					return;
				case InputControlType::GamepadButton:
					for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
					{
						down = down || gamepads.Down[gamepad].test(control.Code);
						pressed = pressed || gamepads.Pressed[gamepad].test(control.Code);
					}
					return;
				case InputControlType::GamepadHalfAxis:
				{
					std::array<bool, c_MaxGamepads>& held = s_State.HalfAxisHeld[controlIndex];
					for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
					{
						// Pushed beyond c_Press, released below c_Release: a stick resting near the threshold does not flicker.
						const float value = gamepads.Axes[gamepad][control.Code] * static_cast<float>(control.Sign);
						held[gamepad] = held[gamepad] ? value > InputActions::c_Release : value > InputActions::c_Press;
						down = down || held[gamepad];
					}
					return;
				}
				case InputControlType::GamepadAxis:
				case InputControlType::GamepadStick:
					return; // Not button-like: Read refuses them for Buttons
			}
		}

		// A control as Axis actions and composites read it: button-like controls 1 while down, half axes from 0 to 1, whole axes
		// from -1 to 1 (the gamepad pushing it furthest).
		float ReadValue(const InputControl& control, const GamepadSnapshot& gamepads)
		{
			switch (control.Type)
			{
				case InputControlType::Key:
					return !IsSuppressed(control.Type, control.Code) && Input::IsKeyDown(control.Code) ? 1.0f : 0.0f;
				case InputControlType::MouseButton:
					return !IsSuppressed(control.Type, control.Code) && Input::IsMouseButtonDown(control.Code) ? 1.0f : 0.0f;
				case InputControlType::GamepadButton:
					for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
					{
						if (gamepads.Down[gamepad].test(control.Code))
							return 1.0f;
					}
					return 0.0f;
				case InputControlType::GamepadHalfAxis:
				{
					float value = 0.0f;
					for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
						value = std::max(value, gamepads.Axes[gamepad][control.Code] * static_cast<float>(control.Sign));
					return std::min(value, 1.0f);
				}
				case InputControlType::GamepadAxis:
				{
					float value = 0.0f;
					for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
					{
						const float axis = gamepads.Axes[gamepad][control.Code];
						if (std::abs(axis) > std::abs(value))
							value = axis;
					}
					return std::clamp(value, -1.0f, 1.0f);
				}
				case InputControlType::GamepadStick:
					return 0.0f; // Read as a vector (ReadStick)
			}
			return 0.0f;
		}

		glm::vec2 ReadStick(const InputControl& control, const GamepadSnapshot& gamepads)
		{
			const uint32_t x = static_cast<uint32_t>(control.Code == 0 ? GamepadAxis::LeftX : GamepadAxis::RightX);
			const uint32_t y = static_cast<uint32_t>(control.Code == 0 ? GamepadAxis::LeftY : GamepadAxis::RightY);
			glm::vec2 value(0.0f);
			for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
			{
				const glm::vec2 stick(gamepads.Axes[gamepad][x], gamepads.Axes[gamepad][y]);
				if (glm::dot(stick, stick) > glm::dot(value, value))
					value = stick;
			}
			return value;
		}

		// Values within the dead zone read 0, the rest is rescaled to reach 1 (as the script SDK's GetGamepadAxis and
		// GetGamepadStick do): for a vector by its length, so diagonals keep their direction.
		glm::vec2 ApplyDeadZone(const glm::vec2& value, float deadZone)
		{
			const float length = glm::length(value);
			if (!(length > deadZone) || deadZone >= 1.0f)
				return glm::vec2(0.0f);
			const float scaled = std::min((length - deadZone) / (1.0f - deadZone), 1.0f);
			return value / length * scaled;
		}

		void Evaluate()
		{
			ST_PROFILE_FUNCTION();
			s_State.StateVersion++;
			for (ActionRecord& record : s_State.Actions)
				record.SimulatedPressed = false;
			for (const SimulatedEvent& event : s_State.SimulatedQueue)
			{
				ActionRecord& record = s_State.Actions[event.Action];
				if (event.Down && !record.SimulatedDown)
					record.SimulatedPressed = true;
				record.SimulatedDown = event.Down;
				if (event.Down)
					record.SimulatedValue = event.Value;
			}
			s_State.SimulatedQueue.clear();

			const GamepadSnapshot gamepads = ReadGamepads();
			for (ActionRecord& record : s_State.Actions)
			{
				InputActionState& state = record.State;
				const bool wasDown = state.Down;
				if (s_State.Capture.Active || !s_State.MapEnabled[record.Map])
				{
					// Let go as a released key would be: an action held when its map was disabled reports Released in this input
					// frame and reads as up after it, so code waiting for the release (a charge, a held state) is never left
					// waiting. While a capture listens the release is hidden with everything else (GetReported).
					state = {};
					state.Released = wasDown;
					record.ValueHeld = false;
					for (uint32_t binding = record.FirstBinding; binding < record.FirstBinding + record.BindingCount; binding++)
					{
						const CompiledBinding& compiled = s_State.Bindings[binding];
						const uint32_t count = compiled.Shape == InputBinding::Shape::Control ? 1u : (compiled.Shape == InputBinding::Shape::Composite1D ? 2u : 4u);
						for (uint32_t control = compiled.FirstControl; control < compiled.FirstControl + count; control++)
							s_State.HalfAxisHeld[control] = {};
					}
					continue;
				}

				bool down = false;
				bool pressed = false;
				glm::vec2 value(0.0f);
				for (uint32_t binding = record.FirstBinding; binding < record.FirstBinding + record.BindingCount; binding++)
				{
					const CompiledBinding& compiled = s_State.Bindings[binding];
					const InputControl* controls = &s_State.Controls[compiled.FirstControl];
					switch (record.Kind)
					{
						case InputActionKind::Button:
							ReadButton(compiled.FirstControl, gamepads, down, pressed);
							break;
						case InputActionKind::Axis:
						{
							const float axis = compiled.Shape == InputBinding::Shape::Composite1D
								? ReadValue(controls[1], gamepads) - ReadValue(controls[0], gamepads)
								: ReadValue(controls[0], gamepads);
							if (std::abs(axis) > std::abs(value.x))
								value.x = axis;
							break;
						}
						case InputActionKind::Vector2:
						{
							glm::vec2 vector(0.0f);
							if (compiled.Shape == InputBinding::Shape::Composite2D)
							{
								vector = glm::vec2(ReadValue(controls[3], gamepads) - ReadValue(controls[2], gamepads),
									ReadValue(controls[0], gamepads) - ReadValue(controls[1], gamepads));
								// Normalized: a diagonal of two keys has length 1, like a full stick.
								const float length = glm::length(vector);
								if (length > 1.0f)
									vector /= length;
							}
							else
							{
								vector = ReadStick(controls[0], gamepads);
							}
							if (glm::dot(vector, vector) > glm::dot(value, value))
								value = vector;
							break;
						}
					}
				}

				if (record.Kind != InputActionKind::Button)
				{
					value = ApplyDeadZone(value, record.DeadZone);
					const float magnitude = glm::length(value);
					record.ValueHeld = record.ValueHeld ? magnitude > InputActions::c_Release : magnitude > InputActions::c_Press;
					down = record.ValueHeld;
				}
				if (record.SimulatedDown)
				{
					down = true;
					value = record.Kind == InputActionKind::Vector2 ? record.SimulatedValue
						: glm::vec2(record.Kind == InputActionKind::Axis ? record.SimulatedValue.x : 1.0f, 0.0f);
				}
				pressed = pressed || record.SimulatedPressed;
				if (record.Kind == InputActionKind::Button)
					value = glm::vec2(down ? 1.0f : 0.0f, 0.0f);

				state.Down = down;
				state.Pressed = !wasDown && (down || pressed);
				state.Released = !down && (wasDown || state.Pressed);
				state.Value = value;
			}
		}

		// Lets the control a capture took count for actions again once an evaluated input frame saw it released. Not sooner: a
		// press and release while input frames are suspended reach the game's next input frame as transitions (Input keeps them
		// for it), and that frame must not report the captured press.
		void UpdateSuppression()
		{
			if (!s_State.Suppressed)
				return;
			const InputControl& control = *s_State.Suppressed;
			bool held = false;
			switch (control.Type)
			{
				case InputControlType::Key:
					held = Input::IsRawKeyDown(control.Code);
					break;
				case InputControlType::MouseButton:
					held = Input::IsRawMouseButtonDown(control.Code);
					break;
				case InputControlType::GamepadButton:
					for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
						held = held || Input::IsRawGamepadButtonDown(gamepad, static_cast<GamepadButton>(control.Code));
					break;
				case InputControlType::GamepadHalfAxis:
					for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
					{
						const float value = Input::GetRawGamepadAxis(gamepad, static_cast<GamepadAxis>(control.Code)) * static_cast<float>(control.Sign);
						held = held || value > InputActions::c_Release;
					}
					break;
				case InputControlType::GamepadAxis:
				case InputControlType::GamepadStick:
					break; // A capture takes no whole axes
			}
			if (!held)
				s_State.Suppressed.reset();
		}

		void FinishCapture(const InputControl& control)
		{
			s_State.StateVersion++;
			InputBinding binding;
			binding.Controls[0] = control;
			s_State.Capture.Active = false;
			s_State.Capture.Result = binding.ToString();
			s_State.Suppressed = control;
		}

		// Compares the controls with those held when the capture began (or at the last call): the first that went down ends it.
		// Keys first, then mouse buttons, gamepad buttons and axes, each in code order.
		void UpdateCapture(bool begin)
		{
			CaptureState& capture = s_State.Capture;
			if (!capture.Active)
				return;
			if (capture.Devices.Keyboard)
			{
				for (KeyCode key = 0; key < c_MaxKeyCode; key++)
				{
					const bool held = Input::IsRawKeyDown(key);
					const bool wentDown = held && !capture.Keys.test(key);
					capture.Keys.set(key, held);
					if (wentDown && !begin && InputNames::GetKeyName(key))
						return FinishCapture({ InputControlType::Key, key, 1 });
				}
			}
			if (capture.Devices.Mouse)
			{
				for (MouseCode button = 0; button < c_MaxMouseButtons; button++)
				{
					const bool held = Input::IsRawMouseButtonDown(button);
					const bool wentDown = held && !capture.MouseButtons.test(button);
					capture.MouseButtons.set(button, held);
					if (wentDown && !begin && InputNames::GetMouseButtonName(button))
						return FinishCapture({ InputControlType::MouseButton, button, 1 });
				}
			}
			if (!capture.Devices.Gamepad)
				return;
			for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
			{
				for (uint16_t button = 0; button < c_GamepadButtonCount; button++)
				{
					const bool held = Input::IsRawGamepadButtonDown(gamepad, static_cast<GamepadButton>(button));
					const bool wentDown = held && !capture.GamepadButtons[gamepad].test(button);
					capture.GamepadButtons[gamepad].set(button, held);
					if (wentDown && !begin)
						return FinishCapture({ InputControlType::GamepadButton, button, 1 });
				}
			}
			for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
			{
				for (uint16_t axis = 0; axis < c_GamepadAxisCount; axis++)
				{
					const float value = Input::GetRawGamepadAxis(gamepad, static_cast<GamepadAxis>(axis));
					for (uint32_t half = 0; half < 2; half++)
					{
						const int8_t sign = half == 0 ? 1 : -1;
						if (sign < 0 && GetGamepadAxisMinimum(static_cast<GamepadAxis>(axis)) >= 0.0f)
							continue; // Triggers have only their + half
						bool& held = capture.HalfAxes[gamepad][axis][half];
						const float pushed = value * static_cast<float>(sign);
						const bool wasHeld = held;
						held = held ? pushed > InputActions::c_Release : pushed > InputActions::c_Press;
						if (held && !wasHeld && !begin)
							return FinishCapture({ InputControlType::GamepadHalfAxis, axis, sign });
					}
				}
			}
		}

		// The save directory's bindings file, or nullopt with the reason.
		std::optional<std::filesystem::path> GetBindingsFile(std::string* outError)
		{
			IGameSaves* saves = EngineServices::Get<IGameSaves>();
			const std::optional<std::filesystem::path> directory = saves ? saves->GetSaveDirectory() : std::nullopt;
			if (!directory)
			{
				if (outError)
					*outError = "The game has no save directory to keep the players' bindings in";
				return std::nullopt;
			}
			return *directory / FileSystem::FromUTF8(InputPlayerBindings::c_FileName);
		}

	}

	void InputActions::Update()
	{
		ST_PROFILE_FUNCTION();
		Refresh();
		UpdateCapture(false);
		const uint64_t frame = Input::GetFrameIndex();
		if (frame == s_State.EvaluatedFrame)
			return;
		s_State.EvaluatedFrame = frame;
		Evaluate();
		// After the evaluation: the input frame in which the captured control was released (and the press Input kept for it
		// while frames were suspended) went by without it.
		UpdateSuppression();
	}

	void InputActions::Reset()
	{
		// The versions go on counting: code that kept an index or a state version notices the reset.
		const uint64_t version = s_State.Version;
		const uint64_t stateVersion = s_State.StateVersion;
		s_State = ActionsState();
		s_State.Version = version;
		s_State.StateVersion = stateVersion + 1;
	}

	void InputActions::ResetSession()
	{
		// The session starts from the project's set as it is now.
		Refresh();
		s_State.Players.Actions.clear();
		s_State.SimulatedQueue.clear();
		s_State.Capture = CaptureState();
		s_State.Suppressed.reset();
		for (ActionRecord& record : s_State.Actions)
		{
			record.State = {};
			record.ValueHeld = false;
			record.SimulatedDown = false;
			record.SimulatedPressed = false;
		}
		s_State.HalfAxisHeld.assign(s_State.HalfAxisHeld.size(), {});
		Compile(InputActionSet(s_State.Set), false);
	}

	const InputActionSet& InputActions::GetSet()
	{
		EnsureParsed();
		return s_State.Set;
	}

	uint64_t InputActions::GetVersion()
	{
		EnsureParsed();
		return s_State.Version;
	}

	std::optional<InputActionState> InputActions::GetState(std::string_view action, std::string* outError)
	{
		const ActionRecord* record = FindRecord(action, outError);
		if (!record)
			return std::nullopt;
		return GetReported(*record);
	}

	bool InputActions::IsDown(std::string_view action)
	{
		const ActionRecord* record = FindRecord(action, nullptr);
		return record && GetReported(*record).Down;
	}

	bool InputActions::IsPressed(std::string_view action)
	{
		const ActionRecord* record = FindRecord(action, nullptr);
		return record && GetReported(*record).Pressed;
	}

	bool InputActions::IsReleased(std::string_view action)
	{
		const ActionRecord* record = FindRecord(action, nullptr);
		return record && GetReported(*record).Released;
	}

	glm::vec2 InputActions::GetValue(std::string_view action)
	{
		const ActionRecord* record = FindRecord(action, nullptr);
		return record ? GetReported(*record).Value : glm::vec2(0.0f);
	}

	std::optional<std::string> InputActions::GetQualifiedName(std::string_view action, std::string* outError)
	{
		EnsureParsed();
		const std::optional<InputActionLocation> location = s_State.Set.FindAction(action, outError);
		if (!location)
			return std::nullopt;
		return s_State.Set.GetQualifiedName(*location);
	}

	std::optional<uint32_t> InputActions::FindIndex(std::string_view action, std::string* outError)
	{
		const ActionRecord* record = FindRecord(action, outError);
		if (!record)
			return std::nullopt;
		return static_cast<uint32_t>(record - s_State.Actions.data());
	}

	InputActionState InputActions::GetStateAt(uint32_t index)
	{
		if (index >= s_State.Actions.size())
			return {};
		return GetReported(s_State.Actions[index]);
	}

	uint64_t InputActions::GetStateVersion()
	{
		return s_State.StateVersion;
	}

	bool InputActions::SetMapEnabled(std::string_view map, bool enabled, std::string* outError)
	{
		EnsureParsed();
		const std::optional<uint32_t> index = s_State.Set.FindMap(map);
		if (!index)
		{
			if (outError)
				*outError = fmt::format("There is no action map '{}'", StringUtils::MakeValidUTF8(map));
			return false;
		}
		s_State.MapEnabled[*index] = enabled;
		s_State.StateVersion++;
		return true;
	}

	std::optional<bool> InputActions::IsMapEnabled(std::string_view map)
	{
		EnsureParsed();
		const std::optional<uint32_t> index = s_State.Set.FindMap(map);
		if (!index)
			return std::nullopt;
		return s_State.MapEnabled[*index];
	}

	std::optional<std::vector<InputBinding>> InputActions::GetBindings(std::string_view action, std::string* outError)
	{
		const ActionRecord* record = FindRecord(action, outError);
		if (!record)
			return std::nullopt;
		std::vector<InputBinding> bindings;
		for (uint32_t index = record->FirstBinding; index < record->FirstBinding + record->BindingCount; index++)
		{
			const CompiledBinding& compiled = s_State.Bindings[index];
			InputBinding binding;
			binding.Type = compiled.Shape;
			for (uint32_t control = 0; control < binding.GetControlCount(); control++)
				binding.Controls[control] = s_State.Controls[compiled.FirstControl + control];
			bindings.push_back(binding);
		}
		return bindings;
	}

	bool InputActions::SetBindings(std::string_view action, std::span<const InputBinding> bindings, std::string* outError)
	{
		EnsureParsed();
		const std::optional<InputActionLocation> location = s_State.Set.FindAction(action, outError);
		if (!location)
			return false;
		const InputAction& definition = s_State.Set.GetAction(*location);
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		if (bindings.size() > InputActionSet::c_MaxBindings)
			return fail(fmt::format("An action has at most {} bindings ({} given)", InputActionSet::c_MaxBindings, bindings.size()));
		for (const InputBinding& binding : bindings)
		{
			if (!binding.FitsKind(definition.Kind))
				return fail(fmt::format("{} is {} action, which cannot use '{}'", s_State.Set.GetQualifiedName(*location), DescribeInputActionKind(definition.Kind),
					binding.ToString()));
		}
		// One entry per action: an earlier one spelled as the set spelled the action then (the editor may have changed a name's
		// case since) would otherwise stay beside it, and Compile would apply whichever sorts first.
		const std::string qualified = s_State.Set.GetQualifiedName(*location);
		const std::string key = StringUtils::ToLower(qualified);
		std::erase_if(s_State.Players.Actions, [&key](const auto& entry) { return StringUtils::ToLower(entry.first) == key; });
		s_State.Players.Actions[qualified] = std::vector<InputBinding>(bindings.begin(), bindings.end());
		Compile(InputActionSet(s_State.Set), true);
		return true;
	}

	bool InputActions::ResetBindings(std::string_view action, std::string* outError)
	{
		EnsureParsed();
		if (action.empty())
		{
			s_State.Players.Actions.clear();
		}
		else
		{
			const std::optional<InputActionLocation> location = s_State.Set.FindAction(action, outError);
			if (!location)
				return false;
			const std::string key = StringUtils::ToLower(s_State.Set.GetQualifiedName(*location));
			std::erase_if(s_State.Players.Actions, [&key](const auto& entry) { return StringUtils::ToLower(entry.first) == key; });
		}
		Compile(InputActionSet(s_State.Set), true);
		return true;
	}

	InputPlayerBindings InputActions::GetPlayerBindings()
	{
		EnsureParsed();
		InputPlayerBindings applying;
		for (const auto& [name, bindings] : s_State.Players.Actions)
		{
			const std::optional<InputActionLocation> location = s_State.Set.FindAction(name);
			if (!location)
				continue;
			const InputAction& action = s_State.Set.GetAction(*location);
			const bool fits = std::all_of(bindings.begin(), bindings.end(), [&action](const InputBinding& binding) { return binding.FitsKind(action.Kind); });
			if (fits)
				applying.Actions[s_State.Set.GetQualifiedName(*location)] = bindings;
		}
		return applying;
	}

	bool InputActions::SaveBindings(std::string* outError)
	{
		const std::optional<std::filesystem::path> file = GetBindingsFile(outError);
		if (!file)
			return false;
		if (!FileSystem::WriteText(*file, GetPlayerBindings().Write()))
		{
			if (outError)
				*outError = fmt::format("Writing the players' bindings to '{}' failed", FileSystem::ToUTF8(*file));
			return false;
		}
		return true;
	}

	void InputActions::LoadBindings()
	{
		Refresh();
		s_State.Players.Actions.clear();
		const std::optional<std::filesystem::path> file = GetBindingsFile(nullptr);
		if (file && FileSystem::Exists(*file))
		{
			const std::string path = FileSystem::ToUTF8(*file);
			const std::optional<std::vector<uint8_t>> bytes = FileSystem::ReadBytesPrefix(*file, InputPlayerBindings::c_MaxFileBytes + 1);
			std::string error;
			std::vector<std::string> dropped;
			std::optional<InputPlayerBindings> players;
			if (!bytes)
				error = "it cannot be read";
			else
				players = InputPlayerBindings::Read(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), s_State.Set, &dropped, &error);
			if (players)
				s_State.Players = std::move(*players);
			else
				ST_CORE_WARN("The players' input bindings '{}' are ignored ({}): the project's bindings apply", path, error);
			if (!dropped.empty())
			{
				ST_CORE_WARN("The players' input bindings '{}' name what the game's actions no longer have ({} dropped; the project's bindings apply "
					"there): {}", path, dropped.size(), dropped.front());
			}
		}
		Compile(InputActionSet(s_State.Set), true);
	}

	bool InputActions::SimulateAction(std::string_view action, bool down, const glm::vec2& value, std::string* outError)
	{
		const ActionRecord* record = FindRecord(action, outError);
		if (!record)
			return false;
		glm::vec2 clamped(0.0f);
		if (std::isfinite(value.x) && std::isfinite(value.y))
			clamped = glm::clamp(value, glm::vec2(-1.0f), glm::vec2(1.0f));
		s_State.SimulatedQueue.push_back({ static_cast<uint32_t>(record - s_State.Actions.data()), down, clamped });
		return true;
	}

	void InputActions::ReleaseAllSimulated()
	{
		s_State.SimulatedQueue.clear();
		for (uint32_t index = 0; index < s_State.Actions.size(); index++)
		{
			if (s_State.Actions[index].SimulatedDown)
				s_State.SimulatedQueue.push_back({ index, false, glm::vec2(0.0f) });
		}
	}

	void InputActions::ClearSimulated()
	{
		s_State.SimulatedQueue.clear();
		for (ActionRecord& record : s_State.Actions)
		{
			record.SimulatedDown = false;
			record.SimulatedPressed = false;
		}
	}

	bool InputActions::BeginCapture(const InputCaptureDevices& devices)
	{
		if (!devices.Keyboard && !devices.Mouse && !devices.Gamepad)
			return false;
		EnsureParsed();
		s_State.StateVersion++;
		s_State.Capture = CaptureState();
		s_State.Capture.Active = true;
		s_State.Capture.Devices = devices;
		UpdateCapture(true);
		return true;
	}

	bool InputActions::IsCapturing()
	{
		return s_State.Capture.Active;
	}

	std::optional<std::string> InputActions::TakeCapturedBinding()
	{
		std::optional<std::string> result = std::move(s_State.Capture.Result);
		s_State.Capture.Result.reset();
		return result;
	}

	void InputActions::CancelCapture()
	{
		s_State.StateVersion++;
		s_State.Capture = CaptureState();
	}

}
