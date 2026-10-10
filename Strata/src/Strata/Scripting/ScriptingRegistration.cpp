#include "stpch.h"
#include "Strata/Scripting/ScriptingRegistration.h"

#include "Strata/Scene/SceneSystem.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "Strata/Scripting/ScriptSystem.h"

namespace Strata
{

	void RegisterScriptingModule()
	{
		// Scripts update first, so gameplay code moves entities before they are simulated (Physics runs after Scripting).
		// They do not run in simulate mode. Scenes use the script engine that is active when they start playing.
		const bool registered = SceneSystemRegistry::Register(MakeSceneSystemDescriptor<ScriptSystem>("Scripting", false, [](Scene& scene)
		{
			return CreateScope<ScriptSystem>(scene, ScriptEngine::GetActive());
		}));
		ST_CORE_VERIFY(registered, "The scripting module could not register its scene system");
	}

}
