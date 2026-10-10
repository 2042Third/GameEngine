#pragma once

#include <functional>
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
	};

	// The engine's composition root: opens the registries (components, asset loaders, built-in assets, scene systems), lets
	// every engine module register into them in dependency order, runs options.AssetPipeline and options.Extra and freezes
	// the component registry. Every program that uses the engine calls it once at startup, before anything touches a
	// registry (the Application constructor does, without options; a program that needs options calls it earlier, e.g. in
	// CreateApplication). A second call is a no-op. Main thread.
	void RegisterBuiltinModules(const ModuleRegistrationOptions& options = {});
	bool AreBuiltinModulesRegistered();

}
