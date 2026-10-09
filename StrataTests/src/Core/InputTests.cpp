#include <doctest/doctest.h>

#include "Strata/Core/StringUtils.h"
#include "Strata/Input/Input.h"
#include "Strata/Input/InputNames.h"

#include <limits>
#include <optional>
#include <span>
#include <string>

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

	TEST_CASE("Simulated input applies at the next frame and is merged with the devices")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::SimulateKey(Key::Left, true);
		CHECK_FALSE(Input::IsKeyDown(Key::Left)); // Queued until the next input frame
		CHECK_FALSE(Input::IsSimulatedKeyDown(Key::Left));

		Input::BeginFrame();
		CHECK(Input::IsKeyDown(Key::Left));
		CHECK(Input::IsKeyPressed(Key::Left));
		CHECK(Input::IsSimulatedKeyDown(Key::Left));

		// The key stays down while either source holds it.
		Input::BeginFrame();
		CHECK_FALSE(Input::IsKeyPressed(Key::Left));
		Input::ProcessKey(Key::Left, true);
		Input::ProcessKey(Key::Left, false);
		Input::SimulateKey(Key::Left, false);
		CHECK(Input::IsKeyDown(Key::Left));

		Input::BeginFrame();
		CHECK_FALSE(Input::IsKeyDown(Key::Left));
		CHECK(Input::IsKeyReleased(Key::Left));
		CHECK_FALSE(Input::IsSimulatedKeyDown(Key::Left));

		// Events of one frame apply in order: a press and release queued together report both transitions.
		Input::SimulateMouseButton(Mouse::ButtonRight, true);
		Input::SimulateMouseButton(Mouse::ButtonRight, false);
		Input::BeginFrame();
		CHECK(Input::IsMouseButtonPressed(Mouse::ButtonRight));
		CHECK(Input::IsMouseButtonReleased(Mouse::ButtonRight));
		CHECK_FALSE(Input::IsMouseButtonDown(Mouse::ButtonRight));

		// Codes outside the tables are ignored.
		Input::SimulateKey(static_cast<KeyCode>(c_MaxKeyCode + 5), true);
		Input::SimulateMouseButton(static_cast<MouseCode>(c_MaxMouseButtons), true);
		Input::BeginFrame();
		CHECK_FALSE(Input::IsKeyDown(static_cast<KeyCode>(c_MaxKeyCode + 5)));
		CHECK_FALSE(Input::IsSimulatedMouseButtonDown(static_cast<MouseCode>(c_MaxMouseButtons)));
		Input::Reset();
	}

	TEST_CASE("A source pressing or releasing a button the other source holds reports no transition")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::ProcessKey(Key::W, true); // A person holds W
		Input::BeginFrame();

		// A tool presses W too: the game sees W held, not pressed again (a key repeat would restart).
		Input::SimulateKey(Key::W, true);
		Input::BeginFrame();
		CHECK(Input::IsKeyDown(Key::W));
		CHECK_FALSE(Input::IsKeyPressed(Key::W));
		CHECK_FALSE(Input::IsKeyReleased(Key::W));

		// The tool lets go while the person still holds it: no release.
		Input::SimulateKey(Key::W, false);
		Input::BeginFrame();
		CHECK(Input::IsKeyDown(Key::W));
		CHECK_FALSE(Input::IsKeyReleased(Key::W));

		// The person lets go now: released.
		Input::ProcessKey(Key::W, false);
		CHECK_FALSE(Input::IsKeyDown(Key::W));
		CHECK(Input::IsKeyReleased(Key::W));

		// The other way round, with a mouse button the tool holds.
		Input::SimulateMouseButton(Mouse::ButtonLeft, true);
		Input::BeginFrame();
		CHECK(Input::IsMouseButtonPressed(Mouse::ButtonLeft));
		Input::BeginFrame();
		Input::ProcessMouseButton(Mouse::ButtonLeft, true);
		CHECK(Input::IsMouseButtonDown(Mouse::ButtonLeft));
		CHECK_FALSE(Input::IsMouseButtonPressed(Mouse::ButtonLeft));
		Input::BeginFrame();
		Input::ProcessMouseButton(Mouse::ButtonLeft, false);
		CHECK(Input::IsMouseButtonDown(Mouse::ButtonLeft));
		CHECK_FALSE(Input::IsMouseButtonReleased(Mouse::ButtonLeft));

		// A device the game cannot see (input disabled) holds nothing for it: the tool's transitions count.
		Input::SetEnabled(false);
		Input::BeginFrame();
		Input::ProcessKey(Key::E, true);
		Input::SimulateKey(Key::E, true);
		Input::BeginFrame();
		CHECK(Input::IsKeyPressed(Key::E));
		Input::Reset();
	}

	TEST_CASE("Simulated input reaches the game while device input is disabled")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::SetEnabled(false);
		Input::ProcessKey(Key::A, true);
		Input::SimulateKey(Key::D, true);
		Input::SimulateMouseButton(Mouse::ButtonLeft, true);
		Input::SimulateScroll({ 0.0f, 1.0f });
		Input::BeginFrame();
		Input::ProcessScroll({ 0.0f, 5.0f });
		CHECK_FALSE(Input::IsKeyDown(Key::A));
		CHECK(Input::IsKeyDown(Key::D));
		CHECK(Input::IsKeyPressed(Key::D));
		CHECK(Input::IsMouseButtonPressed(Mouse::ButtonLeft));
		CHECK(Input::GetScrollDelta() == glm::vec2(0.0f, 1.0f));

		Input::SetEnabled(true);
		CHECK(Input::IsKeyDown(Key::A));
		CHECK(Input::GetScrollDelta() == glm::vec2(0.0f, 6.0f));
		Input::Reset();
	}

	TEST_CASE("The simulated pointer takes over the mouse position")
	{
		Input::Reset();
		Input::SetViewport({ 100.0f, 40.0f }, { 640.0f, 480.0f });
		Input::BeginFrame();
		Input::ProcessMouseMove({ 150.0f, 60.0f });
		CHECK(Input::GetMousePosition() == glm::vec2(50.0f, 20.0f));
		CHECK_FALSE(Input::GetSimulatedMousePosition());

		// Simulated positions are relative to the viewport already; the first one only places the pointer.
		Input::SimulateMouseMove({ 10.0f, 20.0f });
		Input::BeginFrame();
		CHECK(Input::GetMousePosition() == glm::vec2(10.0f, 20.0f));
		CHECK(Input::GetMouseDelta() == glm::vec2(0.0f));
		REQUIRE(Input::GetSimulatedMousePosition());
		CHECK(*Input::GetSimulatedMousePosition() == glm::vec2(10.0f, 20.0f));

		Input::SimulateMouseMove({ 15.0f, 18.0f });
		Input::SimulateMouseMove({ 20.0f, 30.0f });
		Input::SimulateMouseMove({ std::numeric_limits<float>::quiet_NaN(), 0.0f }); // Ignored
		Input::SimulateScroll({ std::numeric_limits<float>::infinity(), 0.0f });     // Ignored
		Input::BeginFrame();
		CHECK(Input::GetMousePosition() == glm::vec2(20.0f, 30.0f));
		CHECK(Input::GetMouseDelta() == glm::vec2(10.0f, 10.0f));
		CHECK(Input::GetScrollDelta() == glm::vec2(0.0f));

		Input::BeginFrame();
		CHECK(Input::GetMouseDelta() == glm::vec2(0.0f));
		CHECK(Input::GetMousePosition() == glm::vec2(20.0f, 30.0f));
		Input::Reset();
	}

	TEST_CASE("Clearing simulated input forgets held keys and queued events without releases")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::SimulateKey(Key::W, true);
		Input::SimulateMouseMove({ 5.0f, 5.0f });
		Input::BeginFrame();
		CHECK(Input::IsKeyDown(Key::W));
		Input::SimulateKey(Key::S, true);
		Input::ProcessKey(Key::E, true);

		Input::ClearSimulated();
		CHECK_FALSE(Input::IsKeyDown(Key::W));
		CHECK_FALSE(Input::IsKeyPressed(Key::W));
		CHECK_FALSE(Input::GetSimulatedMousePosition());
		CHECK(Input::IsKeyDown(Key::E)); // Device input is untouched
		Input::BeginFrame();
		CHECK_FALSE(Input::IsKeyReleased(Key::W));
		CHECK_FALSE(Input::IsKeyDown(Key::S));
		Input::Reset();
	}
}

