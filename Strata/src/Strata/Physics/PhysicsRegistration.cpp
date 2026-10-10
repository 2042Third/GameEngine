#include "stpch.h"
#include "Strata/Physics/PhysicsRegistration.h"

#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Scene/SceneSystem.h"

namespace Strata
{

	void RegisterPhysicsModule()
	{
		// Physics also runs in the editor's simulate mode.
		SceneSystemRegistry::Register({ "Physics", true, [](Scene& scene) -> Scope<SceneSystem> { return CreateScope<PhysicsSystem>(scene); } });
	}

}
