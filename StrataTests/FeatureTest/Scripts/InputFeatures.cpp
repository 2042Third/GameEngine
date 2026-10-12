// Input: keyboard, mouse buttons, mouse position and movement, scrolling, gamepads and the cursor mode.

#include "FeatureScript.h"

using namespace Strata;
using namespace FeatureTest;

// The runner simulates input through the engine's Input API around PressFrame (a field, so the scene decides when):
//   PressFrame - 1   the mouse moves to (100, 60)
//   PressFrame       Space and the left mouse button go down, the mouse moves to (120, 50), the wheel scrolls (0, 2);
//                    gamepad 0 connects with A down, the left stick at (0.6, 0.8) and the right trigger at 0.5
//   PressFrame + 1   nothing happens (all stay down)
//   PressFrame + 2   Space, the left mouse button and A go up
class InputFeatures : public FeatureScript
{
public:
	int32_t PressFrame = 0;

	void OnCreate() override
	{
		Journal(*this, "InputFeatures", "OnCreate");
		Expect(PressFrame > 1, "the scene sets the press frame");
	}

	void OnUpdate(float) override
	{
		const int32_t frame = GetFrame();
		const bool spaceDown = Input::IsKeyDown(Key::Space);
		const bool spacePressed = Input::IsKeyPressed(Key::Space);
		const bool spaceReleased = Input::IsKeyReleased(Key::Space);
		const bool leftDown = Input::IsMouseButtonDown(Mouse::ButtonLeft);
		const bool leftPressed = Input::IsMouseButtonPressed(Mouse::ButtonLeft);
		const bool leftReleased = Input::IsMouseButtonReleased(Mouse::ButtonLeft);

		const bool aDown = Input::IsGamepadButtonDown(GamepadButton::A);
		const bool aPressed = Input::IsGamepadButtonPressed(GamepadButton::A);
		const bool aReleased = Input::IsGamepadButtonReleased(GamepadButton::A, 0);

		Expect(!Input::IsKeyDown(Key::W) && !Input::IsKeyPressed(Key::Escape), "keys that are not pressed");
		Expect(!Input::IsMouseButtonDown(Mouse::ButtonRight), "buttons that are not pressed");
		Expect(!Input::IsGamepadConnected(3) && !Input::IsGamepadButtonDown(GamepadButton::B), "gamepads and buttons that are not there");

		if (frame < PressFrame)
		{
			Expect(!spaceDown && !spacePressed && !spaceReleased && !leftDown && !leftPressed && !leftReleased, "no input before the press frame");
			Expect(!Input::IsGamepadConnected() && !aDown && !aPressed && !aReleased, "no gamepad before the press frame");
		}
		else if (frame == PressFrame)
		{
			Expect(spaceDown && spacePressed && !spaceReleased, "IsKeyDown and IsKeyPressed in the press frame");
			Expect(leftDown && leftPressed && !leftReleased, "IsMouseButtonDown and IsMouseButtonPressed in the press frame");
			Expect(Near(Input::GetMousePosition(), glm::vec2(120.0f, 50.0f)), "GetMousePosition");
			Expect(Near(Input::GetMouseDelta(), glm::vec2(20.0f, -10.0f)), "GetMouseDelta");
			Expect(Near(Input::GetScrollDelta(), glm::vec2(0.0f, 2.0f)), "GetScrollDelta");
			Expect(Input::IsGamepadConnected(0) && aDown && aPressed && !aReleased, "IsGamepadConnected, IsGamepadButtonDown and IsGamepadButtonPressed");
			Expect(Near(Input::GetGamepadAxis(GamepadAxis::LeftX, 0, 0.0f), 0.6f), "GetGamepadAxis without a dead zone");
			Expect(Near(Input::GetGamepadAxis(GamepadAxis::RightTrigger), (0.5f - Input::c_DefaultDeadZone) / (1.0f - Input::c_DefaultDeadZone)),
				"GetGamepadAxis rescales beyond the dead zone");
			// Full tilt (length 1) keeps its direction and its length through the round dead zone.
			Expect(Near(Input::GetGamepadStick(GamepadStick::Left), glm::vec2(0.6f, 0.8f)), "GetGamepadStick");
			Expect(Near(Input::GetGamepadStick(GamepadStick::Right, 0), glm::vec2(0.0f)), "a stick at rest");

			// A first-person camera locks the cursor and reads the mouse movement.
			Input::SetCursorMode(CursorMode::Locked);
			Expect(Input::GetCursorMode() == CursorMode::Locked, "SetCursorMode and GetCursorMode");
		}
		else if (frame == PressFrame + 1)
		{
			Expect(spaceDown && !spacePressed && !spaceReleased, "held keys are down without a transition");
			Expect(leftDown && !leftPressed && !leftReleased, "held buttons are down without a transition");
			Expect(aDown && !aPressed && !aReleased, "held gamepad buttons are down without a transition");
			Expect(Near(Input::GetMousePosition(), glm::vec2(120.0f, 50.0f)), "the mouse position persists");
			Expect(Near(Input::GetMouseDelta(), glm::vec2(0.0f)) && Near(Input::GetScrollDelta(), glm::vec2(0.0f)), "movement is per frame");
		}
		else if (frame == PressFrame + 2)
		{
			Expect(!spaceDown && !spacePressed && spaceReleased, "IsKeyReleased in the release frame");
			Expect(!leftDown && !leftPressed && leftReleased, "IsMouseButtonReleased in the release frame");
			Expect(!aDown && !aPressed && aReleased, "IsGamepadButtonReleased in the release frame");
			Input::SetCursorMode(CursorMode::Normal);
			Expect(Input::GetCursorMode() == CursorMode::Normal, "the cursor is given back");
			Completed = true;
		}
	}
};

ST_SCRIPT_CLASS(InputFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(PressFrame);
}
