#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Strata
{

	// The actions the game UI reads (focus navigation, clicking, going back): the reserved map UI of the game's input actions
	// (InputActionSet), which players can rebind like any other. Their default bindings are the keyboard's Enter, Escape and
	// arrows and every gamepad's A, B, d-pad and left stick (InputActionSet::GetDefaultUIActions).
	enum class UIAction : uint8_t
	{
		Submit = 0,
		Cancel,
		Up,
		Down,
		Left,
		Right
	};
	constexpr uint32_t c_UIActionCount = 6;

	const char* GetUIActionName(UIAction action);
	// The action's input action name: "UI/<name>".
	std::string_view GetUIActionQualifiedName(UIAction action);

	// The UI actions of the current input frame, read from InputActions (the UI map, by qualified name, so a game's own Left
	// never interferes) once per update: down, pressed (this update) and repeated (pressed, then again after c_RepeatDelay
	// seconds held and every c_RepeatInterval after that). Deterministic for given input and deltas (the game UI passes
	// unscaled time, so menus repeat at time scale 0). Main thread only.
	class UIActionState
	{
	public:
		static constexpr float c_RepeatDelay = 0.4f;
		static constexpr float c_RepeatInterval = 0.1f;

		void Update(float deltaSeconds);
		void Reset();

		bool IsDown(UIAction action) const { return m_Actions[Index(action)].Down; }
		bool IsPressed(UIAction action) const { return m_Actions[Index(action)].Pressed; }
		bool IsRepeated(UIAction action) const { return m_Actions[Index(action)].Repeated; }
	private:
		struct ActionRecord
		{
			bool Down = false;
			bool Pressed = false;
			bool Repeated = false;
			float HeldTime = 0.0f;
			float NextRepeat = 0.0f;
		};

		static size_t Index(UIAction action) { return static_cast<size_t>(action); }
	private:
		std::array<ActionRecord, c_UIActionCount> m_Actions = {};
	};

}
