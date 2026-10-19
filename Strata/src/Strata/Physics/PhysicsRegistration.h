#pragma once

namespace Strata
{

	// Registers the physics module with the engine's registries: its components and the "Physics" scene system. Called by
	// Engine::RegisterBuiltinModules only.
	void RegisterPhysicsModule();

	// Registers the physics components (RigidBody, the colliders and CharacterController, with their gizmos:
	// PhysicsComponents.cpp); part of RegisterPhysicsModule.
	void RegisterPhysicsComponents();

}
