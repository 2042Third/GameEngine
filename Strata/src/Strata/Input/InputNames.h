#pragma once

#include "Strata/Input/KeyCodes.h"
#include "Strata/Input/MouseCodes.h"

#include <optional>
#include <span>
#include <string_view>

namespace Strata
{

	// Names of keys and mouse buttons for tools and files (e.g. the editor's input.* commands). Keys are spelled like the
	// Key:: constants of the engine and the script SDK ("A", "D1", "Space", "Left", "F5", "KPEnter", "LeftShift"); mouse
	// buttons are "Left", "Right", "Middle" and "Button3" to "Button7" ("Button0" to "Button2" are accepted too).
	// Lookups ignore case.
	namespace InputNames
	{

		// Null for codes that name no key.
		const char* GetKeyName(KeyCode key);
		std::optional<KeyCode> FindKey(std::string_view name);
		// Every key name, in key code order.
		std::span<const char* const> GetKeyNames();

		// Null for codes that name no button.
		const char* GetMouseButtonName(MouseCode button);
		std::optional<MouseCode> FindMouseButton(std::string_view name);
		// The preferred name of every button, in code order.
		std::span<const char* const> GetMouseButtonNames();

	}

}
