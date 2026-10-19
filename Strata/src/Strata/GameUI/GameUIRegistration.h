#pragma once

namespace Strata
{

	// Registers the GameUI module: its components (category "UI") and the overlay pass that draws canvases in every
	// SceneRenderer (SceneOverlayRegistry). Called by Engine::RegisterBuiltinModules only, after the modules it builds on.
	void RegisterGameUIModule();

	// The components, registered by RegisterGameUIModule (GameUIComponents.cpp).
	void RegisterGameUIComponents();

}
