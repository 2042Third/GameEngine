#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/Timestep.h"

#include <entt/entt.hpp>

#include <functional>
#include <string>
#include <type_traits>
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
	// Systems are created on Scene::OnRuntimeStart in update order (SceneSystemRegistry::GetAll) and destroyed on
	// OnRuntimeStop in reverse. Starting has two phases: every system's OnRuntimeStart, then every system's
	// OnRuntimeStarted (both in update order), so that game code run while starting (the scripts' OnCreate, in
	// OnRuntimeStarted) finds every system running.
	// Per frame the scene calls OnUpdate, then OnFixedUpdate zero or more times at the fixed timestep,
	// then OnLateUpdate. React to entity/component changes through the registry's EnTT signals.
	class SceneSystem
	{
	public:
		virtual ~SceneSystem() = default;

		virtual void OnRuntimeStart() {}
		// After every system of the scene ran OnRuntimeStart: work that uses other systems, or that should see what earlier
		// systems did here (the scripts' OnCreate runs in the scripting system's).
		virtual void OnRuntimeStarted() {}
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
		// Update order: the system runs after the systems named in After and before those named in Before (names of
		// registered systems). Systems without a constraint between them keep their registration order.
		std::vector<std::string> After;
		std::vector<std::string> Before;
		// entt::type_id of the class Create makes (set by MakeSceneSystemDescriptor), by which Scene::GetSystem finds the
		// running system; 0 for a system that is not looked up by type.
		entt::id_type Type = 0;
	};

	// A descriptor for a system of class T, which Scene::GetSystem<T> finds while the scene runs.
	template<typename T>
	SceneSystemDescriptor MakeSceneSystemDescriptor(std::string name, bool runsInSimulateMode, std::function<Scope<T>(Scene&)> factory)
	{
		static_assert(std::is_base_of_v<SceneSystem, T>, "Scene systems derive from SceneSystem");
		SceneSystemDescriptor descriptor;
		descriptor.Name = std::move(name);
		descriptor.RunsInSimulateMode = runsInSimulateMode;
		if (factory)
			descriptor.Create = [factory = std::move(factory)](Scene& scene) -> Scope<SceneSystem> { return factory(scene); };
		descriptor.Type = entt::type_id<T>().hash();
		return descriptor;
	}

	// Registry of scene system factories. Engine::RegisterBuiltinModules opens it and the engine's modules register their
	// systems; applications and tools may register more. Main thread only; register at startup, before scenes start
	// running. Using the registry before BeginRegistration fails ST_CORE_VERIFY.
	class SceneSystemRegistry
	{
	public:
		static void BeginRegistration();
		// Adds a system, or replaces the one with the same name (which then counts as registered last). Refused - false, with
		// an error naming the systems involved, and the registry unchanged - for a descriptor without a name or factory, a
		// type registered under another name, a constraint naming an unregistered system or the system itself, constraints
		// that form a cycle, and while any scene runs.
		[[nodiscard]] static bool Register(SceneSystemDescriptor descriptor);
		// Refused (false) for a name that is not registered, while other systems name it in a constraint (with an error naming
		// them) and while any scene runs.
		[[nodiscard]] static bool Unregister(const std::string& name);
		// Every system in update order: a stable topological order of the constraints in which registration order decides
		// between systems that are not constrained against each other.
		static const std::vector<SceneSystemDescriptor>& GetAll();
	};

}
