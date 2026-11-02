#pragma once

#include "Editor/SceneRaycast.h"

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Math/AABB.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace Strata
{

	class EditorCamera;
	class EditorContext;
	class Scene;
	struct SceneCamera;

	// Where the editor puts new content: on the surface the view looks at (SceneRaycast), resting on it rather than buried
	// at the origin. Assets dragged into the Scene view and asset.place land where Place says, for one request built the
	// same way by both (MakeRayRequest), so the drop lands where its ghost stood. The create menus and Add to Scene compute
	// a translation over the view's pivot (FindPivotSurface, RestOn) and send it with the creating command.
	namespace EditorPlacement
	{

		struct Surface
		{
			glm::vec3 Point = glm::vec3(0.0f);
			glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
			bool Hit = false; // A surface of the scene was found (false: Point is the camera's target)
		};

		// Where an asset placed along a ray landed (Place).
		enum class PlacementSource : uint8_t
		{
			Surface = 0, // A surface of the scene the ray hit (SceneRaycast::Cast): a mesh renderer's triangles, or its bounds while it loads
			Grid,        // Nothing hit: where the ray meets the grid plane y = 0, within c_GridFallbackDistance of its origin
			InFront,     // Neither: on the ray at the request's fallback distance
			Point        // The point the request gave (asset.place {point})
		};
		// "surface", "grid", "inFront", "point".
		const char* PlacementSourceToString(PlacementSource source);

		// How far along a ray that hits nothing the grid plane still counts (the visible grid fades well before).
		constexpr float c_GridFallbackDistance = 500.0f;
		// The in-front distance through a scene's camera, and the lower bound of any in-front distance.
		constexpr float c_DefaultFallbackDistance = 10.0f;
		constexpr float c_MinFallbackDistance = 0.5f;
		// Snapping finds the height at the snapped point (still at the hit's height) with a ray straight down from this far
		// above it, this deep.
		constexpr float c_SnapLift = 1.0f;
		constexpr float c_SnapDepth = 1000.0f;

		struct PlacementRequest
		{
			// The point form (asset.place {point, normal}): the asset rests on it, no ray is cast. Otherwise CastRay is used.
			std::optional<Surface> At;
			Ray CastRay;
			float MaxDistance = std::numeric_limits<float>::max(); // How far the ray looks for a surface
			float FallbackDistance = c_DefaultFallbackDistance;    // InFront's distance (clamped to [c_MinFallbackDistance, c_GridFallbackDistance])
			bool RequireSurface = false;                           // No Grid or InFront: a miss places nothing
			std::optional<float> Snap;                             // The grid step in meters while snapping
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // Over the asset's own (world space)
			float Scale = 1.0f;                                    // Uniform, over the asset's own
		};

		struct Placement
		{
			PlacementSource Source = PlacementSource::Surface;
			Surface RestingOn;                       // The point and normal it rests on (InFront: the point on the ray, normal up)
			Entity HitEntity;                        // The entity hit (Surface only)
			AABB PlacedBounds;                       // The asset's bounds around its origin, rotated and scaled as placed (invalid: unknown)
			glm::vec3 Translation = glm::vec3(0.0f); // Of its root, in world space
			bool Snapped = false;
		};

		// A ray from a view's camera through a point of its image in normalized device coordinates ([-1, 1], +Y up), computed
		// camera-relative (the rotation-only view matrix, then the camera's position added) so it stays exact far from the
		// origin.
		Ray MakeViewRay(const SceneCamera& camera, const glm::vec2& ndc);
		// The in-front distance of a view: the editor camera's distance to its target (null: a scene's camera,
		// c_DefaultFallbackDistance), clamped to [c_MinFallbackDistance, c_GridFallbackDistance].
		float GetFallbackDistance(const EditorCamera* editorCamera);
		// The request of a ray placement (asset.place {ray} and the drag's preview alike): Ray(origin, direction), which
		// normalizes the direction, MaxDistance, FallbackDistance clamped as Place clamps it, and Snap. The preview passes its
		// pointer ray's stored Origin and Direction, the command the same floats read back from its parameters (float to JSON
		// double to float is exact), so both build bit-identical requests and the drop lands exactly where the ghost stood.
		PlacementRequest MakeRayRequest(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, float fallbackDistance,
			std::optional<float> snap);
		// Where an asset with `bounds` (around its origin, unrotated: AssetBounds' result) lands for the request; nullopt only
		// when RequireSurface and the ray hits nothing. Main thread (it casts through SceneRaycast).
		std::optional<Placement> Place(Scene& scene, const PlacementRequest& request, const AABB& bounds);

		// The surface under the view's pivot (the camera's target, which the view orbits): what a ray straight down from just
		// above the target hits; else what a ray from the camera through the view's center hits; else the target itself.
		Surface FindPivotSurface(Scene& scene, const EditorCamera& camera, std::span<const Entity> ignore = {});

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
		// The bounds a prefab or model added through the create menus rests on a surface with: the editor's measurement
		// (EditorContext::GetAssetBounds, which requests what it needs and caches what it finished) once complete, else
		// GetTemplateBounds.
		AABB GetPlacementBounds(EditorContext& context, AssetHandle prefab);

	}

}
