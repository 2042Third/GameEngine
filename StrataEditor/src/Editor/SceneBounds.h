#pragma once

#include <Strata/Math/AABB.h>
#include <Strata/Scene/Entity.h>

#include <span>

namespace Strata
{

	class Scene;

	// World-space bounds of scene content for framing the editor camera.
	namespace SceneBounds
	{

		// Half extents of the box (in the entity's local space) that stands for content without known bounds: lights,
		// cameras, empty entities and meshes that are still loading.
		constexpr float c_PlaceholderExtent = 0.5f;

		// Bounds of an entity's own content: its mesh's bounds when the mesh is loaded, otherwise the placeholder box
		// around its origin. With includeDescendants, united with every descendant's bounds. Updates the scene's world
		// transforms. Invalid for an invalid entity.
		AABB GetEntityBounds(Scene& scene, Entity entity, bool includeDescendants = true);
		// Bounds of several entities, each with its descendants.
		AABB GetEntitiesBounds(Scene& scene, std::span<const Entity> entities);
		// Bounds of every entity that is active in the hierarchy; invalid for a scene without such entities.
		AABB GetSceneBounds(Scene& scene);

	}

}
