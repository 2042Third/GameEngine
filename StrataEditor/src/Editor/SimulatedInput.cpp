#include "Editor/SimulatedInput.h"

#include <Strata/Input/Input.h>
#include <Strata/Input/InputNames.h>

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>

namespace Strata
{

	namespace
	{

		void SimulateControl(SimulatedControlType type, uint16_t code, bool down)
		{
			switch (type)
			{
				case SimulatedControlType::Key:
					Input::SimulateKey(code, down);
					break;
				case SimulatedControlType::MouseButton:
					Input::SimulateMouseButton(code, down);
					break;
				case SimulatedControlType::GamepadButton:
					Input::SimulateGamepadButton(SimulatedInput::GetGamepad(code), static_cast<GamepadButton>(SimulatedInput::GetGamepadControl(code)), down);
					break;
				case SimulatedControlType::GamepadAxis:
					// Axes are set by HoldAxis; letting go returns them to the device's value.
					if (!down)
						Input::ReleaseSimulatedGamepadAxis(SimulatedInput::GetGamepad(code), static_cast<GamepadAxis>(SimulatedInput::GetGamepadControl(code)));
					break;
			}
		}

	}

	Ref<const SimulatedHold> SimulatedInput::Press(SimulatedControlType type, uint16_t code, uint32_t steps, uint64_t fixedSteps)
	{
		const uint64_t nextFrame = Input::GetFrameIndex() + 1;
		Ref<SimulatedHold> hold = CreateRef<SimulatedHold>();
		hold->Type = type;
		hold->Code = code;
		hold->FirstFrame = nextFrame;
		hold->FirstStep = fixedSteps + 1;
		hold->LastStep = steps > 0 ? fixedSteps + steps : 0;
		if (!IsHeld(type, code))
			SimulateControl(type, code, true);
		m_Holds.push_back(hold);
		return hold;
	}

	Ref<const SimulatedHold> SimulatedInput::HoldAxis(uint32_t gamepad, GamepadAxis axis, float value, uint32_t steps, uint64_t fixedSteps)
	{
		const uint64_t nextFrame = Input::GetFrameIndex() + 1;
		const uint16_t code = MakeGamepadCode(gamepad, static_cast<uint32_t>(axis));
		// The axis takes the new value: the hold before ends without letting the axis go.
		for (const Ref<SimulatedHold>& previous : m_Holds)
		{
			if (previous->Type == SimulatedControlType::GamepadAxis && previous->Code == code)
			{
				previous->Ended = true;
				previous->Interrupted = previous->IsTap();
				previous->EndFrame = nextFrame;
			}
		}
		m_Holds.erase(std::remove_if(m_Holds.begin(), m_Holds.end(), [](const Ref<SimulatedHold>& hold) { return hold->Ended; }), m_Holds.end());

		Ref<SimulatedHold> hold = CreateRef<SimulatedHold>();
		hold->Type = SimulatedControlType::GamepadAxis;
		hold->Code = code;
		hold->Value = std::isfinite(value) ? std::clamp(value, GetGamepadAxisMinimum(axis), 1.0f) : 0.0f;
		hold->FirstFrame = nextFrame;
		hold->FirstStep = fixedSteps + 1;
		hold->LastStep = steps > 0 ? fixedSteps + steps : 0;
		Input::SimulateGamepadAxis(gamepad, axis, hold->Value);
		m_Holds.push_back(hold);
		return hold;
	}

	bool SimulatedInput::Release(SimulatedControlType type, uint16_t code)
	{
		bool released = false;
		// End erases from m_Holds: collect the holds first.
		std::vector<Ref<SimulatedHold>> holds;
		for (const Ref<SimulatedHold>& hold : m_Holds)
		{
			if (hold->Type == type && hold->Code == code)
				holds.push_back(hold);
		}
		for (const Ref<SimulatedHold>& hold : holds)
		{
			End(hold, hold->IsTap());
			released = true;
		}
		return released;
	}

	bool SimulatedInput::EndHold(const Ref<const SimulatedHold>& hold)
	{
		const auto it = std::find(m_Holds.begin(), m_Holds.end(), hold);
		if (it == m_Holds.end())
			return false;
		const Ref<SimulatedHold> held = *it;
		End(held, held->IsTap());
		return true;
	}

