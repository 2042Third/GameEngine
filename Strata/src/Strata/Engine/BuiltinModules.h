#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Strata::Engine
{

	struct ModuleRegistrationOptions
	{
		// Opens the asset importer registry and registers the importers: the editor and tools pass RegisterAssetPipeline
		// (Strata/Asset/AssetImporter.h). Shipped games read cooked packs and leave it empty: the composition root never
		// names the importers itself, so they are not linked into games, whose importer registry stays closed.
		std::function<void()> AssetPipeline;
		// Registrations of games, tools and tests, run after the engine's modules and the asset pipeline while the
		// registries are open: the place to register components (ComponentRegistry::Register), which is frozen afterwards.
		std::vector<std::function<void()>> Extra;
		// Engine modules of this build not to register, by name (GetBuiltinModuleNames): a test of what a game without them
		// does, in a full build. Their components stay unknown components, their systems and services are absent. Scene and
		// Renderer are part of every engine and are refused (an error is logged; they register anyway), as are unknown names.
		std::vector<std::string> Exclude;
	};

	// The engine modules this build contains, in registration order: Scene, Renderer, Physics, Audio, Animation, Scripting,
	// without the optional ones the build leaves out (STRATA_WITH_PHYSICS, _AUDIO, _ANIMATION).
	std::vector<std::string> GetBuiltinModuleNames();
	// The engine modules RegisterBuiltinModules registered, in order: GetBuiltinModuleNames without the excluded ones.
	// Empty before it ran. Main thread.
	std::vector<std::string> GetRegisteredModules();

	// The engine's composition root: opens the registries (components, asset loaders, built-in assets, scene systems), lets
	// every engine module register into them in dependency order, runs options.AssetPipeline and options.Extra and freezes
	// the component registry. Every program that uses the engine calls it once at startup, before anything touches a
	// registry (the Application constructor does, without options; a program that needs options calls it earlier, e.g. in
	// CreateApplication). A second call is a no-op. Main thread.
	void RegisterBuiltinModules(const ModuleRegistrationOptions& options = {});
	bool AreBuiltinModulesRegistered();

}
