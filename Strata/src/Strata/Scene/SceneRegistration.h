#pragma once

namespace Strata
{

	// Registers the scene module with the engine's registries: its components, the loaders of the scene-shaped assets
	// (Scene, Prefab, Model) and the project settings section of the collision layers (ProjectCollisionLayers). Called by
	// Engine::RegisterBuiltinModules only.
	void RegisterSceneModule();

	// Registers the scene's components (ID, Name, Transform, Relationship, Tag, Inactive, PrefabInstance and Camera:
	// ComponentRegistration.cpp); part of RegisterSceneModule.
	void RegisterSceneComponents();

}
