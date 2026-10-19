#pragma once

#include "Strata/Asset/AssetTypes.h"
#include "Strata/Scene/Entity.h"
#include "Strata/Scene/Scene.h"

#include <cstddef>
#include <cstdint>
#include <span>
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

#if ST_WITH_PHYSICS
	// Height of the generated box stacks (boxes per column).
	constexpr uint32_t c_BoxStackHeight = 10;

	struct BoxStacks
	{
		Entity Floor;
		std::vector<Entity> Boxes; // Column by column, each from the bottom up
	};
	// `count` dynamic 1 m boxes ("Box", root entities with a RigidBody and a BoxCollider) in a square grid of columns of
	// `height` boxes, 0.2 m apart in every direction, over a static floor ("Floor", a BoxCollider whose top is the plane
	// y = 0). Playing it, the boxes fall 0.2 m, land on each other column by column and come to rest: a burst of contacts
	// in the first second, then every box asleep in its stack.
	BoxStacks CreateBoxStacks(Scene& scene, size_t count, uint32_t height = c_BoxStackHeight);
#endif

	// A primary camera looking toward the origin and a directional light, as a scene opened in the editor has.
	void AddCameraAndLight(Scene& scene);

	// Spacing of the generated grids (CreateCubeGrid, CreatePointLightGrid): the unit cubes leave a unit of space between
	// them.
	constexpr float c_GridSpacing = 2.0f;

	// The scaling measurements of the renderer: `count` unit cubes ("Cube", the built-in cube mesh) on a square grid of
	// c_GridSpacing in the XZ plane, centered on the origin, resting on y = 0, in row order. `materials` gives cube i the
	// material materials[i % size] (empty: the default material). With a valid `parent` the cubes are its children (a
	// group selected as one), otherwise roots.
	std::vector<Entity> CreateCubeGrid(Scene& scene, size_t count, std::span<const AssetHandle> materials = {}, Entity parent = {});

	// `count` point lights ("Light") of `range` on the same kind of grid, `height` above the ground; children of a valid
	// `parent`, otherwise roots.
	std::vector<Entity> CreatePointLightGrid(Scene& scene, size_t count, float range, float height = 2.0f, Entity parent = {});

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
