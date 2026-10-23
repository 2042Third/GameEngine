#include "stpch.h"
#include "Strata/Input/UIActions.h"

#include "Strata/Input/InputActions.h"

#include <cmath>

namespace Strata
{

	const char* GetUIActionName(UIAction action)
	{
		switch (action)
		{
			case UIAction::Submit: return "Submit";
			case UIAction::Cancel: return "Cancel";
			case UIAction::Up: return "Up";
			case UIAction::Down: return "Down";
			case UIAction::Left: return "Left";
			case UIAction::Right: return "Right";
		}
		return "Unknown";
	}

	std::string_view GetUIActionQualifiedName(UIAction action)
	{
		switch (action)
		{
			case UIAction::Submit: return "UI/Submit";
			case UIAction::Cancel: return "UI/Cancel";
			case UIAction::Up: return "UI/Up";
			case UIAction::Down: return "UI/Down";
			case UIAction::Left: return "UI/Left";
			case UIAction::Right: return "UI/Right";
		}
		return "UI/Unknown";
	}

	void UIActionState::Update(float deltaSeconds)
	{
		// Nothing held and nothing changed since the last update: this one would read the same.
		const uint64_t stateVersion = InputActions::GetStateVersion();
		if (stateVersion == m_StateVersion && !m_Active)
			return;
		m_StateVersion = stateVersion;
		const uint64_t setVersion = InputActions::GetVersion();
		if (setVersion != m_SetVersion)
		{
			// Every set has the UI map's actions (InputActionSet::FromJson adds them), so the lookups find them.
			m_SetVersion = setVersion;
			for (uint32_t action = 0; action < c_UIActionCount; action++)
				m_Indices[action] = InputActions::FindIndex(GetUIActionQualifiedName(static_cast<UIAction>(action)));
		}

		const float delta = std::isfinite(deltaSeconds) && deltaSeconds > 0.0f ? deltaSeconds : 0.0f;
		m_Active = false;
		for (uint32_t action = 0; action < c_UIActionCount; action++)
		{
			const InputActionState state = m_Indices[action] ? InputActions::GetStateAt(*m_Indices[action]) : InputActionState();
			ActionRecord& record = m_Actions[action];
			record.Pressed = state.Pressed;
			record.Down = state.Down;
			record.Repeated = false;
			if (record.Pressed)
			{
				record.Repeated = true;
				record.HeldTime = 0.0f;
				record.NextRepeat = c_RepeatDelay;
			}
			else if (state.Down)
			{
				// One repeat an update at most; time beyond it is not owed later.
				record.HeldTime += delta;
				if (record.HeldTime >= record.NextRepeat)
				{
					record.Repeated = true;
					while (record.NextRepeat <= record.HeldTime)
						record.NextRepeat += c_RepeatInterval;
				}
			}
			else
			{
				record.HeldTime = 0.0f;
			}
			m_Active = m_Active || record.Down || record.Pressed || record.Repeated;
		}
	}

	void UIActionState::Reset()
	{
		m_Actions = {};
		m_StateVersion = 0;
		m_Active = false;
	}

}
