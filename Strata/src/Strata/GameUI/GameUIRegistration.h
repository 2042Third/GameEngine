#pragma once

namespace Strata
{

	// Registers the GameUI module: its components (category "UI"). Called by Engine::RegisterBuiltinModules only, after the
	// modules it builds on.
	void RegisterGameUIModule();

	// The components, registered by RegisterGameUIModule (GameUIComponents.cpp).
	void RegisterGameUIComponents();

}
