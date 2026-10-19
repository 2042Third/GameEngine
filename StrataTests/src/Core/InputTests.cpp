#include <doctest/doctest.h>

#include "Strata/Core/StringUtils.h"
#include "Strata/Input/Input.h"
#include "Strata/Input/InputNames.h"
#include "Strata/Input/InputWindow.h"

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

	TEST_CASE("A viewport with a resolution of its own reports the mouse in the game's pixels")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::ProcessMouseMove({ 100.0f, 50.0f });
		// A 1920 x 1080 image shown in 960 x 540 window pixels: every window pixel is two of the game's.
		Input::SetViewport({ 40.0f, 30.0f }, { 960.0f, 540.0f }, { 1920.0f, 1080.0f });
		CHECK(Input::GetMousePosition() == glm::vec2(120.0f, 40.0f));
		// The game's screen is the resolution: what UI hit tests against, in the pixels the mouse is in.
		CHECK(Input::GetViewportSize() == glm::vec2(1920.0f, 1080.0f));
		Input::ProcessMouseMove({ 110.0f, 45.0f });
		CHECK(Input::GetMouseDelta() == glm::vec2(20.0f, -10.0f));
		// Without a resolution, the region's size.
		Input::SetViewport({ 40.0f, 30.0f }, { 960.0f, 540.0f });
		CHECK(Input::GetViewportSize() == glm::vec2(960.0f, 540.0f));
		CHECK(Input::GetMousePosition() == glm::vec2(70.0f, 15.0f));
		Input::SetViewport({ 0.0f, 0.0f }, { 0.0f, 0.0f });
	}

	TEST_CASE("Input reaches its window through InputWindow: the cursor mode and the size without a viewport")
	{
		// A window as Input sees it; no window system is needed.
		struct TestWindow final : public InputWindow
		{
			uint32_t GetWidth() const override { return 800; }
			uint32_t GetHeight() const override { return 600; }
			void SetCursorMode(CursorMode mode) override { Cursor = mode; }

			CursorMode Cursor = CursorMode::Normal;
		};

		TestWindow window;
		Input::Reset();
		Input::SetWindow(&window);
		CHECK(Input::GetViewportSize() == glm::vec2(800.0f, 600.0f));
		Input::SetViewport({ 10.0f, 20.0f }, { 320.0f, 240.0f });
		CHECK(Input::GetViewportSize() == glm::vec2(320.0f, 240.0f));
		Input::SetViewport({ 0.0f, 0.0f }, { 0.0f, 0.0f });

		Input::SetCursorMode(CursorMode::Locked);
		CHECK(window.Cursor == CursorMode::Locked);
		CHECK(Input::GetCursorMode() == CursorMode::Locked);

		// Resetting the input state keeps the window.
		Input::Reset();
		Input::SetCursorMode(CursorMode::Hidden);
		CHECK(window.Cursor == CursorMode::Hidden);

		Input::SetWindow(nullptr);
		CHECK(Input::GetViewportSize() == glm::vec2(0.0f));
		Input::SetCursorMode(CursorMode::Normal);
		CHECK(window.Cursor == CursorMode::Hidden);
		Input::Reset();
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

	TEST_CASE("Suspended input frames keep their transitions and queued events for the next frame")
	{
		Input::Reset();
		Input::BeginFrame();
		const uint64_t frame = Input::GetFrameIndex();
		Input::ProcessKey(Key::A, true);
		Input::SimulateMouseMove({ 4.0f, 4.0f });

		// While suspended (the game does not update), no frame starts: the frame the game saw ends, queued simulated events
		// wait, and device events collect for the game's next update.
		Input::SetSuspended(true);
		CHECK(Input::IsSuspended());
		Input::SimulateKey(Key::S, true);
		Input::BeginFrame();
		CHECK_FALSE(Input::IsKeyPressed(Key::A));
		CHECK(Input::IsKeyDown(Key::A));
		Input::ProcessKey(Key::D, true);
		Input::ProcessScroll({ 0.0f, 1.0f });
		Input::BeginFrame();
		Input::BeginFrame();
		CHECK(Input::GetFrameIndex() == frame);
		CHECK_FALSE(Input::IsKeyDown(Key::S));
		CHECK(Input::HasQueuedSimulatedInput());

		Input::SetSuspended(false);
		Input::BeginFrame();
		CHECK(Input::GetFrameIndex() == frame + 1);
		CHECK_FALSE(Input::HasQueuedSimulatedInput());
		CHECK(Input::IsKeyPressed(Key::S));
		CHECK(Input::IsKeyPressed(Key::D)); // Pressed while the game was paused: reported in its next update
		CHECK(Input::GetScrollDelta() == glm::vec2(0.0f, 1.0f));
		CHECK_FALSE(Input::IsKeyPressed(Key::A));
		CHECK(Input::GetMousePosition() == glm::vec2(4.0f, 4.0f));
		Input::BeginFrame();
		CHECK_FALSE(Input::IsKeyPressed(Key::D));
		CHECK(Input::GetScrollDelta() == glm::vec2(0.0f));
		Input::Reset();
		CHECK_FALSE(Input::IsSuspended());
	}

	TEST_CASE("Releasing all simulated input also drops presses still queued")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::SimulateKey(Key::Q, true);
		Input::SimulateMouseButton(Mouse::ButtonMiddle, true);
		Input::BeginFrame();
		Input::SimulateKey(Key::E, true);         // Not applied yet
		Input::SimulateMouseMove({ 9.0f, 9.0f }); // Stays queued
		Input::ReleaseAllSimulated();
		Input::BeginFrame();
		CHECK(Input::IsKeyReleased(Key::Q));
		CHECK(Input::IsMouseButtonReleased(Mouse::ButtonMiddle));
		CHECK_FALSE(Input::IsKeyDown(Key::E));
		CHECK_FALSE(Input::IsKeyPressed(Key::E));
		CHECK(Input::GetMousePosition() == glm::vec2(9.0f, 9.0f));
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

	TEST_CASE("Gamepad axes are clamped to their ranges and rest at zero, triggers at released")
	{
		Input::Reset();
		Input::BeginFrame();
		const float nan = std::numeric_limits<float>::quiet_NaN();
		float axes[c_GamepadAxisCount] = { 2.0f, -3.0f, nan, 0.25f, -1.0f, 0.75f };
		bool buttons[c_GamepadButtonCount] = {};
		Input::ProcessGamepad(1, true, axes, buttons);
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::LeftX) == 1.0f);
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::LeftY) == -1.0f);
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::RightX) == 0.0f); // Not a number: at rest
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::RightY) == 0.25f);
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::LeftTrigger) == 0.0f); // Triggers do not go below released
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::RightTrigger) == 0.75f);
		CHECK(GetGamepadAxisMinimum(GamepadAxis::LeftY) == -1.0f);
		CHECK(GetGamepadAxisMinimum(GamepadAxis::RightTrigger) == 0.0f);

		// Out of range gamepads and axes read as nothing.
		CHECK(Input::GetGamepadAxis(c_MaxGamepads, GamepadAxis::LeftX) == 0.0f);
		CHECK(Input::GetGamepadAxis(1, static_cast<GamepadAxis>(c_GamepadAxisCount)) == 0.0f);
		CHECK_FALSE(Input::IsGamepadButtonDown(c_MaxGamepads, GamepadButton::A));
		CHECK_FALSE(Input::IsGamepadButtonDown(1, static_cast<GamepadButton>(c_GamepadButtonCount)));

		// A disabled game sees the sticks and triggers at rest.
		Input::SetEnabled(false);
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::RightY) == 0.0f);
		CHECK(Input::GetGamepadAxis(1, GamepadAxis::RightTrigger) == 0.0f);
		Input::Reset();
	}

	TEST_CASE("Simulated gamepads connect when used, merge with the devices and release like keys")
	{
		Input::Reset();
		Input::BeginFrame();
		CHECK_FALSE(Input::IsGamepadConnected(2));
		Input::SimulateGamepadButton(2, GamepadButton::A, true);
		Input::SimulateGamepadAxis(2, GamepadAxis::LeftX, 0.5f);
		Input::SimulateGamepadAxis(2, GamepadAxis::RightTrigger, -4.0f); // Clamped to released
		Input::SimulateGamepadAxis(2, GamepadAxis::LeftY, std::numeric_limits<float>::infinity()); // Ignored
		CHECK_FALSE(Input::IsGamepadConnected(2)); // Queued until the next input frame

		Input::BeginFrame();
		CHECK(Input::IsGamepadConnected(2));
		CHECK(Input::IsSimulatedGamepadConnected(2));
		CHECK(Input::IsGamepadButtonDown(2, GamepadButton::A));
		CHECK(Input::IsGamepadButtonPressed(2, GamepadButton::A));
		CHECK(Input::IsSimulatedGamepadButtonDown(2, GamepadButton::A));
		CHECK(Input::GetGamepadAxis(2, GamepadAxis::LeftX) == 0.5f);
		CHECK(Input::GetGamepadAxis(2, GamepadAxis::RightTrigger) == 0.0f);
		CHECK(Input::GetGamepadAxis(2, GamepadAxis::LeftY) == 0.0f);
		REQUIRE(Input::GetSimulatedGamepadAxis(2, GamepadAxis::LeftX));
		CHECK(*Input::GetSimulatedGamepadAxis(2, GamepadAxis::LeftX) == 0.5f);
		CHECK_FALSE(Input::GetSimulatedGamepadAxis(2, GamepadAxis::LeftY));

		// A device on the same slot: its buttons merge, a held simulated axis wins over its stick.
		float axes[c_GamepadAxisCount] = { -0.75f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f };
		bool buttons[c_GamepadButtonCount] = {};
		buttons[static_cast<size_t>(GamepadButton::B)] = true;
		Input::BeginFrame();
		Input::ProcessGamepad(2, true, axes, buttons);
		CHECK(Input::IsGamepadButtonDown(2, GamepadButton::A));
		CHECK_FALSE(Input::IsGamepadButtonPressed(2, GamepadButton::A));
		CHECK(Input::IsGamepadButtonPressed(2, GamepadButton::B));
		CHECK(Input::GetGamepadAxis(2, GamepadAxis::LeftX) == 0.5f);
		CHECK(Input::GetGamepadAxis(2, GamepadAxis::LeftY) == 0.5f);

		// Releasing the axis gives it back to the device; releasing the button is a release transition.
		Input::ReleaseSimulatedGamepadAxis(2, GamepadAxis::LeftX);
		Input::SimulateGamepadButton(2, GamepadButton::A, false);
		Input::BeginFrame();
		CHECK(Input::GetGamepadAxis(2, GamepadAxis::LeftX) == -0.75f);
		CHECK_FALSE(Input::IsGamepadButtonDown(2, GamepadButton::A));
		CHECK(Input::IsGamepadButtonReleased(2, GamepadButton::A));

		// Simulated gamepads reach the game while device input is disabled.
		Input::SetEnabled(false);
		Input::SimulateGamepadButton(2, GamepadButton::Start, true);
		Input::SimulateGamepadAxis(2, GamepadAxis::RightY, -1.0f);
		Input::BeginFrame();
		CHECK(Input::IsGamepadButtonPressed(2, GamepadButton::Start));
		CHECK_FALSE(Input::IsGamepadButtonDown(2, GamepadButton::B));
		CHECK(Input::GetGamepadAxis(2, GamepadAxis::RightY) == -1.0f);
		CHECK(Input::GetGamepadAxis(2, GamepadAxis::LeftY) == 0.0f);

		// Releasing everything lets go of the buttons and axes; clearing forgets the gamepad.
		Input::SimulateGamepadButton(2, GamepadButton::X, true); // Still queued: dropped
		Input::ReleaseAllSimulated();
		Input::BeginFrame();
		CHECK(Input::IsGamepadButtonReleased(2, GamepadButton::Start));
		CHECK_FALSE(Input::IsGamepadButtonDown(2, GamepadButton::X));
		CHECK_FALSE(Input::GetSimulatedGamepadAxis(2, GamepadAxis::RightY));
		CHECK(Input::IsSimulatedGamepadConnected(2));
		Input::ClearSimulated();
		CHECK_FALSE(Input::IsSimulatedGamepadConnected(2));
		CHECK(Input::IsGamepadConnected(2)); // The device is still there

		// Out of range gamepads and controls are ignored.
		Input::SimulateGamepadButton(c_MaxGamepads, GamepadButton::A, true);
		Input::SimulateGamepadAxis(0, static_cast<GamepadAxis>(c_GamepadAxisCount), 1.0f);
		Input::BeginFrame();
		CHECK_FALSE(Input::IsSimulatedGamepadConnected(0));
		CHECK_FALSE(Input::HasQueuedSimulatedInput());
		Input::Reset();
	}

	TEST_CASE("The game's cursor mode applies to the window only while the game has the input")
	{
		struct TestWindow final : public InputWindow
		{
			uint32_t GetWidth() const override { return 800; }
			uint32_t GetHeight() const override { return 600; }
			void SetCursorMode(CursorMode mode) override { Cursor = mode; Changes++; }

			CursorMode Cursor = CursorMode::Normal;
			int Changes = 0;
		};

		TestWindow window;
		Input::Reset();
		Input::SetWindow(&window);

		Input::SetGameCursorMode(CursorMode::Locked);
		CHECK(Input::GetGameCursorMode() == CursorMode::Locked);
		CHECK(window.Cursor == CursorMode::Locked);
		CHECK(Input::GetCursorMode() == CursorMode::Locked);

		// The game loses the input (the editor's game view lost the focus): the cursor is free, and comes back locked.
		Input::SetEnabled(false);
		CHECK(window.Cursor == CursorMode::Normal);
		CHECK(Input::GetGameCursorMode() == CursorMode::Locked);
		Input::SetGameCursorMode(CursorMode::Hidden); // Recorded, applied once the game has the input again
		CHECK(window.Cursor == CursorMode::Normal);
		Input::SetEnabled(true);
		CHECK(window.Cursor == CursorMode::Hidden);

		// Enabling input that is enabled changes nothing.
		const int changes = window.Changes;
		Input::SetEnabled(true);
		CHECK(window.Changes == changes);

		// A game with the normal cursor leaves the window's cursor to the editor (e.g. its fly mode).
		Input::SetGameCursorMode(CursorMode::Normal);
		Input::SetCursorMode(CursorMode::Locked);
		Input::SetEnabled(false);
		Input::SetEnabled(true);
		CHECK(window.Cursor == CursorMode::Locked);

		// Resetting forgets the game's mode.
		Input::SetGameCursorMode(CursorMode::Hidden);
		Input::Reset();
		CHECK(Input::GetGameCursorMode() == CursorMode::Normal);
		Input::SetCursorMode(CursorMode::Normal);
		Input::SetWindow(nullptr);
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

	TEST_CASE("Typed text is reported for one input frame as UTF-8, the devices' first, then simulated text")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::ProcessText('h');
		Input::ProcessText(0xE9);    // e with acute: two bytes
		Input::ProcessText(0x20AC);  // The euro sign: three bytes
		Input::ProcessText(0x1F600); // An emoji: four bytes
		Input::ProcessText('\n');    // Control characters, surrogates and code points beyond Unicode are not text
		Input::ProcessText(0x7F);
		Input::ProcessText(0xD800);
		Input::ProcessText(0x110000);
		CHECK(Input::GetTypedText() == "h\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");

		// Simulated text waits for the next input frame, and follows the devices' text there.
		Input::SimulateText("ok \xC3\xA9");
		Input::SimulateText("");
		CHECK(Input::HasQueuedSimulatedInput());
		Input::BeginFrame();
		Input::ProcessText('x');
		CHECK(Input::GetTypedText() == "xok \xC3\xA9");
		Input::BeginFrame();
		CHECK(Input::GetTypedText().empty());

		// While input frames are suspended, the devices' text waits for the game's next update.
		Input::SetSuspended(true);
		Input::BeginFrame();
		Input::ProcessText('a');
		Input::BeginFrame();
		Input::ProcessText('b');
		Input::SetSuspended(false);
		Input::BeginFrame();
		CHECK(Input::GetTypedText() == "ab");
		Input::Reset();
	}

	TEST_CASE("Malformed simulated UTF-8 becomes U+FFFD, and a frame's text stops at c_MaxTypedTextBytes whole characters")
	{
		Input::Reset();
		Input::BeginFrame();
		// A stray continuation byte, a truncated sequence before ASCII, an overlong encoding and an encoded surrogate.
		Input::SimulateText("a\x80" "b\xE2\x82" "c\xC0\xAF\xED\xA0\x80");
		Input::BeginFrame();
		const std::string replacement = "\xEF\xBF\xBD";
		CHECK(Input::GetTypedText() == "a" + replacement + "b" + replacement + "c" + replacement + replacement);

		// 1,023 bytes of ASCII leave room for one byte: the next two-byte character is dropped whole, and so is what follows
		// (the frame keeps what was typed first).
		Input::SimulateText(std::string(Input::c_MaxTypedTextBytes - 1, 'z') + "\xC3\xA9" "q");
		Input::BeginFrame();
		const std::string capped = Input::GetTypedText();
		CHECK(capped.size() == Input::c_MaxTypedTextBytes - 1);
		CHECK(capped.find_first_not_of('z') == std::string::npos);

		// The devices' text has the same limit, and simulated text only fills what it leaves.
		for (size_t index = 0; index < Input::c_MaxTypedTextBytes - 2; index++)
			Input::ProcessText('d');
		Input::ProcessText(0x20AC);
		Input::ProcessText('d');
		CHECK(Input::GetTypedText() == std::string(Input::c_MaxTypedTextBytes - 2, 'd') + "zz");
		Input::SimulateText("s");
		Input::BeginFrame();
		Input::ProcessText(0x20AC);
		Input::ProcessText('e');
		CHECK(Input::GetTypedText() == "\xE2\x82\xAC" "es");
		Input::Reset();
	}

	TEST_CASE("Disabled input drops the devices' text, and simulated text still comes through")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::SetEnabled(false);
		Input::ProcessText('k');
		Input::SimulateText("sim");
		CHECK(Input::GetTypedText().empty());
		Input::BeginFrame();
		Input::ProcessText('k');
		CHECK(Input::GetTypedText() == "sim");
		Input::SetEnabled(true);
		CHECK(Input::GetTypedText() == "ksim");

		// Clearing simulated input drops its queued text.
		Input::SimulateText("gone");
		Input::ClearSimulated();
		Input::BeginFrame();
		CHECK(Input::GetTypedText().empty());
		Input::Reset();
	}

	TEST_CASE("Pointer capture hides mouse buttons and scrolling from the game, never from the pointer state")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::ProcessMouseMove({ 30.0f, 40.0f });
		Input::ProcessMouseButton(Mouse::ButtonLeft, true);
		Input::ProcessScroll({ 0.0f, 2.0f });
		Input::SimulateMouseButton(Mouse::ButtonRight, true);
		CHECK(Input::IsMouseButtonPressed(Mouse::ButtonLeft));

		Input::SetPointerCapture(true);
		CHECK(Input::IsPointerCaptured());
		CHECK_FALSE(Input::IsMouseButtonDown(Mouse::ButtonLeft));
		CHECK_FALSE(Input::IsMouseButtonPressed(Mouse::ButtonLeft));
		CHECK(Input::GetScrollDelta() == glm::vec2(0.0f));
		PointerState pointer = Input::GetPointerState();
		CHECK(pointer.Position == glm::vec2(30.0f, 40.0f));
		CHECK(pointer.ScrollDelta == glm::vec2(0.0f, 2.0f));
		CHECK(pointer.IsDown(Mouse::ButtonLeft));
		CHECK(pointer.IsPressed(Mouse::ButtonLeft));
		CHECK_FALSE(pointer.IsDown(Mouse::ButtonRight)); // Simulated presses apply at the next input frame
		CHECK(Input::GetMousePosition() == glm::vec2(30.0f, 40.0f)); // The position is not hidden

		Input::BeginFrame();
		Input::ProcessMouseButton(Mouse::ButtonLeft, false);
		CHECK_FALSE(Input::IsMouseButtonReleased(Mouse::ButtonLeft));
		CHECK_FALSE(Input::IsMouseButtonDown(Mouse::ButtonRight));
		pointer = Input::GetPointerState();
		CHECK(pointer.IsReleased(Mouse::ButtonLeft));
		CHECK(pointer.IsDown(Mouse::ButtonRight));
		CHECK(pointer.IsPressed(Mouse::ButtonRight));
		CHECK_FALSE(pointer.IsDown(static_cast<MouseCode>(40)));

		// Capture lasts until it is cleared: by its owner, or by Reset.
		Input::SetPointerCapture(false);
		CHECK(Input::IsMouseButtonReleased(Mouse::ButtonLeft));
		CHECK(Input::IsMouseButtonDown(Mouse::ButtonRight));
		Input::SetPointerCapture(true);
		Input::Reset();
		CHECK_FALSE(Input::IsPointerCaptured());
	}

	TEST_CASE("Keyboard capture hides keys and typed text from the game, never from the key state")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::ProcessKey(Key::A, true);
		Input::ProcessText('a');
		Input::SimulateText("b");
		Input::SimulateKey(Key::Enter, true);
		Input::SetKeyboardCapture(true);
		CHECK(Input::IsKeyboardCaptured());
		CHECK_FALSE(Input::IsKeyDown(Key::A));
		CHECK_FALSE(Input::IsKeyPressed(Key::A));
		CHECK(Input::GetTypedText().empty());
		KeyState a = Input::GetKeyState(Key::A);
		CHECK(a.Down);
		CHECK(a.Pressed);
		CHECK_FALSE(a.Released);
		CHECK(Input::GetUncapturedTypedText() == "a");

		// Simulated keys and text reach the key state alike, in the next input frame.
		Input::BeginFrame();
		CHECK(Input::GetKeyState(Key::Enter).Pressed);
		CHECK_FALSE(Input::IsKeyPressed(Key::Enter));
		CHECK(Input::GetUncapturedTypedText() == "b");
		Input::ProcessKey(Key::A, false);
		a = Input::GetKeyState(Key::A);
		CHECK(a.Released);
		CHECK_FALSE(Input::IsKeyReleased(Key::A));
		// Out-of-range codes are no key.
		CHECK_FALSE(Input::GetKeyState(static_cast<KeyCode>(60000)).Down);

		// Capture lasts until it is cleared: by its owner, or by Reset.
		Input::SetKeyboardCapture(false);
		CHECK(Input::IsKeyDown(Key::Enter));
		CHECK(Input::GetTypedText() == "b");
		Input::SetKeyboardCapture(true);
		Input::Reset();
		CHECK_FALSE(Input::IsKeyboardCaptured());
	}

	TEST_CASE("Disabled input hides the devices' pointer buttons from the pointer state, not simulated ones")
	{
		Input::Reset();
		Input::BeginFrame();
		Input::SetEnabled(false);
		Input::ProcessMouseButton(Mouse::ButtonLeft, true);
		Input::SimulateMouseButton(Mouse::ButtonMiddle, true);
		Input::BeginFrame();
		Input::ProcessScroll({ 1.0f, 0.0f });
		const PointerState pointer = Input::GetPointerState();
		CHECK_FALSE(pointer.IsDown(Mouse::ButtonLeft));
		CHECK(pointer.ScrollDelta == glm::vec2(0.0f));
		CHECK(pointer.IsPressed(Mouse::ButtonMiddle));
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

	TEST_CASE("Gamepad buttons and axes are named like their constants")
	{
		const std::span<const char* const> buttons = InputNames::GetGamepadButtonNames();
		REQUIRE(buttons.size() == c_GamepadButtonCount);
		for (uint32_t index = 0; index < c_GamepadButtonCount; index++)
		{
			const GamepadButton button = static_cast<GamepadButton>(index);
			CHECK(InputNames::FindGamepadButton(buttons[index]) == button);
			CHECK(InputNames::FindGamepadButton(StringUtils::ToLower(buttons[index])) == button);
			CHECK(std::string(InputNames::GetGamepadButtonName(button)) == buttons[index]);
		}
		CHECK(InputNames::FindGamepadButton("A") == GamepadButton::A);
		CHECK(InputNames::FindGamepadButton("leftbumper") == GamepadButton::LeftBumper);
		CHECK(InputNames::FindGamepadButton("DPadLeft") == GamepadButton::DPadLeft);
		CHECK_FALSE(InputNames::FindGamepadButton("Triangle"));
		CHECK_FALSE(InputNames::FindGamepadButton(""));
		CHECK(InputNames::GetGamepadButtonName(static_cast<GamepadButton>(c_GamepadButtonCount)) == nullptr);

		const std::span<const char* const> axes = InputNames::GetGamepadAxisNames();
		REQUIRE(axes.size() == c_GamepadAxisCount);
		for (uint32_t index = 0; index < c_GamepadAxisCount; index++)
		{
			const GamepadAxis axis = static_cast<GamepadAxis>(index);
			CHECK(InputNames::FindGamepadAxis(axes[index]) == axis);
			CHECK(std::string(InputNames::GetGamepadAxisName(axis)) == axes[index]);
		}
		CHECK(InputNames::FindGamepadAxis("rightTrigger") == GamepadAxis::RightTrigger);
		CHECK_FALSE(InputNames::FindGamepadAxis("LeftZ"));
		CHECK(InputNames::GetGamepadAxisName(static_cast<GamepadAxis>(c_GamepadAxisCount)) == nullptr);
	}
}
