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
	class EditorContext;
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

		// The surface under the view's pivot (the camera's target, which the view orbits): what a ray straight down from just
		// above the target hits; else what a ray from the camera through the view's center hits; else the target itself.
		Surface FindPivotSurface(Scene& scene, const EditorCamera& camera, std::span<const Entity> ignore = {});
		// What a ray from the camera through a point of the view (a cursor, in normalized device coordinates: [-1, 1], +Y up)
		// hits, for the camera at `aspectRatio`.
		std::optional<SceneRayHit> CastAt(Scene& scene, const EditorCamera& camera, const glm::vec2& ndc, float aspectRatio, std::span<const Entity> ignore = {});
		// The surface under a point of the view (CastAt); else the one under the view's pivot (FindPivotSurface).
		Surface FindSurfaceAt(Scene& scene, const EditorCamera& camera, const glm::vec2& ndc, float aspectRatio, std::span<const Entity> ignore = {});

		// The translation that rests a box (an entity's bounds at its origin, unrotated and unscaled) on the surface: the box
		// touches the surface's point with its side facing the surface. Invalid bounds: the point itself.
		glm::vec3 RestOn(const Surface& surface, const AABB& bounds);
		// The bounds of a mesh at its entity's origin: the loaded mesh's, else what the asset database recorded; invalid when
		// unknown.
		AABB GetMeshBounds(AssetHandle mesh);
		// The bounds of what a prefab or model draws around its (first) root's origin, as prefab.instantiate makes it with a
		// translation for that root: its mesh renderers' bounds (GetMeshBounds) over the root's subtree, with the root's
		// rotation and scale and its descendants' transforms, skinned meshes in their skeleton's rest pose. Instantiates it
		// into a scene of its own (loading the prefab); skeletons are only requested, never waited for: a skinned mesh whose
		// skeleton is not resident yet counts in its bind pose. Invalid when it draws nothing or cannot be loaded.
		AABB GetTemplateBounds(AssetHandle prefab);
		// The bounds a placed prefab or model rests on a surface with: the editor's measurement (EditorContext::GetAssetBounds,
		// which requests what it needs and caches what it finished) once complete, else GetTemplateBounds. The viewport asks
		// for it while an asset is dragged over it, so that by the drop what it needs is resident.
		AABB GetPlacementBounds(EditorContext& context, AssetHandle prefab);

	}

}
