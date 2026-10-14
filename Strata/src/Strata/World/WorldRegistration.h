#pragma once

namespace Strata
{

	// Registers the world module with the engine's registries: the World asset loader. Called by
	// Engine::RegisterBuiltinModules only, after the animation module.
	void RegisterWorldModule();

}
