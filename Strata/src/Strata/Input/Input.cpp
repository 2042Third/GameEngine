#include "stpch.h"
#include "Strata/Input/Input.h"

#include "Strata/Input/InputWindow.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <string>
#include <vector>

namespace Strata
{

	namespace
	{

		// Keys or mouse buttons of one source (the devices, or simulated input).
		template<size_t Count>
		struct ButtonStates
		{
			std::bitset<Count> Down;
			std::bitset<Count> Pressed;
			std::bitset<Count> Released;

			void Set(size_t index, bool down)
			{
				if (down && !Down.test(index))
					Pressed.set(index);
				else if (!down && Down.test(index))
					Released.set(index);
				Down.set(index, down);
			}

			void ClearTransitions()
			{
				Pressed.reset();
				Released.reset();
			}
		};

		struct GamepadState
		{
			bool Connected = false;
			float Axes[c_GamepadAxisCount] = {};
			ButtonStates<c_GamepadButtonCount> Buttons;
		};

		// A gamepad of simulated input: connected once used, with the axes it holds.
		struct SimulatedGamepad
		{
			bool Connected = false;
			ButtonStates<c_GamepadButtonCount> Buttons;
			std::optional<float> Axes[c_GamepadAxisCount];
		};

		enum class SimulatedEventType : uint8_t
		{
			Key = 0,
			MouseButton,
			MouseMove,
			Scroll,
			GamepadButton,
			GamepadAxis,
			GamepadAxisRelease,
			Text
		};

		// The text typed during an input frame: whole characters up to Input::c_MaxTypedTextBytes; once one does not fit, the
		// rest of the frame's text is dropped too, so what is kept is what was typed first.
		struct FrameText
		{
			std::string Bytes;
			bool Full = false;

			void Clear()
			{
				Bytes.clear();
				Full = false;
			}
		};

		struct SimulatedEvent
		{
			SimulatedEventType Type = SimulatedEventType::Key;
			uint16_t Code = 0; // Key, mouse button, gamepad button or gamepad axis
			bool Down = false;
			glm::vec2 Value = { 0.0f, 0.0f }; // Pointer position, scroll offset, or (x) gamepad axis value
			uint8_t Gamepad = 0;
			std::string Text; // Typed text (UTF-8)
		};

		struct SimulatedState
		{
			ButtonStates<c_MaxKeyCode> Keys;
			ButtonStates<c_MaxMouseButtons> MouseButtons;
			std::optional<glm::vec2> MousePosition; // Relative to the input viewport
			glm::vec2 MouseDelta = { 0.0f, 0.0f };
			glm::vec2 ScrollDelta = { 0.0f, 0.0f };
			SimulatedGamepad Gamepads[c_MaxGamepads];
			FrameText TypedText;               // Of the current input frame
			std::vector<SimulatedEvent> Queue; // Applied by the next BeginFrame
		};

		struct InputState
		{
			ButtonStates<c_MaxKeyCode> Keys;
			ButtonStates<c_MaxMouseButtons> MouseButtons;

			glm::vec2 MousePosition = { 0.0f, 0.0f };
			glm::vec2 MouseDelta = { 0.0f, 0.0f };
			glm::vec2 ScrollDelta = { 0.0f, 0.0f };
			bool HasMousePosition = false;

			GamepadState Gamepads[c_MaxGamepads];
			FrameText TypedText; // The devices' text of the current input frame

			SimulatedState Simulated;
			bool PointerCaptured = false;

			bool Suspended = false;
			bool FrameSeen = true; // Whether the game has had the current transitions (false once suspended frames collect them)
			uint64_t FrameIndex = 0;
			bool Enabled = true;
			glm::vec2 ViewportOrigin = { 0.0f, 0.0f };
			glm::vec2 ViewportSize = { 0.0f, 0.0f };
			CursorMode Cursor = CursorMode::Normal;     // Last set on the window
			CursorMode GameCursor = CursorMode::Normal; // What the game asks for (SetGameCursorMode)
			InputWindow* TargetWindow = nullptr;
		};

