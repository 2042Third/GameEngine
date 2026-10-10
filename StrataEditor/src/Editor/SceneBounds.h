#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Math/AABB.h>
#include <Strata/Scene/Entity.h>

#include <span>
#include <vector>

namespace Strata
{

	class Scene;

	// World-space bounds of scene content for framing the editor camera. Bounds cover what renders: mesh renderers and
	// world-space text. Content that renders nothing (cameras, lights, empty entities, screen-space text) only counts
	// when nothing in the measured set renders, as a placeholder box around each entity's origin, so a selected light
	// can still be framed.
	namespace SceneBounds
	{

		// Half extents of the box (in the entity's local space) that stands for content without known bounds: entities
		// that render nothing, and meshes that are still loading.
		constexpr float c_PlaceholderExtent = 0.5f;

		// Bounds of an entity's content (with includeDescendants, united with its descendants'): its renderable content,
		// or the placeholder box when none of them renders anything. Mesh bounds come from loaded meshes (a mesh that is
		// still loading stands as the placeholder box). Updates the scene's world transforms. Invalid for an invalid
		// entity.
		AABB GetEntityBounds(Scene& scene, Entity entity, bool includeDescendants = true);
		// Bounds of several entities, each with its descendants, by the same rule.
		AABB GetEntitiesBounds(Scene& scene, std::span<const Entity> entities);
		// Bounds of the renderable content of every entity that is active in the hierarchy, or of the placeholder boxes
		// of all of them when nothing renders; invalid for a scene without active entities. outPendingMeshes receives the
		// meshes that were still loading (their placeholder boxes stood in for them).
		AABB GetSceneBounds(Scene& scene, std::vector<AssetHandle>* outPendingMeshes = nullptr);

	}

}
