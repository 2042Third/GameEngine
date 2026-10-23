#include <doctest/doctest.h>

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Input/Input.h"
#include "Strata/Input/InputActions.h"
#include "Strata/Input/InputActionSet.h"
#include "Strata/Input/UIActions.h"

#include <nlohmann/json.hpp>

#include <limits>
#include <string>
#include <vector>

using namespace Strata;

namespace
{

	// Starts an input frame, lets the input actions take it (as Application does) and updates the UI actions with it.
	void Step(UIActionState& actions, float deltaSeconds)
	{
		Input::BeginFrame();
		InputActions::Update();
		actions.Update(deltaSeconds);
	}

}

TEST_SUITE("Core.UIActions")
{
	TEST_CASE("The UI map's defaults list every action in order, as Buttons with the controls the game UI navigated with")
	{
		const std::vector<InputAction>& defaults = InputActionSet::GetDefaultUIActions();
		REQUIRE(defaults.size() == c_UIActionCount);
		for (uint32_t action = 0; action < c_UIActionCount; action++)
		{
			const UIAction uiAction = static_cast<UIAction>(action);
			CHECK(defaults[action].Name == GetUIActionName(uiAction));
			CHECK(std::string(GetUIActionQualifiedName(uiAction)) == std::string("UI/") + GetUIActionName(uiAction));
			CHECK(defaults[action].Kind == InputActionKind::Button);
			for (const InputBinding& binding : defaults[action].Bindings)
				CHECK(binding.FitsKind(InputActionKind::Button));
		}
		const auto texts = [&defaults](UIAction action)
		{
			std::vector<std::string> bindings;
			for (const InputBinding& binding : defaults[static_cast<size_t>(action)].Bindings)
				bindings.push_back(binding.ToString());
			return bindings;
		};
		CHECK(texts(UIAction::Submit) == std::vector<std::string> { "Key/Enter", "Key/KPEnter", "Key/Space", "Gamepad/A" });
		CHECK(texts(UIAction::Cancel) == std::vector<std::string> { "Key/Escape", "Gamepad/B" });
		CHECK(texts(UIAction::Up) == std::vector<std::string> { "Key/Up", "Gamepad/DPadUp", "Gamepad/LeftY+" });
		CHECK(texts(UIAction::Down) == std::vector<std::string> { "Key/Down", "Gamepad/DPadDown", "Gamepad/LeftY-" });
		CHECK(texts(UIAction::Left) == std::vector<std::string> { "Key/Left", "Gamepad/DPadLeft", "Gamepad/LeftX-" });
		CHECK(texts(UIAction::Right) == std::vector<std::string> { "Key/Right", "Gamepad/DPadRight", "Gamepad/LeftX+" });
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

	TEST_CASE("The UI actions follow the set when the project changes it, and idle updates read nothing new")
	{
		SettingsRegistry registry;
		REQUIRE(registry.Register(InputActionSet::MakeSection()));
		ProjectSettingsValues settings;
		EngineServices::ScopedProvider<IProjectSettings> provider(settings);
		Input::Reset();
		UIActionState actions;
		Input::SimulateKey(Key::Enter, true);
		Step(actions, 0.016f);
		CHECK(actions.IsPressed(UIAction::Submit));

		// The UI map moves behind a game's map: every UI action has another place in the set now.
		const nlohmann::json moved = { { "Maps", nlohmann::json::array({
			{ { "Name", "Gameplay" }, { "Actions", nlohmann::json::array({ { { "Name", "Jump" }, { "Bindings", { "Key/Escape" } } },
				{ { "Name", "Fire" }, { "Bindings", { "Key/Enter" } } } }) } },
			{ { "Name", "UI" } } }) } };
		std::string error;
		REQUIRE_MESSAGE(settings.Set(InputActionSet::c_SectionId, moved, registry, &error), error);
		Step(actions, 0.016f);
		CHECK(actions.IsDown(UIAction::Submit));
		CHECK_FALSE(actions.IsPressed(UIAction::Submit));
		Input::SimulateKey(Key::Escape, true);
		Step(actions, 0.016f);
		CHECK(actions.IsPressed(UIAction::Cancel));
		CHECK(actions.IsDown(UIAction::Submit));
		Input::SimulateKey(Key::Enter, false);
		Input::SimulateKey(Key::Escape, false);
		Step(actions, 0.016f);
		CHECK_FALSE(actions.IsDown(UIAction::Submit));
		CHECK_FALSE(actions.IsDown(UIAction::Cancel));

		// Updates without an input frame or a change read what the last one did; a map disabled meanwhile lets go of the held
		// action in the next input frame.
		Input::SimulateKey(Key::Down, true);
		Step(actions, 0.016f);
		CHECK(actions.IsPressed(UIAction::Down));
		actions.Update(0.016f);
		CHECK(actions.IsDown(UIAction::Down));
		CHECK(InputActions::SetMapEnabled("UI", false));
		actions.Update(0.016f);
		CHECK(actions.IsDown(UIAction::Down));
		Step(actions, 0.016f);
		CHECK_FALSE(actions.IsDown(UIAction::Down));
		Input::Reset();
	}
}
