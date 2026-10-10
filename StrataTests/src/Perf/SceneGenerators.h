#pragma once

#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// Generated scenes for the perf tests (the shapes of the scaling measurements: wide, deep, scripted), built through the
// public Scene API, so that they hold exactly what editing or loading a scene creates.
namespace Strata::Tests::Perf
{

	// Children per entity of the generated nested hierarchies (a million entities reach six levels below the root).
	constexpr uint32_t c_NestedFanOut = 10;
	// The API test module's class with a trivial OnUpdate (it counts its calls); StrataTestScriptsAPI must be loaded.
	constexpr std::string_view c_TrivialScriptClass = "HiddenCallbacks";

	// `count` empty root entities ("Root"), each a little to the right of the one before, in hierarchy order.
	std::vector<Entity> CreateFlatRoots(Scene& scene, size_t count);

	struct NestedHierarchy
	{
		Entity Root;
		// Breadth first: the root, its children, their children...; the last one is a leaf of the deepest level.
		std::vector<Entity> Entities;
	};
	// A tree of `count` empty entities (the root included) under one root: each entity gets `fanOut` children ("Node", each
	// offset from its parent) before the next level starts, so every level but the deepest is full.
	NestedHierarchy CreateNestedHierarchy(Scene& scene, size_t count, uint32_t fanOut = c_NestedFanOut);

	// `count` root entities ("Scripted") whose Script component runs `className`, by default the trivial class of the API test
	// module. The script instances are created when the scene starts playing.
	std::vector<Entity> CreateScriptedEntities(Scene& scene, size_t count, std::string_view className = c_TrivialScriptClass);

	// A primary camera looking toward the origin and a directional light, as a scene opened in the editor has.
	void AddCameraAndLight(Scene& scene);

	// The job system as the editor and games start it (a worker per hardware thread but one), for the lifetime of this
	// object: large scene updates spread over it, so measurements of generated scenes run with it.
	class ScopedApplicationJobSystem
	{
	public:
		ScopedApplicationJobSystem();
		~ScopedApplicationJobSystem();

		ScopedApplicationJobSystem(const ScopedApplicationJobSystem&) = delete;
		ScopedApplicationJobSystem& operator=(const ScopedApplicationJobSystem&) = delete;
	};

}