	void SimulatedInput::ReleaseAll()
	{
		const uint64_t endFrame = Input::GetFrameIndex() + 1;
		for (const Ref<SimulatedHold>& hold : m_Holds)
		{
			hold->Ended = true;
			hold->Interrupted = hold->IsTap();
			hold->EndFrame = endFrame;
		}
		m_Holds.clear();
		Input::ReleaseAllSimulated();
	}

	void SimulatedInput::Reset()
	{
		const uint64_t endFrame = Input::GetFrameIndex() + 1;
		for (const Ref<SimulatedHold>& hold : m_Holds)
		{
			hold->Ended = true;
			hold->Interrupted = hold->IsTap();
			hold->EndFrame = endFrame;
		}
		m_Holds.clear();
	}

	void SimulatedInput::Update(uint64_t fixedSteps)
	{
		std::vector<Ref<SimulatedHold>> finished;
		for (const Ref<SimulatedHold>& hold : m_Holds)
		{
			if (hold->IsTap() && fixedSteps >= hold->LastStep)
				finished.push_back(hold);
		}
		for (const Ref<SimulatedHold>& hold : finished)
			End(hold, false);
	}

	std::vector<uint16_t> SimulatedInput::GetHeld(SimulatedControlType type) const
	{
		std::vector<uint16_t> codes;
		for (const Ref<SimulatedHold>& hold : m_Holds)
		{
			if (hold->Type == type && std::find(codes.begin(), codes.end(), hold->Code) == codes.end())
				codes.push_back(hold->Code);
		}
		std::sort(codes.begin(), codes.end());
		return codes;
	}

	float SimulatedInput::GetAxisValue(uint16_t code) const
	{
		for (const Ref<SimulatedHold>& hold : m_Holds)
		{
			if (hold->Type == SimulatedControlType::GamepadAxis && hold->Code == code)
				return hold->Value;
		}
		return 0.0f;
	}

	size_t SimulatedInput::GetHeldCount() const
	{
		return GetHeld(SimulatedControlType::Key).size() + GetHeld(SimulatedControlType::MouseButton).size()
			+ GetHeld(SimulatedControlType::GamepadButton).size() + GetHeld(SimulatedControlType::GamepadAxis).size();
	}

	std::string SimulatedInput::DescribeHolds() const
	{
		std::string text;
		auto append = [&text](const std::string& name)
		{
			text += (text.empty() ? "" : ", ") + name;
		};
		for (uint16_t key : GetHeld(SimulatedControlType::Key))
		{
			const char* name = InputNames::GetKeyName(key);
			append(name ? std::string(name) : "Key " + std::to_string(key));
		}
		for (uint16_t button : GetHeld(SimulatedControlType::MouseButton))
		{
			const char* name = InputNames::GetMouseButtonName(button);
			append("Mouse " + (name ? std::string(name) : std::to_string(button)));
		}
		for (uint16_t code : GetHeld(SimulatedControlType::GamepadButton))
		{
			const char* name = InputNames::GetGamepadButtonName(static_cast<GamepadButton>(GetGamepadControl(code)));
			append(fmt::format("Gamepad {} {}", GetGamepad(code) + 1, name ? name : std::to_string(GetGamepadControl(code))));
		}
		for (uint16_t code : GetHeld(SimulatedControlType::GamepadAxis))
		{
			const char* name = InputNames::GetGamepadAxisName(static_cast<GamepadAxis>(GetGamepadControl(code)));
			append(fmt::format("Gamepad {} {} {:.2f}", GetGamepad(code) + 1, name ? name : std::to_string(GetGamepadControl(code)), GetAxisValue(code)));
		}
		return text;
	}

	bool SimulatedInput::IsHeld(SimulatedControlType type, uint16_t code) const
	{
		return std::any_of(m_Holds.begin(), m_Holds.end(), [&](const Ref<SimulatedHold>& hold) { return hold->Type == type && hold->Code == code; });
	}

	void SimulatedInput::End(const Ref<SimulatedHold>& hold, bool interrupted)
	{
		hold->Ended = true;
		hold->Interrupted = interrupted;
		hold->EndFrame = Input::GetFrameIndex() + 1;
		m_Holds.erase(std::remove(m_Holds.begin(), m_Holds.end(), hold), m_Holds.end());
		if (!IsHeld(hold->Type, hold->Code))
			SimulateControl(hold->Type, hold->Code, false);
	}

}
