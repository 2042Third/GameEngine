#pragma once

// Engine services: interfaces that a module offers the whole process, so that modules which may not include it (the layers
// of StrataTests/Architecture/Layers.json) can still use it. They are like scene services (Scene/SceneServices.h), but not
// tied to a running scene: the master volume of the audio output, for instance, outlives every scene.
//
// A module registers its providers in its registration function, while the composition root runs
// (Engine::RegisterBuiltinModules, on the main thread, before anything uses them), and they stay registered for the rest of
// the process. Get returns null for an interface that no module of the build provides, which callers handle like a missing
// feature. Main thread only.
namespace Strata::EngineServices
{

	namespace Detail
	{
		template<typename T>
		inline T* s_Provider = nullptr;
	}

	// Makes `provider` the process's provider of interface T (replacing an earlier one); it must live until the process ends.
	template<typename T>
	void Register(T& provider)
	{
		Detail::s_Provider<T> = &provider;
	}

	template<typename T>
	T* Get()
	{
		return Detail::s_Provider<T>;
	}

}
