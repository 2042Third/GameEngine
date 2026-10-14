#include "stpch.h"
#include "Strata/Engine/BuiltinModules.h"

#include "Strata/Animation/AnimationRegistration.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/MeshGeometry.h"
#include "Strata/Audio/AudioRegistration.h"
#include "Strata/Core/Log.h"
#include "Strata/Physics/PhysicsRegistration.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Renderer/RendererRegistration.h"
#include "Strata/Scene/SceneRegistration.h"
#include "Strata/Scene/SceneSystem.h"
#include "Strata/Scripting/ScriptingRegistration.h"

#include <atomic>
#include <string>

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
			// E.g. a program that registers with options after constructing its Application, which registered without them.
			if (options.AssetPipeline)
				ST_CORE_ERROR("Engine::RegisterBuiltinModules() is called again with the asset pipeline, which is ignored: the modules are registered already");
			if (!options.Extra.empty())
				ST_CORE_ERROR("Engine::RegisterBuiltinModules() is called again with {} extra registrations, which are ignored: the modules are registered already", options.Extra.size());
			return;
		}
		s_State.store(RegistrationState::Registering, std::memory_order_release);

		ComponentRegistry::BeginRegistration();
		AssetLoaderRegistry::BeginRegistration();
		AssetManagerExtensionRegistry::BeginRegistration();
		BuiltinAssets::BeginRegistration();
		MeshGeometryRegistry::BeginRegistration();
		SceneSystemRegistry::BeginRegistration();

		// Lower layers first. Scene systems order themselves against other modules' systems with soft constraints
		// (SceneSystemDescriptor::AfterIfPresent, BeforeIfPresent), which apply whatever the registration order.
		RegisterSceneModule();
		RegisterRendererModule();
		RegisterScriptingModule();
		RegisterPhysicsModule();
		RegisterAudioModule();
		RegisterAnimationModule();
		// Tooling, handed in by the program (see ModuleRegistrationOptions::AssetPipeline).
		if (options.AssetPipeline)
			options.AssetPipeline();

		for (const std::function<void()>& registration : options.Extra)
		{
			if (registration)
				registration();
		}

		ComponentRegistry::Freeze();
		s_State.store(RegistrationState::Registered, std::memory_order_release);

		std::string order;
		for (const SceneSystemDescriptor& descriptor : SceneSystemRegistry::GetAll())
			order += (order.empty() ? "" : ", ") + descriptor.Name;
		std::string absent;
		for (const std::string& constraint : SceneSystemRegistry::GetAbsentSoftConstraints())
			absent += (absent.empty() ? "" : ", ") + constraint;
		ST_CORE_INFO("Scene systems update in the order {}{}", order.empty() ? std::string("(none)") : order,
			absent.empty() ? std::string() : fmt::format("; soft constraints naming absent systems: {}", absent));
	}

	bool AreBuiltinModulesRegistered()
	{
		return s_State.load(std::memory_order_acquire) == RegistrationState::Registered;
	}

}