		InputState s_State;

		// Whether a source held a button through the whole input frame (down, without a transition).
		template<size_t Count>
		bool IsHeldThroughout(const ButtonStates<Count>& states, size_t index)
		{
			return states.Down.test(index) && !states.Pressed.test(index) && !states.Released.test(index);
		}

		// The game sees the devices (only while input is enabled) and simulated input merged: a button is down while either source
		// holds it.
		template<size_t Count>
		bool IsMergedDown(const ButtonStates<Count>& device, const ButtonStates<Count>& simulated, size_t index)
		{
			return index < Count && ((s_State.Enabled && device.Down.test(index)) || simulated.Down.test(index));
		}

		// A press or release of one source is a transition of the merged button only if the other source did not hold the button
		// through the frame: a tool pressing or letting go of a key a person holds (or the other way round) changes nothing the game
		// sees, and never restarts a key repeat.
		template<size_t Count>
		bool IsMergedTransition(std::bitset<Count> ButtonStates<Count>::* transition, const ButtonStates<Count>& device, const ButtonStates<Count>& simulated,
			size_t index)
		{
			if (index >= Count)
				return false;
			const bool deviceTransition = s_State.Enabled && (device.*transition).test(index);
			const bool deviceHeld = s_State.Enabled && IsHeldThroughout(device, index);
			const bool simulatedTransition = (simulated.*transition).test(index);
			return (deviceTransition && !IsHeldThroughout(simulated, index)) || (simulatedTransition && !deviceHeld);
		}

		bool IsFinite(const glm::vec2& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
		}

