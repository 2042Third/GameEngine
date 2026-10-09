#include "stpch.h"
#include "Strata/Scene/SceneSystem.h"

#include "Strata/Physics/PhysicsSystem.h"

namespace Strata
{

	namespace
	{

		// Registers the physics system during static initialization. It cannot be registered from
		// RegisterBuiltinSceneSystems: that function runs inside the registry's std::call_once, and
		// SceneSystemRegistry::Register looks the registry up again, re-entering the same call_once (a deadlock).
		// This translation unit is always linked because Scene.cpp references RegisterBuiltinSceneSystems, and the
		// registry's storage is a function-local static, so registering before main is safe.
		struct BuiltinSceneSystemRegistrar
		{
			BuiltinSceneSystemRegistrar()
			{
				SceneSystemRegistry::Register({ "Physics", true, [](Scene& scene) -> Scope<SceneSystem> { return CreateScope<PhysicsSystem>(scene); } });
			}
		};

		const BuiltinSceneSystemRegistrar s_BuiltinSceneSystemRegistrar;

	}

	// Registers the engine's built-in scene systems, in update order. Called once, before the first lookup of the
	// scene system registry. Engine modules add their systems here (e.g. scripting, physics, audio).
	void RegisterBuiltinSceneSystems()
	{
	}

}
