#include "stpch.h"
#include "Strata/Engine/BuiltinModules.h"

#include "Strata/Asset/AssetManager.h"
#include "Strata/Asset/BuiltinAssets.h"
#include "Strata/Asset/MeshGeometry.h"
#include "Strata/Core/Log.h"
#include "Strata/GameUI/GameUIRegistration.h"
#include "Strata/Reflection/ComponentRegistry.h"
#include "Strata/Renderer/RendererRegistration.h"
#include "Strata/Scene/SceneRegistration.h"
#include "Strata/Scene/SceneSystem.h"
#include "Strata/Scripting/ScriptingRegistration.h"

// The optional modules this build has (STRATA_WITH_*, Docs/DeveloperGuide.md, "Optional modules").
#if ST_WITH_PHYSICS
	#include "Strata/Physics/PhysicsRegistration.h"
#endif
#if ST_WITH_AUDIO
	#include "Strata/Audio/AudioRegistration.h"
#endif
#if ST_WITH_ANIMATION
	#include "Strata/Animation/AnimationRegistration.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <string_view>

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
		std::vector<std::string> s_RegisteredModules;

		struct BuiltinModule
		{
			std::string_view Name;
			void (*Register)();
		};

		// The engine's modules, lower layers first, those of the optional modules the build has. The order is also the order
		// of the modules' components in ComponentRegistry::GetAll (the Inspector, Add Component, component.list). Scene systems
		// order themselves against other modules' systems with soft constraints (SceneSystemDescriptor::AfterIfPresent,
		// BeforeIfPresent), which apply whatever the registration order and whichever modules are present.
		constexpr BuiltinModule c_BuiltinModules[] = {
			{ "Scene", RegisterSceneModule },
			{ "Renderer", RegisterRendererModule },
#if ST_WITH_PHYSICS
			{ "Physics", RegisterPhysicsModule },
#endif
#if ST_WITH_AUDIO
			{ "Audio", RegisterAudioModule },
#endif
#if ST_WITH_ANIMATION
			{ "Animation", RegisterAnimationModule },
#endif
			{ "Scripting", RegisterScriptingModule },
			{ "GameUI", RegisterGameUIModule },
		};

		// Every engine has these: the others reach the scene and the renderer directly.
		constexpr std::array<std::string_view, 2> c_RequiredModules = { "Scene", "Renderer" };

		bool IsBuiltinModule(std::string_view name)
		{
			return std::any_of(std::begin(c_BuiltinModules), std::end(c_BuiltinModules), [name](const BuiltinModule& module) { return module.Name == name; });
		}

		// The modules options.Exclude leaves out, after refusing the required and unknown names.
		std::vector<std::string_view> GetExcludedModules(const ModuleRegistrationOptions& options)
		{
			std::vector<std::string_view> excluded;
			for (const std::string& name : options.Exclude)
			{
				if (std::find(c_RequiredModules.begin(), c_RequiredModules.end(), name) != c_RequiredModules.end())
					ST_CORE_ERROR("Engine::RegisterBuiltinModules(): the {} module cannot be excluded (every engine has it); it is registered", name);
				else if (!IsBuiltinModule(name))
					ST_CORE_ERROR("Engine::RegisterBuiltinModules(): '{}' is no engine module of this build and cannot be excluded", name);
				else
					excluded.push_back(name);
			}
			return excluded;
		}

	}

	std::vector<std::string> GetBuiltinModuleNames()
	{
		std::vector<std::string> names;
		for (const BuiltinModule& module : c_BuiltinModules)
			names.emplace_back(module.Name);
		return names;
	}

	std::vector<std::string> GetRegisteredModules()
	{
		return s_RegisteredModules;
	}

	void RegisterBuiltinModules(const ModuleRegistrationOptions& options)
	{
		const RegistrationState state = s_State.load(std::memory_order_acquire);
		ST_CORE_VERIFY(state != RegistrationState::Registering, "Engine::RegisterBuiltinModules() is called while the engine's modules are being registered");
		if (state == RegistrationState::Registered)
		{
			// E.g. a program whose CreateApplication registered by itself before EntryPoint.h's main registers
			// launch->Registration, or a test helper that registers again in a process whose main already did.
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

		const std::vector<std::string_view> excluded = GetExcludedModules(options);
		for (const BuiltinModule& module : c_BuiltinModules)
		{
			if (std::find(excluded.begin(), excluded.end(), module.Name) != excluded.end())
				continue;
			module.Register();
			s_RegisteredModules.emplace_back(module.Name);
		}
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
		std::string modules;
		for (const std::string& name : s_RegisteredModules)
			modules += (modules.empty() ? "" : ", ") + name;
		ST_CORE_INFO("Engine modules: {}. Scene systems update in the order {}{}", modules, order.empty() ? std::string("(none)") : order,
			absent.empty() ? std::string() : fmt::format("; soft constraints naming absent systems: {}", absent));
	}

	bool AreBuiltinModulesRegistered()
	{
		return s_State.load(std::memory_order_acquire) == RegistrationState::Registered;
	}

}