		// Appends a code point's UTF-8 to a frame's text while it fits within Input::c_MaxTypedTextBytes.
		void AppendCodepoint(FrameText& text, uint32_t codepoint)
		{
			if (text.Full)
				return;
			char bytes[4];
			size_t length = 0;
			if (codepoint < 0x80)
			{
				bytes[length++] = static_cast<char>(codepoint);
			}
			else if (codepoint < 0x800)
			{
				bytes[length++] = static_cast<char>(0xC0 | (codepoint >> 6));
				bytes[length++] = static_cast<char>(0x80 | (codepoint & 0x3F));
			}
			else if (codepoint < 0x10000)
			{
				bytes[length++] = static_cast<char>(0xE0 | (codepoint >> 12));
				bytes[length++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
				bytes[length++] = static_cast<char>(0x80 | (codepoint & 0x3F));
			}
			else
			{
				bytes[length++] = static_cast<char>(0xF0 | (codepoint >> 18));
				bytes[length++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
				bytes[length++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
				bytes[length++] = static_cast<char>(0x80 | (codepoint & 0x3F));
			}
			if (text.Bytes.size() + length <= Input::c_MaxTypedTextBytes)
				text.Bytes.append(bytes, length);
			else
				text.Full = true;
		}

		// Whether a code point is typed text: not a control character, a surrogate or beyond Unicode.
		bool IsTextCodepoint(uint32_t codepoint)
		{
			return codepoint >= 0x20 && codepoint != 0x7F && !(codepoint >= 0x80 && codepoint < 0xA0) && !(codepoint >= 0xD800 && codepoint <= 0xDFFF)
				&& codepoint <= 0x10FFFF;
		}

		// Decodes UTF-8 (malformed sequences, overlong encodings and surrogates as U+FFFD) and appends the text's characters.
		void AppendUTF8(FrameText& text, std::string_view utf8)
		{
			size_t index = 0;
			while (index < utf8.size())
			{
				const uint8_t lead = static_cast<uint8_t>(utf8[index]);
				uint32_t length = 0;
				uint32_t codepoint = 0;
				uint32_t minimum = 0;
				if (lead < 0x80)
				{
					length = 1;
					codepoint = lead;
				}
				else if ((lead & 0xE0) == 0xC0)
				{
					length = 2;
					codepoint = lead & 0x1F;
					minimum = 0x80;
				}
				else if ((lead & 0xF0) == 0xE0)
				{
					length = 3;
					codepoint = lead & 0x0F;
					minimum = 0x800;
				}
				else if ((lead & 0xF8) == 0xF0)
				{
					length = 4;
					codepoint = lead & 0x07;
					minimum = 0x10000;
				}
				else
				{
					AppendCodepoint(text, 0xFFFD); // A continuation byte or an invalid lead
					index++;
					continue;
				}
				// A truncated sequence replaces only the bytes that belong to it.
				uint32_t consumed = 1;
				bool valid = true;
				for (; consumed < length; consumed++)
				{
					if (index + consumed >= utf8.size() || (static_cast<uint8_t>(utf8[index + consumed]) & 0xC0) != 0x80)
					{
						valid = false;
						break;
					}
					codepoint = (codepoint << 6) | (static_cast<uint8_t>(utf8[index + consumed]) & 0x3F);
				}
				if (!valid || codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF))
					codepoint = 0xFFFD;
				if (IsTextCodepoint(codepoint))
					AppendCodepoint(text, codepoint);
				index += consumed;
			}
		}

		void ApplyCursorMode(CursorMode mode)
		{
			s_State.Cursor = mode;
			if (s_State.TargetWindow)
				s_State.TargetWindow->SetCursorMode(mode);
		}

		// The value of an axis at rest: 0 for the sticks, the minimum (released) for the triggers.
		float GetRestValue(GamepadAxis axis)
		{
			return std::max(GetGamepadAxisMinimum(axis), 0.0f);
		}

		void ApplySimulatedEvent(SimulatedState& state, const SimulatedEvent& event)
		{
			switch (event.Type)
			{
				case SimulatedEventType::Key:
					state.Keys.Set(event.Code, event.Down);
					break;
				case SimulatedEventType::MouseButton:
					state.MouseButtons.Set(event.Code, event.Down);
					break;
				case SimulatedEventType::GamepadButton:
					state.Gamepads[event.Gamepad].Connected = true;
					state.Gamepads[event.Gamepad].Buttons.Set(event.Code, event.Down);
					break;
				case SimulatedEventType::GamepadAxis:
					state.Gamepads[event.Gamepad].Connected = true;
					state.Gamepads[event.Gamepad].Axes[event.Code] = event.Value.x;
					break;
				case SimulatedEventType::GamepadAxisRelease:
					state.Gamepads[event.Gamepad].Axes[event.Code].reset();
					break;
				case SimulatedEventType::MouseMove:
					// Like a device, the first position only establishes where the pointer is.
					if (state.MousePosition)
						state.MouseDelta += event.Value - *state.MousePosition;
					state.MousePosition = event.Value;
					break;
				case SimulatedEventType::Scroll:
					state.ScrollDelta += event.Value;
					break;
				case SimulatedEventType::Text:
					AppendUTF8(state.TypedText, event.Text);
					break;
			}
		}

	}

	bool Input::IsKeyDown(KeyCode key)
	{
		return IsMergedDown(s_State.Keys, s_State.Simulated.Keys, key);
	}

	bool Input::IsKeyPressed(KeyCode key)
	{
		return IsMergedTransition(&ButtonStates<c_MaxKeyCode>::Pressed, s_State.Keys, s_State.Simulated.Keys, key);
	}

	bool Input::IsKeyReleased(KeyCode key)
	{
		return IsMergedTransition(&ButtonStates<c_MaxKeyCode>::Released, s_State.Keys, s_State.Simulated.Keys, key);
	}

	bool Input::IsMouseButtonDown(MouseCode button)
	{
		return !s_State.PointerCaptured && IsMergedDown(s_State.MouseButtons, s_State.Simulated.MouseButtons, button);
	}

	bool Input::IsMouseButtonPressed(MouseCode button)
	{
		return !s_State.PointerCaptured && IsMergedTransition(&ButtonStates<c_MaxMouseButtons>::Pressed, s_State.MouseButtons, s_State.Simulated.MouseButtons, button);
	}

	bool Input::IsMouseButtonReleased(MouseCode button)
	{
		return !s_State.PointerCaptured && IsMergedTransition(&ButtonStates<c_MaxMouseButtons>::Released, s_State.MouseButtons, s_State.Simulated.MouseButtons, button);
	}

	glm::vec2 Input::GetMousePosition()
	{
		if (s_State.Simulated.MousePosition)
			return *s_State.Simulated.MousePosition;
		return s_State.MousePosition - s_State.ViewportOrigin;
	}

	glm::vec2 Input::GetMouseDelta()
	{
		return (s_State.Enabled ? s_State.MouseDelta : glm::vec2(0.0f)) + s_State.Simulated.MouseDelta;
	}

	glm::vec2 Input::GetScrollDelta()
	{
		if (s_State.PointerCaptured)
			return glm::vec2(0.0f);
		return (s_State.Enabled ? s_State.ScrollDelta : glm::vec2(0.0f)) + s_State.Simulated.ScrollDelta;
	}

	std::string Input::GetTypedText()
	{
		std::string text = s_State.Enabled ? s_State.TypedText.Bytes : std::string();
		// Together within the frame's limit: simulated text after the devices', whole characters only.
		const std::string& simulated = s_State.Simulated.TypedText.Bytes;
		size_t index = 0;
		while (index < simulated.size())
		{
			const uint8_t lead = static_cast<uint8_t>(simulated[index]);
			const size_t length = lead < 0x80 ? 1 : ((lead & 0xE0) == 0xC0 ? 2 : ((lead & 0xF0) == 0xE0 ? 3 : 4));
			if (text.size() + length > c_MaxTypedTextBytes)
				break;
			text.append(simulated, index, length);
			index += length;
		}
		return text;
	}

	void Input::SetPointerCapture(bool captured)
	{
		s_State.PointerCaptured = captured;
	}

	bool Input::IsPointerCaptured()
	{
		return s_State.PointerCaptured;
	}

	PointerState Input::GetPointerState()
	{
		PointerState state;
		state.Position = GetMousePosition();
		state.ScrollDelta = (s_State.Enabled ? s_State.ScrollDelta : glm::vec2(0.0f)) + s_State.Simulated.ScrollDelta;
		for (MouseCode button = 0; button < c_MaxMouseButtons; button++)
		{
			const uint32_t bit = 1u << button;
			if (IsMergedDown(s_State.MouseButtons, s_State.Simulated.MouseButtons, button))
				state.Down |= bit;
			if (IsMergedTransition(&ButtonStates<c_MaxMouseButtons>::Pressed, s_State.MouseButtons, s_State.Simulated.MouseButtons, button))
				state.Pressed |= bit;
			if (IsMergedTransition(&ButtonStates<c_MaxMouseButtons>::Released, s_State.MouseButtons, s_State.Simulated.MouseButtons, button))
				state.Released |= bit;
		}
		return state;
	}

	bool Input::IsGamepadConnected(uint32_t gamepad)
	{
		return gamepad < c_MaxGamepads && (s_State.Gamepads[gamepad].Connected || s_State.Simulated.Gamepads[gamepad].Connected);
	}

	bool Input::IsGamepadButtonDown(uint32_t gamepad, GamepadButton button)
	{
		if (gamepad >= c_MaxGamepads)
			return false;
		return IsMergedDown(s_State.Gamepads[gamepad].Buttons, s_State.Simulated.Gamepads[gamepad].Buttons, static_cast<size_t>(button));
	}

	bool Input::IsGamepadButtonPressed(uint32_t gamepad, GamepadButton button)
	{
		if (gamepad >= c_MaxGamepads)
			return false;
		return IsMergedTransition(&ButtonStates<c_GamepadButtonCount>::Pressed, s_State.Gamepads[gamepad].Buttons, s_State.Simulated.Gamepads[gamepad].Buttons,
			static_cast<size_t>(button));
	}

	bool Input::IsGamepadButtonReleased(uint32_t gamepad, GamepadButton button)
	{
		if (gamepad >= c_MaxGamepads)
			return false;
		return IsMergedTransition(&ButtonStates<c_GamepadButtonCount>::Released, s_State.Gamepads[gamepad].Buttons, s_State.Simulated.Gamepads[gamepad].Buttons,
			static_cast<size_t>(button));
	}

	float Input::GetGamepadAxis(uint32_t gamepad, GamepadAxis axis)
	{
		const uint32_t index = static_cast<uint32_t>(axis);
		if (gamepad >= c_MaxGamepads || index >= c_GamepadAxisCount)
			return 0.0f;
		// An axis simulated input holds wins over the device's, like a tool taking the stick.
		if (const std::optional<float>& simulated = s_State.Simulated.Gamepads[gamepad].Axes[index])
			return *simulated;
		return s_State.Enabled ? s_State.Gamepads[gamepad].Axes[index] : GetRestValue(axis);
	}

	void Input::SetCursorMode(CursorMode mode)
	{
		ApplyCursorMode(mode);
	}

	CursorMode Input::GetCursorMode()
	{
		return s_State.Cursor;
	}

	void Input::SetGameCursorMode(CursorMode mode)
	{
		s_State.GameCursor = mode;
		if (s_State.Enabled)
			ApplyCursorMode(mode);
	}

	CursorMode Input::GetGameCursorMode()
	{
		return s_State.GameCursor;
	}

	glm::vec2 Input::GetViewportSize()
	{
		if (s_State.ViewportSize.x > 0.0f && s_State.ViewportSize.y > 0.0f)
			return s_State.ViewportSize;
		if (s_State.TargetWindow)
			return glm::vec2(s_State.TargetWindow->GetWidth(), s_State.TargetWindow->GetHeight());
		return glm::vec2(0.0f);
	}

	void Input::BeginFrame()
	{
		// Transitions and movement are cleared once the game has had them (the first BeginFrame after a frame it updated in);
		// while input frames are suspended, what happens meanwhile accumulates for the game's next update.
		if (s_State.FrameSeen)
		{
			s_State.Keys.ClearTransitions();
			s_State.MouseButtons.ClearTransitions();
			s_State.MouseDelta = glm::vec2(0.0f);
			s_State.ScrollDelta = glm::vec2(0.0f);
			s_State.TypedText.Clear();
			for (GamepadState& gamepad : s_State.Gamepads)
				gamepad.Buttons.ClearTransitions();

			SimulatedState& simulated = s_State.Simulated;
			simulated.Keys.ClearTransitions();
			simulated.MouseButtons.ClearTransitions();
			for (SimulatedGamepad& gamepad : simulated.Gamepads)
				gamepad.Buttons.ClearTransitions();
			simulated.MouseDelta = glm::vec2(0.0f);
			simulated.ScrollDelta = glm::vec2(0.0f);
			simulated.TypedText.Clear();
			s_State.FrameSeen = false;
		}
		if (s_State.Suspended)
			return;

		s_State.FrameIndex++;
		SimulatedState& simulated = s_State.Simulated;
		for (const SimulatedEvent& event : simulated.Queue)
			ApplySimulatedEvent(simulated, event);
		simulated.Queue.clear();
		s_State.FrameSeen = true; // The game updates in this frame
	}

	void Input::Reset()
	{
		InputWindow* window = s_State.TargetWindow;
		s_State = InputState();
		s_State.TargetWindow = window;
	}

	void Input::SetSuspended(bool suspended)
	{
		s_State.Suspended = suspended;
	}

	bool Input::IsSuspended()
	{
		return s_State.Suspended;
	}

	uint64_t Input::GetFrameIndex()
	{
		return s_State.FrameIndex;
	}

	void Input::SetEnabled(bool enabled)
	{
		if (enabled == s_State.Enabled)
			return;
		s_State.Enabled = enabled;
		// The game's cursor applies while the game has the input (see SetGameCursorMode).
		if (s_State.GameCursor != CursorMode::Normal)
			ApplyCursorMode(enabled ? s_State.GameCursor : CursorMode::Normal);
	}

	bool Input::IsEnabled()
	{
		return s_State.Enabled;
	}

	void Input::SetWindow(InputWindow* window)
	{
		s_State.TargetWindow = window;
	}

	void Input::SetViewport(const glm::vec2& origin, const glm::vec2& size)
	{
		s_State.ViewportOrigin = origin;
		s_State.ViewportSize = size;
	}

	void Input::ProcessKey(KeyCode key, bool down)
	{
		if (key < c_MaxKeyCode)
			s_State.Keys.Set(key, down);
	}

	void Input::ProcessMouseButton(MouseCode button, bool down)
	{
		if (button < c_MaxMouseButtons)
			s_State.MouseButtons.Set(button, down);
	}

	void Input::ProcessMouseMove(const glm::vec2& windowPosition)
	{
		if (s_State.HasMousePosition)
			s_State.MouseDelta += windowPosition - s_State.MousePosition;
		s_State.MousePosition = windowPosition;
		s_State.HasMousePosition = true;
	}

	void Input::ProcessScroll(const glm::vec2& offset)
	{
		s_State.ScrollDelta += offset;
	}

	void Input::ProcessGamepad(uint32_t gamepad, bool connected, const float* axes, const bool* buttons)
	{
		if (gamepad >= c_MaxGamepads)
			return;

		GamepadState& state = s_State.Gamepads[gamepad];
		state.Connected = connected;
		for (uint32_t index = 0; index < c_GamepadAxisCount; index++)
		{
			// At rest without a device; finite values within the axis' range otherwise.
			const GamepadAxis axis = static_cast<GamepadAxis>(index);
			const float value = connected && axes && std::isfinite(axes[index]) ? axes[index] : GetRestValue(axis);
			state.Axes[index] = std::clamp(value, GetGamepadAxisMinimum(axis), 1.0f);
		}
		for (uint32_t index = 0; index < c_GamepadButtonCount; index++)
			state.Buttons.Set(index, connected && buttons ? buttons[index] : false);
	}

	void Input::ProcessText(uint32_t codepoint)
	{
		if (IsTextCodepoint(codepoint))
			AppendCodepoint(s_State.TypedText, codepoint);
	}

	void Input::SimulateKey(KeyCode key, bool down)
	{
		if (key < c_MaxKeyCode)
			s_State.Simulated.Queue.push_back({ SimulatedEventType::Key, key, down, glm::vec2(0.0f) });
	}

	void Input::SimulateMouseButton(MouseCode button, bool down)
	{
		if (button < c_MaxMouseButtons)
			s_State.Simulated.Queue.push_back({ SimulatedEventType::MouseButton, button, down, glm::vec2(0.0f) });
	}

	void Input::SimulateMouseMove(const glm::vec2& viewportPosition)
	{
		if (IsFinite(viewportPosition))
			s_State.Simulated.Queue.push_back({ SimulatedEventType::MouseMove, 0, false, viewportPosition });
	}

	void Input::SimulateScroll(const glm::vec2& offset)
	{
		if (IsFinite(offset))
			s_State.Simulated.Queue.push_back({ SimulatedEventType::Scroll, 0, false, offset });
	}

	void Input::SimulateText(std::string_view utf8)
	{
		if (utf8.empty())
			return;
		SimulatedEvent event;
		event.Type = SimulatedEventType::Text;
		event.Text = std::string(utf8);
		s_State.Simulated.Queue.push_back(std::move(event));
	}

	void Input::SimulateGamepadButton(uint32_t gamepad, GamepadButton button, bool down)
	{
		if (gamepad < c_MaxGamepads && static_cast<uint32_t>(button) < c_GamepadButtonCount)
			s_State.Simulated.Queue.push_back({ SimulatedEventType::GamepadButton, static_cast<uint16_t>(button), down, glm::vec2(0.0f), static_cast<uint8_t>(gamepad) });
	}

	void Input::SimulateGamepadAxis(uint32_t gamepad, GamepadAxis axis, float value)
	{
		if (gamepad >= c_MaxGamepads || static_cast<uint32_t>(axis) >= c_GamepadAxisCount || !std::isfinite(value))
			return;
		const float clamped = std::clamp(value, GetGamepadAxisMinimum(axis), 1.0f);
		s_State.Simulated.Queue.push_back({ SimulatedEventType::GamepadAxis, static_cast<uint16_t>(axis), false, glm::vec2(clamped, 0.0f), static_cast<uint8_t>(gamepad) });
	}

	void Input::ReleaseSimulatedGamepadAxis(uint32_t gamepad, GamepadAxis axis)
	{
		if (gamepad < c_MaxGamepads && static_cast<uint32_t>(axis) < c_GamepadAxisCount)
			s_State.Simulated.Queue.push_back({ SimulatedEventType::GamepadAxisRelease, static_cast<uint16_t>(axis), false, glm::vec2(0.0f), static_cast<uint8_t>(gamepad) });
	}

	void Input::ReleaseAllSimulated()
	{
		std::vector<SimulatedEvent>& queue = s_State.Simulated.Queue;
		queue.erase(std::remove_if(queue.begin(), queue.end(), [](const SimulatedEvent& event)
		{
			return event.Type == SimulatedEventType::Key || event.Type == SimulatedEventType::MouseButton || event.Type == SimulatedEventType::GamepadButton
				|| event.Type == SimulatedEventType::GamepadAxis;
		}), queue.end());
		for (KeyCode key = 0; key < c_MaxKeyCode; key++)
		{
			if (s_State.Simulated.Keys.Down.test(key))
				SimulateKey(key, false);
		}
		for (MouseCode button = 0; button < c_MaxMouseButtons; button++)
		{
			if (s_State.Simulated.MouseButtons.Down.test(button))
				SimulateMouseButton(button, false);
		}
		for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
		{
			const SimulatedGamepad& state = s_State.Simulated.Gamepads[gamepad];
			for (uint32_t button = 0; button < c_GamepadButtonCount; button++)
			{
				if (state.Buttons.Down.test(button))
					SimulateGamepadButton(gamepad, static_cast<GamepadButton>(button), false);
			}
			for (uint32_t axis = 0; axis < c_GamepadAxisCount; axis++)
			{
				if (state.Axes[axis])
					ReleaseSimulatedGamepadAxis(gamepad, static_cast<GamepadAxis>(axis));
			}
		}
	}

	void Input::ClearSimulated()
	{
		s_State.Simulated = SimulatedState();
	}

	bool Input::HasQueuedSimulatedInput()
	{
		return !s_State.Simulated.Queue.empty();
	}

	bool Input::IsSimulatedKeyDown(KeyCode key)
	{
		return key < c_MaxKeyCode && s_State.Simulated.Keys.Down.test(key);
	}

	bool Input::IsSimulatedMouseButtonDown(MouseCode button)
	{
		return button < c_MaxMouseButtons && s_State.Simulated.MouseButtons.Down.test(button);
	}

	std::optional<glm::vec2> Input::GetSimulatedMousePosition()
	{
		return s_State.Simulated.MousePosition;
	}

	bool Input::IsSimulatedGamepadConnected(uint32_t gamepad)
	{
		return gamepad < c_MaxGamepads && s_State.Simulated.Gamepads[gamepad].Connected;
	}

	bool Input::IsSimulatedGamepadButtonDown(uint32_t gamepad, GamepadButton button)
	{
		return gamepad < c_MaxGamepads && static_cast<uint32_t>(button) < c_GamepadButtonCount
			&& s_State.Simulated.Gamepads[gamepad].Buttons.Down.test(static_cast<size_t>(button));
	}

	std::optional<float> Input::GetSimulatedGamepadAxis(uint32_t gamepad, GamepadAxis axis)
	{
		if (gamepad >= c_MaxGamepads || static_cast<uint32_t>(axis) >= c_GamepadAxisCount)
			return std::nullopt;
		return s_State.Simulated.Gamepads[gamepad].Axes[static_cast<size_t>(axis)];
	}

}
