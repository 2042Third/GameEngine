#include "stpch.h"
#include "Strata/Core/Window.h"

namespace Strata
{

	// The window backend of a build without GLFW (STRATA_WITH_GLFW OFF): none. Programs run headless (--headless,
	// ApplicationSpecification::Headless) or with a window their embedder makes (ApplicationSpecification::WindowFactory);
	// Application reports a windowed start through its fatal-error path.
	Scope<Window> Window::Create(const WindowSpecification& specification)
	{
		ST_CORE_ERROR("Cannot open the window '{}': this build has no window backend (STRATA_WITH_GLFW is OFF); run it headless",
			specification.Title);
		return nullptr;
	}

}
