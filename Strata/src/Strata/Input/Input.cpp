#include "stpch.h"
#include "Strata/Input/Input.h"

#include "Strata/Core/Window.h"

#include <bitset>
#include <cmath>
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
			bool Buttons[c_GamepadButtonCount] = {};
			bool PreviousButtons[c_GamepadButtonCount] = {};
		};

		enum class SimulatedEventType : uint8_t
		{
			Key = 0,
			MouseButton,
			MouseMove,
			Scroll
		};

		struct SimulatedEvent
		{
			SimulatedEventType Type = SimulatedEventType::Key;
			uint16_t Code = 0; // Key or mouse button
			bool Down = false;
			glm::vec2 Value = { 0.0f, 0.0f }; // Pointer position or scroll offset
		};

		struct SimulatedState
		{
			ButtonStates<c_MaxKeyCode> Keys;
			ButtonStates<c_MaxMouseButtons> MouseButtons;
			std::optional<glm::vec2> MousePosition; // Relative to the input viewport
			glm::vec2 MouseDelta = { 0.0f, 0.0f };
			glm::vec2 ScrollDelta = { 0.0f, 0.0f };
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

			SimulatedState Simulated;

			bool Enabled = true;
			glm::vec2 ViewportOrigin = { 0.0f, 0.0f };
			glm::vec2 ViewportSize = { 0.0f, 0.0f };
			CursorMode Cursor = CursorMode::Normal;
			Window* TargetWindow = nullptr;
		};

		InputState s_State;

		// A query over both sources: the devices count only while input is enabled, simulated input always.
		template<size_t Count>
		bool Query(std::bitset<Count> ButtonStates<Count>::* states, const ButtonStates<Count>& device, const ButtonStates<Count>& simulated, size_t index)
		{
			if (index >= Count)
				return false;
			return (s_State.Enabled && (device.*states).test(index)) || (simulated.*states).test(index);
		}

		bool IsFinite(const glm::vec2& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
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
				case SimulatedEventType::MouseMove:
					// Like a device, the first position only establishes where the pointer is.
					if (state.MousePosition)
						state.MouseDelta += event.Value - *state.MousePosition;
					state.MousePosition = event.Value;
					break;
				case SimulatedEventType::Scroll:
					state.ScrollDelta += event.Value;
					break;
			}
		}

	}

	bool Input::IsKeyDown(KeyCode key)
	{
		return Query(&ButtonStates<c_MaxKeyCode>::Down, s_State.Keys, s_State.Simulated.Keys, key);
	}

	bool Input::IsKeyPressed(KeyCode key)
	{
		return Query(&ButtonStates<c_MaxKeyCode>::Pressed, s_State.Keys, s_State.Simulated.Keys, key);
	}

	bool Input::IsKeyReleased(KeyCode key)
	{
		return Query(&ButtonStates<c_MaxKeyCode>::Released, s_State.Keys, s_State.Simulated.Keys, key);
	}

	bool Input::IsMouseButtonDown(MouseCode button)
	{
		return Query(&ButtonStates<c_MaxMouseButtons>::Down, s_State.MouseButtons, s_State.Simulated.MouseButtons, button);
	}

	bool Input::IsMouseButtonPressed(MouseCode button)
	{
		return Query(&ButtonStates<c_MaxMouseButtons>::Pressed, s_State.MouseButtons, s_State.Simulated.MouseButtons, button);
	}

	bool Input::IsMouseButtonReleased(MouseCode button)
	{
		return Query(&ButtonStates<c_MaxMouseButtons>::Released, s_State.MouseButtons, s_State.Simulated.MouseButtons, button);
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
		return (s_State.Enabled ? s_State.ScrollDelta : glm::vec2(0.0f)) + s_State.Simulated.ScrollDelta;
	}

	bool Input::IsGamepadConnected(uint32_t gamepad)
	{
		return gamepad < c_MaxGamepads && s_State.Gamepads[gamepad].Connected;
	}

	bool Input::IsGamepadButtonDown(uint32_t gamepad, GamepadButton button)
	{
		const uint32_t index = static_cast<uint32_t>(button);
		if (!s_State.Enabled || gamepad >= c_MaxGamepads || index >= c_GamepadButtonCount)
			return false;
		return s_State.Gamepads[gamepad].Buttons[index];
	}

	bool Input::IsGamepadButtonPressed(uint32_t gamepad, GamepadButton button)
	{
		const uint32_t index = static_cast<uint32_t>(button);
		if (!s_State.Enabled || gamepad >= c_MaxGamepads || index >= c_GamepadButtonCount)
			return false;
		const GamepadState& state = s_State.Gamepads[gamepad];
		return state.Buttons[index] && !state.PreviousButtons[index];
	}

	float Input::GetGamepadAxis(uint32_t gamepad, GamepadAxis axis)
	{
		const uint32_t index = static_cast<uint32_t>(axis);
		if (!s_State.Enabled || gamepad >= c_MaxGamepads || index >= c_GamepadAxisCount)
			return 0.0f;
		return s_State.Gamepads[gamepad].Axes[index];
	}

	void Input::SetCursorMode(CursorMode mode)
	{
		s_State.Cursor = mode;
		if (s_State.TargetWindow)
			s_State.TargetWindow->SetCursorMode(mode);
	}

	CursorMode Input::GetCursorMode()
	{
		return s_State.Cursor;
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
		s_State.Keys.ClearTransitions();
		s_State.MouseButtons.ClearTransitions();
		s_State.MouseDelta = glm::vec2(0.0f);
		s_State.ScrollDelta = glm::vec2(0.0f);
		for (GamepadState& gamepad : s_State.Gamepads)
			std::copy(std::begin(gamepad.Buttons), std::end(gamepad.Buttons), std::begin(gamepad.PreviousButtons));

		SimulatedState& simulated = s_State.Simulated;
		simulated.Keys.ClearTransitions();
		simulated.MouseButtons.ClearTransitions();
		simulated.MouseDelta = glm::vec2(0.0f);
		simulated.ScrollDelta = glm::vec2(0.0f);
		for (const SimulatedEvent& event : simulated.Queue)
			ApplySimulatedEvent(simulated, event);
		simulated.Queue.clear();
	}

	void Input::Reset()
	{
		Window* window = s_State.TargetWindow;
		s_State = InputState();
		s_State.TargetWindow = window;
	}

	void Input::SetEnabled(bool enabled)
	{
		s_State.Enabled = enabled;
	}

	bool Input::IsEnabled()
	{
		return s_State.Enabled;
	}

	void Input::SetWindow(Window* window)
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
			state.Axes[index] = connected && axes ? axes[index] : 0.0f;
		for (uint32_t index = 0; index < c_GamepadButtonCount; index++)
			state.Buttons[index] = connected && buttons ? buttons[index] : false;
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

	void Input::ClearSimulated()
	{
		s_State.Simulated = SimulatedState();
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

}
