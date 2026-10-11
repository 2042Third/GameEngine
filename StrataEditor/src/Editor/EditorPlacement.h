#pragma once

#include "Editor/SceneRaycast.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Math/AABB.h>

#include <glm/glm.hpp>

#include <optional>
#include <span>

namespace Strata
{

	class EditorCamera;
	class Scene;

	// Where the editor puts new content: on the surface the view looks at (SceneRaycast), resting on it rather than buried
	// at the origin. The UI computes a translation here and sends it with the creating command (entity.create,
	// prefab.instantiate), so automation places content the same way by passing the translation itself.
	namespace EditorPlacement
	{

		struct Surface
		{
			glm::vec3 Point = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
			bool Hit = false; // A surface of the scene was found (false: Point is the camera's target)
		};

		// The surface the view centers on: what a ray from the camera through the view's center hits; else what lies under
		// the camera's target (a ray straight down from above it); else the target itself.
		Surface FindViewSurface(Scene& scene, const EditorCamera& camera, std::span<const Entity> ignore = {});
		// What a ray from the camera through a point of the view (a cursor, in normalized device coordinates: [-1, 1], +Y up)
		// hits, for the camera at `aspectRatio`.
		std::optional<SceneRayHit> CastAt(Scene& scene, const EditorCamera& camera, const glm::vec2& ndc, float aspectRatio, std::span<const Entity> ignore = {});
		// The surface under a point of the view (CastAt); else what lies under the camera's target, else the target.
		Surface FindSurfaceAt(Scene& scene, const EditorCamera& camera, const glm::vec2& ndc, float aspectRatio, std::span<const Entity> ignore = {});

		// The translation that rests a box (an entity's bounds at its origin, unrotated and unscaled) on the surface: the box
		// touches the surface's point with its side facing the surface. Invalid bounds: the point itself.
		glm::vec3 RestOn(const Surface& surface, const AABB& bounds);
		// The bounds of a mesh at its entity's origin: the loaded mesh's, else what the asset database recorded; invalid when
		// unknown.
		AABB GetMeshBounds(AssetHandle mesh);

	}

}
