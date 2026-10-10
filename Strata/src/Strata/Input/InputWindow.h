#pragma once

#include "Strata/Input/MouseCodes.h"

#include <cstdint>

namespace Strata
{

	// What Input needs of the window it reads (Window implements it): the size, which stands in for the input viewport
	// until one is set, and the cursor mode games ask for. Keeps the input layer below the application shell.
	class InputWindow
	{
	public:
		virtual ~InputWindow() = default;

		virtual uint32_t GetWidth() const = 0;
		virtual uint32_t GetHeight() const = 0;
		virtual void SetCursorMode(CursorMode mode) = 0;
	};

}
