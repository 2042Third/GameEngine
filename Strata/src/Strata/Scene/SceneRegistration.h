#pragma once

namespace Strata
{

	// Registers the scene module with the engine's registries: its components and the loaders of the scene-shaped assets
	// (Scene, Prefab, Model). Called by Engine::RegisterBuiltinModules only.
	void RegisterSceneModule();

	// Registers the scene's components (ID, Name, Transform, Relationship, Tag, Inactive, PrefabInstance and Camera:
	// ComponentRegistration.cpp); part of RegisterSceneModule.
	void RegisterSceneComponents();

}
