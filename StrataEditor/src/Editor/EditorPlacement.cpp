#include "Editor/EditorPlacement.h"

#include "Editor/EditorCamera.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Renderer/Mesh.h>

#include <algorithm>
#include <cmath>

namespace Strata
{

	namespace
	{

		// How far above the camera's target the downward ray starts (and how far below it it ends): content under the target
		// within this height is found.
		constexpr float c_TargetSearchHeight = 1000.0f;

		EditorPlacement::Surface ToSurface(const SceneRayHit& hit)
		{
			return EditorPlacement::Surface { hit.Point, hit.Normal, true };
		}

		// Under the camera's target, else the target.
		EditorPlacement::Surface FindUnderTarget(Scene& scene, const EditorCamera& camera, std::span<const Entity> ignore)
		{
			const glm::vec3 target = camera.GetTarget();
			const Ray down(target + glm::vec3(0.0f, c_TargetSearchHeight, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
			if (const std::optional<SceneRayHit> hit = SceneRaycast::Cast(scene, down, c_TargetSearchHeight * 2.0f, ignore))
				return ToSurface(*hit);
			return EditorPlacement::Surface { target, glm::vec3(0.0f, 1.0f, 0.0f), false };
		}

	}

	namespace EditorPlacement
	{

		Surface FindViewSurface(Scene& scene, const EditorCamera& camera, std::span<const Entity> ignore)
		{
			const Ray view(camera.GetPosition(), camera.GetForward());
			if (const std::optional<SceneRayHit> hit = SceneRaycast::Cast(scene, view, camera.GetFar(), ignore))
				return ToSurface(*hit);
			return FindUnderTarget(scene, camera, ignore);
		}

		std::optional<SceneRayHit> CastAt(Scene& scene, const EditorCamera& camera, const glm::vec2& ndc, float aspectRatio, std::span<const Entity> ignore)
		{
			const glm::mat4 viewProjection = camera.GetProjectionMatrix(aspectRatio) * camera.GetViewMatrix();
			return SceneRaycast::Cast(scene, Ray::FromNDC(ndc, glm::inverse(viewProjection)), camera.GetFar(), ignore);
		}

		Surface FindSurfaceAt(Scene& scene, const EditorCamera& camera, const glm::vec2& ndc, float aspectRatio, std::span<const Entity> ignore)
		{
			if (const std::optional<SceneRayHit> hit = CastAt(scene, camera, ndc, aspectRatio, ignore))
				return ToSurface(*hit);
			return FindUnderTarget(scene, camera, ignore);
		}

		glm::vec3 RestOn(const Surface& surface, const AABB& bounds)
		{
			if (!bounds.IsValid())
				return surface.Point;
			// The box's support along the normal: how far its farthest point against the normal lies behind its origin.
			const glm::vec3 normal = glm::length(surface.Normal) > 0.0f ? glm::normalize(surface.Normal) : glm::vec3(0.0f, 1.0f, 0.0f);
			const glm::vec3 center = bounds.GetCenter();
			const glm::vec3 extents = bounds.GetExtents();
			const float behind = glm::dot(glm::abs(normal), extents) - glm::dot(normal, center);
			return surface.Point + normal * behind;
		}

		AABB GetMeshBounds(AssetHandle mesh)
		{
			if (!AssetManager::HasActive())
				return AABB();
			if (const Ref<Mesh> loaded = AssetManager::GetAsset<Mesh>(mesh); loaded && loaded->GetBounds().IsValid())
				return loaded->GetBounds();
			if (const std::optional<AABB> recorded = AssetManager::GetActive()->GetBounds(mesh))
				return *recorded;
			return AABB();
		}

	}

}
