#include "stpch.h"
#include "Strata/Scene/SceneSystem.h"

#include "Strata/Scripting/ScriptEngine.h"
#include "Strata/Scripting/ScriptSystem.h"

namespace Strata
{

	// Creates the engine's built-in scene systems, in update order. Called once, before the first lookup of the
	// scene system registry. Engine modules add their systems here (e.g. scripting, physics, audio).
	void CreateBuiltinSceneSystems(std::vector<SceneSystemDescriptor>& descriptors)
	{
		// Scripts update first, so systems registered after them (physics) see this frame's changes. They do not run in
		// simulate mode. Scenes use the script engine that is active when they start playing.
		descriptors.push_back({ "Scripting", false, [](Scene& scene) -> Scope<SceneSystem>
		{
			return CreateScope<ScriptSystem>(scene, ScriptEngine::GetActive());
		} });
	}

}
