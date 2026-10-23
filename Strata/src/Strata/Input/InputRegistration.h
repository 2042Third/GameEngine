#pragma once

namespace Strata
{

	// Registers the input module with the engine's registries: the project settings section of the game's input actions
	// (Input.Actions, InputActionSet::MakeSection). Called by Engine::RegisterBuiltinModules only, first: every engine has
	// input.
	void RegisterInputModule();

}
