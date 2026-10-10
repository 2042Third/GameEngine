#pragma once

namespace Strata
{

	// Registers the scene module with the engine's registries: every built-in component and the loaders of the
	// scene-shaped assets (Scene, Prefab, Model). Called by Engine::RegisterBuiltinModules only.
	void RegisterSceneModule();

	// Registers the built-in components (ComponentRegistration.cpp); part of RegisterSceneModule.
	void RegisterSceneComponents();

}
