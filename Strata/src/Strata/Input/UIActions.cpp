#include "stpch.h"
#include "Strata/Input/UIActions.h"

#include "Strata/Core/Assert.h"
#include "Strata/Input/Input.h"
#include "Strata/Input/InputNames.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Strata
{

	namespace
	{

		// A control of c_UIActionMap, parsed once.
		struct CompiledBinding
		{
			enum class Kind : uint8_t
			{
				Key = 0,
				GamepadButton,
				Stick
			};

			Kind Type = Kind::Key;
			KeyCode Key = 0;
			GamepadButton Button = GamepadButton::A;
			GamepadAxis Axis = GamepadAxis::LeftX;
			float Sign = 1.0f;
		};

		using CompiledMap = std::array<std::vector<CompiledBinding>, c_UIActionCount>;

		CompiledMap CompileMap()
		{
			CompiledMap map;
			for (uint32_t action = 0; action < c_UIActionCount; action++)
			{
				const UIActionBinding& entry = c_UIActionMap[action];
				ST_CORE_ASSERT(entry.Action == GetUIActionName(static_cast<UIAction>(action)), "c_UIActionMap lists the actions in UIAction order");
				for (const std::string_view binding : entry.Bindings)
				{
					if (binding.empty())
						continue;
					CompiledBinding compiled;
					bool known = false;
					if (binding.starts_with("Key/"))
					{
						const std::optional<KeyCode> key = InputNames::FindKey(binding.substr(4));
						known = key.has_value();
						compiled.Key = key.value_or(0);
					}
					else if (binding.starts_with("Gamepad/"))
					{
						std::string_view name = binding.substr(8);
						if (name.ends_with('+') || name.ends_with('-'))
						{
							compiled.Type = CompiledBinding::Kind::Stick;
							compiled.Sign = name.back() == '+' ? 1.0f : -1.0f;
							const std::optional<GamepadAxis> axis = InputNames::FindGamepadAxis(name.substr(0, name.size() - 1));
							known = axis.has_value();
							compiled.Axis = axis.value_or(GamepadAxis::LeftX);
						}
						else
						{
							compiled.Type = CompiledBinding::Kind::GamepadButton;
							const std::optional<GamepadButton> button = InputNames::FindGamepadButton(name);
							known = button.has_value();
							compiled.Button = button.value_or(GamepadButton::A);
						}
					}
					ST_CORE_VERIFY(known, "c_UIActionMap names an unknown control '{}'", binding);
					map[action].push_back(compiled);
				}
				const auto sticks = std::count_if(map[action].begin(), map[action].end(), [](const CompiledBinding& compiled) { return compiled.Type == CompiledBinding::Kind::Stick; });
				ST_CORE_VERIFY(sticks <= 1, "c_UIActionMap binds more than one stick direction to '{}'", entry.Action);
			}
			return map;
		}

		const CompiledMap& GetCompiledMap()
		{
			static const CompiledMap s_Map = CompileMap();
			return s_Map;
		}

	}

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

	void UIActionState::Update(float deltaSeconds)
	{
		const float delta = std::isfinite(deltaSeconds) && deltaSeconds > 0.0f ? deltaSeconds : 0.0f;
		const CompiledMap& map = GetCompiledMap();
		for (uint32_t action = 0; action < c_UIActionCount; action++)
		{
			bool down = false;
			bool pressed = false;
			for (const CompiledBinding& binding : map[action])
			{
				switch (binding.Type)
				{
					case CompiledBinding::Kind::Key:
						down = down || Input::IsKeyDown(binding.Key);
						pressed = pressed || Input::IsKeyPressed(binding.Key);
						break;
					case CompiledBinding::Kind::GamepadButton:
						for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
						{
							down = down || Input::IsGamepadButtonDown(gamepad, binding.Button);
							pressed = pressed || Input::IsGamepadButtonPressed(gamepad, binding.Button);
						}
						break;
					case CompiledBinding::Kind::Stick:
						for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
						{
							// Pushed beyond c_StickPress, released below c_StickRelease: a stick resting near the threshold
							// does not flicker.
							bool& held = m_StickHeld[gamepad][action];
							const float value = Input::IsGamepadConnected(gamepad) ? Input::GetGamepadAxis(gamepad, binding.Axis) * binding.Sign : 0.0f;
							held = held ? value > c_StickRelease : value > c_StickPress;
							down = down || held;
						}
						break;
				}
			}

			ActionRecord& record = m_Actions[action];
			const bool wasDown = record.Down;
			record.Pressed = pressed || (down && !wasDown);
			record.Down = down;
			record.Repeated = false;
			if (record.Pressed)
			{
				record.Repeated = true;
				record.HeldTime = 0.0f;
				record.NextRepeat = c_RepeatDelay;
			}
			else if (down)
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
		}
	}

	void UIActionState::Reset()
	{
		m_Actions = {};
		m_StickHeld = {};
	}

}
