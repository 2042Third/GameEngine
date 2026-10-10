#include "stpch.h"
#include "Strata/Physics/PhysicsRegistration.h"

#include "Strata/Physics/PhysicsSystem.h"
#include "Strata/Scene/SceneSystem.h"

namespace Strata
{

	void RegisterPhysicsModule()
	{
		// Physics simulates what the scripts moved this frame. It also runs in the editor's simulate mode.
		SceneSystemDescriptor descriptor = MakeSceneSystemDescriptor<PhysicsSystem>("Physics", true, [](Scene& scene) { return CreateScope<PhysicsSystem>(scene); });
		descriptor.After = { "Scripting" };
		const bool registered = SceneSystemRegistry::Register(std::move(descriptor));
		ST_CORE_VERIFY(registered, "The physics module could not register its scene system");
	}

}
