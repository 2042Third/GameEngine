#include "stpch.h"
#include "Strata/Scene/SceneSystem.h"

#include "Strata/Audio/AudioSystem.h"
#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "Strata/Scripting/ScriptSystem.h"

namespace Strata
{

	// Creates the engine's built-in scene systems, in update order. Called once, before the first lookup of the
	// scene system registry. Engine modules add their systems here (e.g. scripting, physics, audio).
	void CreateBuiltinSceneSystems(std::vector<SceneSystemDescriptor>& descriptors)
	{
		// Scripts update first, so gameplay code moves entities before they are simulated. They do not run in simulate
		// mode. Scenes use the script engine that is active when they start playing.
		descriptors.push_back({ "Scripting", false, [](Scene& scene) -> Scope<SceneSystem>
		{
			return CreateScope<ScriptSystem>(scene, ScriptEngine::GetActive());
		} });
		descriptors.push_back({ "Physics", true, [](Scene& scene) -> Scope<SceneSystem> { return CreateScope<PhysicsSystem>(scene); } });
		// Audio updates last (in OnLateUpdate), so sources and the listener follow the transforms scripts and physics produced this
		// frame. Like scripts, it does not run in simulate mode.
		descriptors.push_back({ "Audio", false, [](Scene& scene) -> Scope<SceneSystem> { return CreateScope<AudioSystem>(scene); } });
	}

}