TEST_SUITE("Core.InputNames")
{
	TEST_CASE("Every key name maps to its code and back, ignoring case")
	{
		const std::span<const char* const> names = InputNames::GetKeyNames();
		CHECK(names.size() == 120);
		int32_t previous = -1;
		for (const char* name : names)
		{
			INFO(name);
			const std::optional<KeyCode> key = InputNames::FindKey(name);
			REQUIRE(key);
			CHECK(static_cast<int32_t>(*key) > previous); // Code order
			previous = *key;
			CHECK(std::string(InputNames::GetKeyName(*key)) == name);
			CHECK(InputNames::FindKey(StringUtils::ToLower(name)) == key);
		}
		CHECK(InputNames::FindKey("Left") == Key::Left);
		CHECK(InputNames::FindKey("SPACE") == Key::Space);
		CHECK(InputNames::FindKey("D1") == Key::D1);
		CHECK(InputNames::FindKey("KPEnter") == Key::KPEnter);
		CHECK(InputNames::FindKey("Menu") == Key::Menu);
		CHECK_FALSE(InputNames::FindKey(""));
		CHECK_FALSE(InputNames::FindKey("Windows"));
		CHECK_FALSE(InputNames::FindKey("Left "));
		CHECK(InputNames::GetKeyName(0) == nullptr);
		CHECK(InputNames::GetKeyName(c_MaxKeyCode) == nullptr);
	}

	TEST_CASE("Mouse buttons have names and numbered aliases")
	{
		const std::span<const char* const> names = InputNames::GetMouseButtonNames();
		REQUIRE(names.size() == c_MaxMouseButtons);
		for (MouseCode button = 0; button < c_MaxMouseButtons; button++)
		{
			CHECK(InputNames::FindMouseButton(names[button]) == button);
			CHECK(std::string(InputNames::GetMouseButtonName(button)) == names[button]);
		}
		CHECK(InputNames::FindMouseButton("left") == Mouse::ButtonLeft);
		CHECK(InputNames::FindMouseButton("Right") == Mouse::ButtonRight);
		CHECK(InputNames::FindMouseButton("Middle") == Mouse::ButtonMiddle);
		CHECK(InputNames::FindMouseButton("Button0") == Mouse::ButtonLeft);
		CHECK(InputNames::FindMouseButton("button2") == Mouse::ButtonMiddle);
		CHECK(InputNames::FindMouseButton("Button7") == Mouse::Button7);
		CHECK_FALSE(InputNames::FindMouseButton("Button8"));
		CHECK_FALSE(InputNames::FindMouseButton(""));
		CHECK(InputNames::GetMouseButtonName(c_MaxMouseButtons) == nullptr);
	}
}
