#include "stpch.h"
#include "Strata/Scripting/ScriptingRegistration.h"

#include "Strata/Scene/SceneSystem.h"
#include "Strata/Scripting/ScriptEngine.h"
#include "Strata/Scripting/ScriptSystem.h"

namespace Strata
{

	void RegisterScriptingModule()
	{
		// Scripts do not run in simulate mode. Scenes use the script engine that is active when they start playing.
		SceneSystemRegistry::Register({ "Scripting", false, [](Scene& scene) -> Scope<SceneSystem>
		{
			return CreateScope<ScriptSystem>(scene, ScriptEngine::GetActive());
		} });
	}

}
