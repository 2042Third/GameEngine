#include "stpch.h"
#include "Strata/Scene/SceneSystem.h"

namespace Strata
{

	// Creates the engine's built-in scene systems, in update order. Called once, before the first lookup of the
	// scene system registry. Engine modules add their systems here (e.g. scripting, physics, audio).
	void CreateBuiltinSceneSystems(std::vector<SceneSystemDescriptor>&)
	{
	}

}
