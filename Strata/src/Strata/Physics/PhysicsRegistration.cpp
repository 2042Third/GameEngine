#include "stpch.h"
#include "Strata/Physics/PhysicsRegistration.h"

#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Scene/SceneSystem.h"

namespace Strata
{

	void RegisterPhysicsModule()
	{
		// Physics simulates what the scripts moved this frame (another module's system: a soft constraint). It also runs in the
		// editor's simulate mode.
		SceneSystemDescriptor descriptor = MakeSceneSystemDescriptor<PhysicsSystem>("Physics", true, [](Scene& scene) { return CreateScope<PhysicsSystem>(scene); });
		descriptor.AfterIfPresent = { "Scripting" };
		descriptor.ProfileStage = "Scene.Physics";
		const bool registered = SceneSystemRegistry::Register(std::move(descriptor));
		ST_CORE_VERIFY(registered, "The physics module could not register its scene system");
	}

}
