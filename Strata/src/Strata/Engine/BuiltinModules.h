#pragma once

#include <functional>
#include <vector>

namespace Strata::Engine
{

	struct ModuleRegistrationOptions
	{
		// Registers the asset importers (the editor and tools). Shipped games read cooked packs and run without them; their
		// importer registry stays empty.
		bool AssetPipeline = true;
		// Registrations of games, tools and tests, run after the engine's modules while the registries are open: the place
		// to register components (ComponentRegistry::Register), which is frozen afterwards.
		std::vector<std::function<void()>> Extra;
	};

	// The engine's composition root: opens the registries (components, asset loaders and importers, built-in assets, scene
	// systems), lets every engine module register into them in dependency order, runs options.Extra and freezes the
	// component registry. Every program that uses the engine calls it once at startup, before anything touches a registry
	// (the Application constructor does; a program that needs Extra registrations calls it earlier, e.g. in
	// CreateApplication). A second call is a no-op. Main thread.
	void RegisterBuiltinModules(const ModuleRegistrationOptions& options = {});
	bool AreBuiltinModulesRegistered();

}
