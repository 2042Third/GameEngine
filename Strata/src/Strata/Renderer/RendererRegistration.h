#pragma once

namespace Strata
{

	// Registers the renderer module with the engine's registries: the loaders of textures, meshes, materials and fonts,
	// and the objects of the built-in meshes and default material. Called by Engine::RegisterBuiltinModules only.
	void RegisterRendererModule();

}
