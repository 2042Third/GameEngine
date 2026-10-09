#include "stpch.h"
#include "Strata/Scene/SceneSystem.h"

#include "Strata/Physics/PhysicsSystem.h"

namespace Strata
{

	// Creates the engine's built-in scene systems, in update order. Called once, before the first lookup of the
	// scene system registry. Engine modules add their systems here (e.g. scripting, physics, audio).
	void CreateBuiltinSceneSystems(std::vector<SceneSystemDescriptor>& descriptors)
	{
		// Intended order: scripting first (once it exists, so that gameplay code moves entities before they are
		// simulated), then physics, then audio (which follows the simulated transforms).
		descriptors.push_back({ "Physics", true, [](Scene& scene) -> Scope<SceneSystem> { return CreateScope<PhysicsSystem>(scene); } });
	}

}
