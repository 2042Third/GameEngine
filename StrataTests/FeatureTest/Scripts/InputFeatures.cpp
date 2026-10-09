// Input: keyboard, mouse buttons, mouse position and movement, scrolling.

#include "FeatureScript.h"

using namespace Strata;
using namespace FeatureTest;

// The runner simulates input through the engine's Input API around PressFrame (a field, so the scene decides when):
//   PressFrame - 1   the mouse moves to (100, 60)
//   PressFrame       Space and the left mouse button go down, the mouse moves to (120, 50), the wheel scrolls (0, 2)
//   PressFrame + 1   nothing happens (both stay down)
//   PressFrame + 2   Space and the left mouse button go up
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

		Expect(!Input::IsKeyDown(Key::W) && !Input::IsKeyPressed(Key::Escape), "keys that are not pressed");
		Expect(!Input::IsMouseButtonDown(Mouse::ButtonRight), "buttons that are not pressed");

		if (frame < PressFrame)
		{
			Expect(!spaceDown && !spacePressed && !spaceReleased && !leftDown && !leftPressed && !leftReleased, "no input before the press frame");
		}
		else if (frame == PressFrame)
		{
			Expect(spaceDown && spacePressed && !spaceReleased, "IsKeyDown and IsKeyPressed in the press frame");
			Expect(leftDown && leftPressed && !leftReleased, "IsMouseButtonDown and IsMouseButtonPressed in the press frame");
			Expect(Near(Input::GetMousePosition(), glm::vec2(120.0f, 50.0f)), "GetMousePosition");
			Expect(Near(Input::GetMouseDelta(), glm::vec2(20.0f, -10.0f)), "GetMouseDelta");
			Expect(Near(Input::GetScrollDelta(), glm::vec2(0.0f, 2.0f)), "GetScrollDelta");
		}
		else if (frame == PressFrame + 1)
		{
			Expect(spaceDown && !spacePressed && !spaceReleased, "held keys are down without a transition");
			Expect(leftDown && !leftPressed && !leftReleased, "held buttons are down without a transition");
			Expect(Near(Input::GetMousePosition(), glm::vec2(120.0f, 50.0f)), "the mouse position persists");
			Expect(Near(Input::GetMouseDelta(), glm::vec2(0.0f)) && Near(Input::GetScrollDelta(), glm::vec2(0.0f)), "movement is per frame");
		}
		else if (frame == PressFrame + 2)
		{
			Expect(!spaceDown && !spacePressed && spaceReleased, "IsKeyReleased in the release frame");
			Expect(!leftDown && !leftPressed && leftReleased, "IsMouseButtonReleased in the release frame");
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
