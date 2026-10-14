#pragma once

namespace Strata
{

	// Registers the world module with the engine's registries: the World asset loader, the World and StreamingSource
	// components and the "World" scene system (WorldStreamingSystem). Called by Engine::RegisterBuiltinModules only, after
	// the scripting and physics modules (its system runs between them).
	void RegisterWorldModule();

}
