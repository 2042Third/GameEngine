#include <doctest/doctest.h>

#include "Perf/PerfUtils.h"

#include "Strata/Core/EngineServices.h"
#include "Strata/Core/ProjectSettings.h"
#include "Strata/Core/SettingsRegistry.h"
#include "Strata/Input/Input.h"
#include "Strata/Input/InputActions.h"
#include "Strata/Input/InputActionSet.h"

#include <nlohmann/json.hpp>

#include <string>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	constexpr uint32_t c_FramesPerBatch = 1000;

	// 256 actions of 4 bindings (the UI map's six included), of every kind and control: keys, mouse and gamepad buttons, half
	// and whole axes, sticks and composites.
	nlohmann::json MakeActions256()
	{
		const char* buttonBindings[4] = { "Key/Space", "Mouse/Left", "Gamepad/A", "Gamepad/RightTrigger+" };
		const char* axisBindings[4] = { "1D:Key/A,Key/D", "Gamepad/LeftX", "1D:Gamepad/LeftTrigger+,Gamepad/RightTrigger+", "Key/W" };
		const char* vectorBindings[4] = { "2D:Key/W,Key/S,Key/A,Key/D", "Gamepad/LeftStick", "Gamepad/RightStick",
			"2D:Gamepad/DPadUp,Gamepad/DPadDown,Gamepad/LeftX-,Gamepad/LeftX+" };
		nlohmann::json uiActions = nlohmann::json::array();
		for (const InputAction& action : InputActionSet::GetDefaultUIActions())
			uiActions.push_back({ { "Name", action.Name }, { "Bindings", { buttonBindings[0], buttonBindings[1], buttonBindings[2], buttonBindings[3] } } });
		nlohmann::json maps = nlohmann::json::array({ { { "Name", "UI" }, { "Actions", uiActions } } });
		size_t actions = uiActions.size();
		for (uint32_t map = 0; actions < InputActionSet::c_MaxActions; map++)
		{
			nlohmann::json mapActions = nlohmann::json::array();
			for (uint32_t index = 0; index < 50 && actions < InputActionSet::c_MaxActions; index++, actions++)
			{
				const uint32_t kind = index % 3;
				const char** bindings = kind == 0 ? buttonBindings : (kind == 1 ? axisBindings : vectorBindings);
				mapActions.push_back({ { "Name", "Action " + std::to_string(index) }, { "Kind", kind == 0 ? "Button" : (kind == 1 ? "Axis" : "Vector2") },
					{ "Bindings", { bindings[0], bindings[1], bindings[2], bindings[3] } } });
			}
			maps.push_back({ { "Name", "Map " + std::to_string(map) }, { "Actions", mapActions } });
		}
		return { { "Maps", maps } };
	}

}

TEST_SUITE("Perf.Input")
{
	TEST_CASE("Evaluating 256 actions with every gamepad connected costs microseconds per input frame")
	{
		SettingsRegistry registry;
		REQUIRE(registry.Register(InputActionSet::MakeSection()));
		ProjectSettingsValues values;
		std::string error;
		REQUIRE_MESSAGE(values.Set(InputActionSet::c_SectionId, MakeActions256(), registry, &error), error);
		EngineServices::ScopedProvider<IProjectSettings> provider(values);
		Input::Reset();

		// Every gamepad connected with its sticks pushed and a button held, so that every binding reads its control.
		const float axes[c_GamepadAxisCount] = { 0.6f, -0.4f, 0.3f, 0.8f, 0.2f, 0.7f };
		bool buttons[c_GamepadButtonCount] = {};
		buttons[static_cast<uint32_t>(GamepadButton::A)] = true;
		for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
			Input::ProcessGamepad(gamepad, true, axes, buttons);
		Input::ProcessKey(Key::W, true);
		Input::BeginFrame();
		InputActions::Update();
		REQUIRE(InputActions::GetSet().GetActionCount() == InputActionSet::c_MaxActions);
		REQUIRE(InputActions::IsDown("Map 0/Action 0"));

		Perf::CheckBudgetConfirmed("Perf.Input.Actions256", [&]
		{
			const Perf::Measurement batches = Perf::Measure("Perf.Input.Actions256 (1000 input frames)", 2, 30, [&]
			{
				for (uint32_t frame = 0; frame < c_FramesPerBatch; frame++)
				{
					Input::BeginFrame();
					InputActions::Update();
				}
			});
			return batches.MedianMs * 1000.0 / c_FramesPerBatch;
		});
		Input::Reset();
	}
}
