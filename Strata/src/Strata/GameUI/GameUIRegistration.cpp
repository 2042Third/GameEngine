#include "stpch.h"
#include "Strata/GameUI/GameUIRegistration.h"

#include "Strata/GameUI/GameUIRenderer.h"

namespace Strata
{

	void RegisterGameUIModule()
	{
		RegisterGameUIComponents();
		// Every SceneRenderer draws the game UI over the finished image (Renderer/SceneOverlay.h).
		RegisterGameUIRenderer();
	}

}
