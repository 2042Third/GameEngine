#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"

#include <functional>
#include <string>
#include <vector>

namespace Strata
{

	class Entity;
	class Scene;

	enum class SceneRuntimeMode : uint8_t
	{
		Play,    // Full game simulation: scripts, physics, audio
		Simulate // Physics only (editor "simulate" mode); no scripts or audio
	};

	// A runtime subsystem attached to a scene while it is playing (physics world, script instances, audio).
	// Systems are created on Scene::OnRuntimeStart in registration order and destroyed on OnRuntimeStop.
	// Per frame the scene calls OnUpdate, then OnFixedUpdate zero or more times at the fixed timestep,
	// then OnLateUpdate. React to entity/component changes through the registry's EnTT signals.
	class SceneSystem
	{
	public:
		virtual ~SceneSystem() = default;

		virtual void OnRuntimeStart() {}
		virtual void OnRuntimeStop() {}
		virtual void OnUpdate(Timestep) {}
		virtual void OnFixedUpdate(float) {}
		virtual void OnLateUpdate(Timestep) {}

		// Called while the scene is running when it is paused or resumed (Scene::SetPaused, e.g. the editor's pause button).
		// While paused the scene calls no update functions (except for Scene::Step), so systems that keep running on their own
		// (audio playback) halt here.
		virtual void OnPausedChanged(bool) {}

		// Called while the scene is running, right before an entity is destroyed (descendants before their ancestors).
		// The entity is still complete and valid. Destruction requested from here is deferred; entities attached to
		// the dying hierarchy from here are announced (and destroyed) as well.
		virtual void OnEntityDestroying(const Entity&) {}
	};

	struct SceneSystemDescriptor
	{
		std::string Name;
		bool RunsInSimulateMode = false;
		std::function<Scope<SceneSystem>(Scene&)> Create;
	};

	// Registry of scene system factories. The engine's own systems are created by CreateBuiltinSceneSystems
	// (SceneSystemRegistration.cpp) on first use; applications and tools may register more. The order of registration
	// is the update order. Main thread only; register at startup, before scenes start running.
	class SceneSystemRegistry
	{
	public:
		static void Register(SceneSystemDescriptor descriptor);
		static void Unregister(const std::string& name);
		static const std::vector<SceneSystemDescriptor>& GetAll();
	};

}
