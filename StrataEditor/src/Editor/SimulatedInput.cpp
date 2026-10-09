#include "Editor/SimulatedInput.h"

#include <Strata/Input/Input.h>
#include <Strata/Input/InputNames.h>

#include <algorithm>

namespace Strata
{

	namespace
	{

		void SimulateButton(SimulatedButtonType type, uint16_t code, bool down)
		{
			if (type == SimulatedButtonType::Key)
				Input::SimulateKey(code, down);
			else
				Input::SimulateMouseButton(code, down);
		}

	}

	Ref<const SimulatedHold> SimulatedInput::Press(SimulatedButtonType type, uint16_t code, uint32_t frames)
	{
		const uint64_t nextFrame = Input::GetFrameIndex() + 1;
		Ref<SimulatedHold> hold = CreateRef<SimulatedHold>();
		hold->Type = type;
		hold->Code = code;
		hold->FirstFrame = nextFrame;
		hold->LastFrame = frames > 0 ? nextFrame + frames - 1 : 0;
		if (!IsHeld(type, code))
			SimulateButton(type, code, true);
		m_Holds.push_back(hold);
		return hold;
	}

	bool SimulatedInput::Release(SimulatedButtonType type, uint16_t code)
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
			End(hold, hold->LastFrame != 0);
			released = true;
		}
		return released;
	}

	void SimulatedInput::ReleaseAll()
	{
		const uint64_t endFrame = Input::GetFrameIndex() + 1;
		for (const Ref<SimulatedHold>& hold : m_Holds)
		{
			hold->Ended = true;
			hold->Interrupted = hold->LastFrame != 0;
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
			hold->Interrupted = hold->LastFrame != 0;
			hold->EndFrame = endFrame;
		}
		m_Holds.clear();
	}

	void SimulatedInput::Update()
	{
		const uint64_t frame = Input::GetFrameIndex();
		std::vector<Ref<SimulatedHold>> finished;
		for (const Ref<SimulatedHold>& hold : m_Holds)
		{
			if (hold->LastFrame != 0 && frame >= hold->LastFrame)
				finished.push_back(hold);
		}
		for (const Ref<SimulatedHold>& hold : finished)
			End(hold, false);
	}

	std::vector<uint16_t> SimulatedInput::GetHeld(SimulatedButtonType type) const
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

	std::string SimulatedInput::DescribeHolds() const
	{
		std::string text;
		auto append = [&text](const std::string& name)
		{
			text += (text.empty() ? "" : ", ") + name;
		};
		for (uint16_t key : GetHeld(SimulatedButtonType::Key))
		{
			const char* name = InputNames::GetKeyName(key);
			append(name ? std::string(name) : "Key " + std::to_string(key));
		}
		for (uint16_t button : GetHeld(SimulatedButtonType::MouseButton))
		{
			const char* name = InputNames::GetMouseButtonName(button);
			append("Mouse " + (name ? std::string(name) : std::to_string(button)));
		}
		return text;
	}

	bool SimulatedInput::IsHeld(SimulatedButtonType type, uint16_t code) const
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
			SimulateButton(hold->Type, hold->Code, false);
	}

}
