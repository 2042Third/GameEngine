#pragma once

namespace Strata
{

	// Registers the scripting module with the engine's registries: the Script component and the "Scripting" scene system.
	// Called by Engine::RegisterBuiltinModules only.
	void RegisterScriptingModule();

	// Registers the Script component (ScriptingComponents.cpp); part of RegisterScriptingModule.
	void RegisterScriptingComponents();

}
