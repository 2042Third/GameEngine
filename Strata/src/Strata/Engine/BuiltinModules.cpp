#include "stpch.h"
#include "Strata/Engine/BuiltinModules.h"

#include "Strata/Asset/AssetImporter.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Audio/AudioRegistration.h"
#include "Strata/Physics/PhysicsRegistration.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Renderer/RendererRegistration.h"
#include "Strata/Scene/SceneRegistration.h"
#include "Strata/Scene/SceneSystem.h"
#include "Strata/Scripting/ScriptingRegistration.h"

#include <atomic>

namespace Strata::Engine
{

	namespace
	{

		enum class RegistrationState : uint8_t
		{
			NotRegistered,
			Registering,
			Registered
		};

		std::atomic<RegistrationState> s_State = RegistrationState::NotRegistered;

	}

	void RegisterBuiltinModules(const ModuleRegistrationOptions& options)
	{
		const RegistrationState state = s_State.load(std::memory_order_acquire);
		ST_CORE_VERIFY(state != RegistrationState::Registering, "Engine::RegisterBuiltinModules() is called while the engine's modules are being registered");
		if (state == RegistrationState::Registered)
		{
			if (!options.Extra.empty())
				ST_CORE_ERROR("Engine::RegisterBuiltinModules() is called again with {} extra registrations, which are ignored: the modules are registered already", options.Extra.size());
			return;
		}
		s_State.store(RegistrationState::Registering, std::memory_order_release);

		ComponentRegistry::BeginRegistration();
		AssetLoaderRegistry::BeginRegistration();
		AssetImporterRegistry::BeginRegistration();
		BuiltinAssets::BeginRegistration();
		SceneSystemRegistry::BeginRegistration();

		// Lower layers first. Scripting registers before Physics and Physics before Audio: that is their update order.
		RegisterSceneModule();
		RegisterRendererModule();
		RegisterScriptingModule();
		RegisterPhysicsModule();
		RegisterAudioModule();
		if (options.AssetPipeline)
			RegisterAssetPipeline();

		for (const std::function<void()>& registration : options.Extra)
		{
			if (registration)
				registration();
		}

		ComponentRegistry::Freeze();
		s_State.store(RegistrationState::Registered, std::memory_order_release);
	}

	bool AreBuiltinModulesRegistered()
	{
		return s_State.load(std::memory_order_acquire) == RegistrationState::Registered;
	}

}
