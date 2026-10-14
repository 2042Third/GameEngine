#include <doctest/doctest.h>

#include "Strata/Input/Input.h"
#include "Strata/Input/InputNames.h"
#include "Strata/Input/UIActions.h"

#include <limits>
#include <string>
#include <string_view>

using namespace Strata;

namespace
{

	// Starts an input frame and updates the actions with it.
	void Step(UIActionState& actions, float deltaSeconds)
	{
		Input::BeginFrame();
		actions.Update(deltaSeconds);
	}

}

TEST_SUITE("Core.UIActions")
{
	TEST_CASE("The UI action map lists every action in order, with controls Input knows")
	{
		for (uint32_t action = 0; action < c_UIActionCount; action++)
		{
			const UIActionBinding& entry = c_UIActionMap[action];
			CHECK(entry.Action == GetUIActionName(static_cast<UIAction>(action)));
			uint32_t sticks = 0;
			for (const std::string_view binding : entry.Bindings)
			{
				if (binding.empty())
					continue;
				INFO("Binding ", std::string(binding)); // A string: doctest prints no std::string_view without <ostream>
				if (binding.starts_with("Key/"))
				{
					CHECK(InputNames::FindKey(binding.substr(4)).has_value());
					continue;
				}
				REQUIRE(binding.starts_with("Gamepad/"));
				const std::string_view name = binding.substr(8);
				if (name.ends_with('+') || name.ends_with('-'))
				{
					sticks++;
					CHECK(InputNames::FindGamepadAxis(name.substr(0, name.size() - 1)).has_value());
				}
				else
				{
					CHECK(InputNames::FindGamepadButton(name).has_value());
				}
			}
			CHECK(sticks <= 1);
		}
	}

	TEST_CASE("A held key presses its action once, repeats after the delay and then at the interval")
	{
		Input::Reset();
		UIActionState actions;
		Input::SimulateKey(Key::Down, true);
		Step(actions, 0.016f);
		CHECK(actions.IsPressed(UIAction::Down));
		CHECK(actions.IsRepeated(UIAction::Down));
		CHECK(actions.IsDown(UIAction::Down));
		CHECK_FALSE(actions.IsDown(UIAction::Up));

		Step(actions, 0.3f); // Held 0.3 s
		CHECK_FALSE(actions.IsPressed(UIAction::Down));
		CHECK_FALSE(actions.IsRepeated(UIAction::Down));
		CHECK(actions.IsDown(UIAction::Down));
		Step(actions, 0.15f); // 0.45 s: past the delay
		CHECK(actions.IsRepeated(UIAction::Down));
		Step(actions, 0.03f); // 0.48 s
		CHECK_FALSE(actions.IsRepeated(UIAction::Down));
		Step(actions, 0.04f); // 0.52 s: the next repeat was due at 0.5 s
		CHECK(actions.IsRepeated(UIAction::Down));
		// A long frame repeats once, and owes nothing afterwards.
		Step(actions, 0.5f); // 1.02 s
		CHECK(actions.IsRepeated(UIAction::Down));
		Step(actions, 0.05f); // 1.07 s: the next is due at 1.1 s
		CHECK_FALSE(actions.IsRepeated(UIAction::Down));

		Input::SimulateKey(Key::Down, false);
		Step(actions, 0.016f);
		CHECK_FALSE(actions.IsDown(UIAction::Down));
		CHECK_FALSE(actions.IsRepeated(UIAction::Down));

		// Time scale 0 is the caller's business: the actions take unscaled deltas, and a zero or invalid delta adds nothing.
		Input::SimulateKey(Key::Enter, true);
		Step(actions, 0.0f);
		CHECK(actions.IsPressed(UIAction::Submit));
		Step(actions, -1.0f);
		Step(actions, std::numeric_limits<float>::quiet_NaN());
		CHECK_FALSE(actions.IsRepeated(UIAction::Submit));
		CHECK(actions.IsDown(UIAction::Submit));

		actions.Reset();
		CHECK_FALSE(actions.IsDown(UIAction::Submit));
		Input::Reset();
	}

	TEST_CASE("Each action reads its keys and the buttons of any gamepad")
	{
		Input::Reset();
		UIActionState actions;
		struct Expectation
		{
			UIAction Action;
			KeyCode Key;
			GamepadButton Button;
		};
		const Expectation expectations[] = {
			{ UIAction::Submit, Key::Space, GamepadButton::A },
			{ UIAction::Cancel, Key::Escape, GamepadButton::B },
			{ UIAction::Up, Key::Up, GamepadButton::DPadUp },
			{ UIAction::Down, Key::Down, GamepadButton::DPadDown },
			{ UIAction::Left, Key::Left, GamepadButton::DPadLeft },
			{ UIAction::Right, Key::Right, GamepadButton::DPadRight },
		};
		for (const Expectation& expectation : expectations)
		{
			CAPTURE(GetUIActionName(expectation.Action));
			Input::SimulateKey(expectation.Key, true);
			Step(actions, 0.016f);
			CHECK(actions.IsPressed(expectation.Action));
			Input::SimulateKey(expectation.Key, false);
			Step(actions, 0.016f);
			CHECK_FALSE(actions.IsDown(expectation.Action));

			Input::SimulateGamepadButton(3, expectation.Button, true);
			Step(actions, 0.016f);
			CHECK(actions.IsPressed(expectation.Action));
			Input::SimulateGamepadButton(3, expectation.Button, false);
			Step(actions, 0.016f);
			CHECK_FALSE(actions.IsDown(expectation.Action));
		}
		Input::SimulateKey(Key::KPEnter, true);
		Step(actions, 0.016f);
		CHECK(actions.IsPressed(UIAction::Submit));
		Input::Reset();
	}

	TEST_CASE("A stick presses its direction beyond 0.5 and lets go below 0.35")
	{
		Input::Reset();
		UIActionState actions;
		Input::SimulateGamepadAxis(1, GamepadAxis::LeftY, 0.45f);
		Step(actions, 0.016f);
		CHECK_FALSE(actions.IsDown(UIAction::Up));
		Input::SimulateGamepadAxis(1, GamepadAxis::LeftY, 0.6f); // +Y is up
		Step(actions, 0.016f);
		CHECK(actions.IsPressed(UIAction::Up));
		CHECK_FALSE(actions.IsDown(UIAction::Down));
		// Resting between the thresholds holds the direction: no flicker, no second press.
		Input::SimulateGamepadAxis(1, GamepadAxis::LeftY, 0.4f);
		Step(actions, 0.016f);
		CHECK(actions.IsDown(UIAction::Up));
		CHECK_FALSE(actions.IsPressed(UIAction::Up));
		Input::SimulateGamepadAxis(1, GamepadAxis::LeftY, 0.3f);
		Step(actions, 0.016f);
		CHECK_FALSE(actions.IsDown(UIAction::Up));

		Input::SimulateGamepadAxis(1, GamepadAxis::LeftX, -0.9f);
		Step(actions, 0.016f);
		CHECK(actions.IsPressed(UIAction::Left));
		CHECK_FALSE(actions.IsDown(UIAction::Right));
		Input::ReleaseSimulatedGamepadAxis(1, GamepadAxis::LeftX);
		Step(actions, 0.016f);
		CHECK_FALSE(actions.IsDown(UIAction::Left));
		Input::Reset();
	}
}
