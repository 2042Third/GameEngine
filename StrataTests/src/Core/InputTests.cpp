#include <doctest/doctest.h>

#include "Strata/Input/Input.h"

using namespace Strata;

TEST_SUITE("Core.Input")
{
	TEST_CASE("Key transitions are reported for one frame")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::ProcessKey(Key::W, true);
		CHECK(Input::IsKeyDown(Key::W));
		CHECK(Input::IsKeyPressed(Key::W));
		CHECK_FALSE(Input::IsKeyReleased(Key::W));

		Input::BeginFrame();
		CHECK(Input::IsKeyDown(Key::W));
		CHECK_FALSE(Input::IsKeyPressed(Key::W));

		Input::ProcessKey(Key::W, false);
		CHECK_FALSE(Input::IsKeyDown(Key::W));
		CHECK(Input::IsKeyReleased(Key::W));

		// Press and release within a single frame reports both transitions.
		Input::BeginFrame();
		Input::ProcessKey(Key::Space, true);
		Input::ProcessKey(Key::Space, false);
		CHECK(Input::IsKeyPressed(Key::Space));
		CHECK(Input::IsKeyReleased(Key::Space));
		CHECK_FALSE(Input::IsKeyDown(Key::Space));

		CHECK_FALSE(Input::IsKeyDown(static_cast<KeyCode>(c_MaxKeyCode + 5)));
	}

	TEST_CASE("Mouse position, deltas and viewport offset")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::ProcessMouseMove({ 100.0f, 50.0f });
		CHECK(Input::GetMouseDelta() == glm::vec2(0.0f)); // First sample establishes the position
		Input::ProcessMouseMove({ 110.0f, 45.0f });
		CHECK(Input::GetMouseDelta() == glm::vec2(10.0f, -5.0f));

		Input::SetViewport({ 100.0f, 40.0f }, { 640.0f, 480.0f });
		CHECK(Input::GetMousePosition() == glm::vec2(10.0f, 5.0f));
		CHECK(Input::GetViewportSize() == glm::vec2(640.0f, 480.0f));

		Input::ProcessScroll({ 0.0f, 1.0f });
		Input::ProcessScroll({ 0.0f, 2.0f });
		CHECK(Input::GetScrollDelta() == glm::vec2(0.0f, 3.0f));

		Input::BeginFrame();
		CHECK(Input::GetMouseDelta() == glm::vec2(0.0f));
		CHECK(Input::GetScrollDelta() == glm::vec2(0.0f));

		Input::ProcessMouseButton(Mouse::ButtonLeft, true);
		CHECK(Input::IsMouseButtonDown(Mouse::ButtonLeft));
		CHECK(Input::IsMouseButtonPressed(Mouse::ButtonLeft));
		Input::SetViewport({ 0.0f, 0.0f }, { 0.0f, 0.0f });
	}

	TEST_CASE("Disabled input reports nothing but keeps tracking state")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::SetEnabled(false);
		Input::ProcessKey(Key::A, true);
		CHECK_FALSE(Input::IsKeyDown(Key::A));
		CHECK_FALSE(Input::IsKeyPressed(Key::A));

		Input::SetEnabled(true);
		CHECK(Input::IsKeyDown(Key::A));
		Input::Reset();
		CHECK(Input::IsEnabled());
	}

	TEST_CASE("Gamepad state")
	{
		Input::Reset();
		Input::BeginFrame();
		float axes[c_GamepadAxisCount] = { 0.5f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f };
		bool buttons[c_GamepadButtonCount] = {};
		buttons[static_cast<size_t>(GamepadButton::A)] = true;
		Input::ProcessGamepad(0, true, axes, buttons);

		CHECK(Input::IsGamepadConnected(0));
		CHECK_FALSE(Input::IsGamepadConnected(1));
		CHECK(Input::GetGamepadAxis(0, GamepadAxis::LeftX) == doctest::Approx(0.5f));
		CHECK(Input::IsGamepadButtonDown(0, GamepadButton::A));
		CHECK(Input::IsGamepadButtonPressed(0, GamepadButton::A));

		Input::BeginFrame();
		Input::ProcessGamepad(0, true, axes, buttons);
		CHECK_FALSE(Input::IsGamepadButtonPressed(0, GamepadButton::A));

		Input::ProcessGamepad(0, false, nullptr, nullptr);
		CHECK_FALSE(Input::IsGamepadButtonDown(0, GamepadButton::A));
		Input::Reset();
	}
}
