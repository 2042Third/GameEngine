#pragma once

#include <cstdint>

namespace Strata
{

	using MouseCode = uint16_t;

	// Values match GLFW mouse button codes.
	namespace Mouse
	{
		enum : MouseCode
		{
			Button0 = 0,
			Button1 = 1,
			Button2 = 2,
			Button3 = 3,
			Button4 = 4,
			Button5 = 5,
			Button6 = 6,
			Button7 = 7,

			ButtonLast = Button7,
			ButtonLeft = Button0,
			ButtonRight = Button1,
			ButtonMiddle = Button2
		};
	}

	constexpr MouseCode c_MaxMouseButtons = 8;

	enum class CursorMode : uint8_t
	{
		Normal = 0,
		Hidden,
		Locked // Hidden and confined to the window; reports unbounded relative motion (first-person cameras)
	};

}
