#include "stpch.h"
#include "Strata/Input/InputNames.h"

#include "Strata/Core/StringUtils.h"

#include <array>
#include <iterator>

namespace Strata
{

	namespace
	{

		struct KeyEntry
		{
			KeyCode Code;
			const char* Name;
		};

		// In code order. The names are those of the Key:: constants (KeyCodes.h), which tools and scripts know.
		constexpr KeyEntry c_Keys[] = {
			{ Key::Space, "Space" }, { Key::Apostrophe, "Apostrophe" }, { Key::Comma, "Comma" }, { Key::Minus, "Minus" },
			{ Key::Period, "Period" }, { Key::Slash, "Slash" },
			{ Key::D0, "D0" }, { Key::D1, "D1" }, { Key::D2, "D2" }, { Key::D3, "D3" }, { Key::D4, "D4" }, { Key::D5, "D5" },
			{ Key::D6, "D6" }, { Key::D7, "D7" }, { Key::D8, "D8" }, { Key::D9, "D9" },
			{ Key::Semicolon, "Semicolon" }, { Key::Equal, "Equal" },
			{ Key::A, "A" }, { Key::B, "B" }, { Key::C, "C" }, { Key::D, "D" }, { Key::E, "E" }, { Key::F, "F" }, { Key::G, "G" },
			{ Key::H, "H" }, { Key::I, "I" }, { Key::J, "J" }, { Key::K, "K" }, { Key::L, "L" }, { Key::M, "M" }, { Key::N, "N" },
			{ Key::O, "O" }, { Key::P, "P" }, { Key::Q, "Q" }, { Key::R, "R" }, { Key::S, "S" }, { Key::T, "T" }, { Key::U, "U" },
			{ Key::V, "V" }, { Key::W, "W" }, { Key::X, "X" }, { Key::Y, "Y" }, { Key::Z, "Z" },
			{ Key::LeftBracket, "LeftBracket" }, { Key::Backslash, "Backslash" }, { Key::RightBracket, "RightBracket" },
			{ Key::GraveAccent, "GraveAccent" }, { Key::World1, "World1" }, { Key::World2, "World2" },
			{ Key::Escape, "Escape" }, { Key::Enter, "Enter" }, { Key::Tab, "Tab" }, { Key::Backspace, "Backspace" },
			{ Key::Insert, "Insert" }, { Key::Delete, "Delete" }, { Key::Right, "Right" }, { Key::Left, "Left" },
			{ Key::Down, "Down" }, { Key::Up, "Up" }, { Key::PageUp, "PageUp" }, { Key::PageDown, "PageDown" },
			{ Key::Home, "Home" }, { Key::End, "End" }, { Key::CapsLock, "CapsLock" }, { Key::ScrollLock, "ScrollLock" },
			{ Key::NumLock, "NumLock" }, { Key::PrintScreen, "PrintScreen" }, { Key::Pause, "Pause" },
			{ Key::F1, "F1" }, { Key::F2, "F2" }, { Key::F3, "F3" }, { Key::F4, "F4" }, { Key::F5, "F5" }, { Key::F6, "F6" },
			{ Key::F7, "F7" }, { Key::F8, "F8" }, { Key::F9, "F9" }, { Key::F10, "F10" }, { Key::F11, "F11" }, { Key::F12, "F12" },
			{ Key::F13, "F13" }, { Key::F14, "F14" }, { Key::F15, "F15" }, { Key::F16, "F16" }, { Key::F17, "F17" },
			{ Key::F18, "F18" }, { Key::F19, "F19" }, { Key::F20, "F20" }, { Key::F21, "F21" }, { Key::F22, "F22" },
			{ Key::F23, "F23" }, { Key::F24, "F24" }, { Key::F25, "F25" },
			{ Key::KP0, "KP0" }, { Key::KP1, "KP1" }, { Key::KP2, "KP2" }, { Key::KP3, "KP3" }, { Key::KP4, "KP4" },
			{ Key::KP5, "KP5" }, { Key::KP6, "KP6" }, { Key::KP7, "KP7" }, { Key::KP8, "KP8" }, { Key::KP9, "KP9" },
			{ Key::KPDecimal, "KPDecimal" }, { Key::KPDivide, "KPDivide" }, { Key::KPMultiply, "KPMultiply" },
			{ Key::KPSubtract, "KPSubtract" }, { Key::KPAdd, "KPAdd" }, { Key::KPEnter, "KPEnter" }, { Key::KPEqual, "KPEqual" },
			{ Key::LeftShift, "LeftShift" }, { Key::LeftControl, "LeftControl" }, { Key::LeftAlt, "LeftAlt" },
			{ Key::LeftSuper, "LeftSuper" }, { Key::RightShift, "RightShift" }, { Key::RightControl, "RightControl" },
			{ Key::RightAlt, "RightAlt" }, { Key::RightSuper, "RightSuper" }, { Key::Menu, "Menu" }
		};

		constexpr std::array<const char*, std::size(c_Keys)> c_KeyNames = []()
		{
			std::array<const char*, std::size(c_Keys)> names = {};
			for (size_t index = 0; index < names.size(); index++)
				names[index] = c_Keys[index].Name;
			return names;
		}();

		constexpr std::array<const char*, c_MaxMouseButtons> c_MouseButtonNames = {
			"Left", "Right", "Middle", "Button3", "Button4", "Button5", "Button6", "Button7"
		};

		// The numbered names of the first three buttons, accepted as aliases of Left, Right and Middle.
		constexpr std::array<const char*, 3> c_MouseButtonAliases = { "Button0", "Button1", "Button2" };

	}

	namespace InputNames
	{

		const char* GetKeyName(KeyCode key)
		{
			for (const KeyEntry& entry : c_Keys)
			{
				if (entry.Code == key)
					return entry.Name;
			}
			return nullptr;
		}

		std::optional<KeyCode> FindKey(std::string_view name)
		{
			for (const KeyEntry& entry : c_Keys)
			{
				if (StringUtils::EqualsIgnoreCase(entry.Name, name))
					return entry.Code;
			}
			return std::nullopt;
		}

		std::span<const char* const> GetKeyNames()
		{
			return c_KeyNames;
		}

		const char* GetMouseButtonName(MouseCode button)
		{
			return button < c_MouseButtonNames.size() ? c_MouseButtonNames[button] : nullptr;
		}

		std::optional<MouseCode> FindMouseButton(std::string_view name)
		{
			for (size_t index = 0; index < c_MouseButtonNames.size(); index++)
			{
				if (StringUtils::EqualsIgnoreCase(c_MouseButtonNames[index], name))
					return static_cast<MouseCode>(index);
			}
			for (size_t index = 0; index < c_MouseButtonAliases.size(); index++)
			{
				if (StringUtils::EqualsIgnoreCase(c_MouseButtonAliases[index], name))
					return static_cast<MouseCode>(index);
			}
			return std::nullopt;
		}

		std::span<const char* const> GetMouseButtonNames()
		{
			return c_MouseButtonNames;
		}

	}

}
