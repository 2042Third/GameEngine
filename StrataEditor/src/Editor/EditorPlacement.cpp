#include "Editor/EditorPlacement.h"

#include "Editor/EditorCamera.h"

#include <Strata/Asset/AssetManager.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/Scene.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Strata
{

	namespace
	{

		// How far above the camera's target the downward ray starts: just above, so that what is over the target (a roof, a
		// branch) is not taken for the surface under it, while a surface the target lies on is found.
		constexpr float c_PivotLift = 0.01f;
		// How far below the camera's target the downward ray looks.
		constexpr float c_TargetSearchDepth = 1000.0f;

		EditorPlacement::Surface ToSurface(const SceneRayHit& hit)
		{
			return EditorPlacement::Surface { hit.Point, hit.Normal, true };
		}

		// What lies under the camera's target.
		std::optional<SceneRayHit> CastUnderTarget(Scene& scene, const EditorCamera& camera, std::span<const Entity> ignore)
		{
			const Ray down(camera.GetTarget() + glm::vec3(0.0f, c_PivotLift, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
			return SceneRaycast::Cast(scene, down, c_PivotLift + c_TargetSearchDepth, ignore);
		}

		// Adds an entity's mesh bounds and its descendants' to `bounds`, in the scene's space.
		void AddSubtreeBounds(Scene& scene, Entity entity, AABB& bounds)
		{
			std::vector<Entity> pending = { entity };
			while (!pending.empty())
			{
				const Entity current = pending.back();
				pending.pop_back();
				if (const MeshRendererComponent* renderer = current.TryGetComponent<MeshRendererComponent>())
				{
					const AABB mesh = EditorPlacement::GetMeshBounds(renderer->Mesh);
					if (mesh.IsValid())
						bounds.Expand(mesh.Transformed(scene.GetWorldTransform(current)));
				}
				const std::vector<Entity> children = current.GetChildren();
				pending.insert(pending.end(), children.begin(), children.end());
			}
		}

	}

	namespace EditorPlacement
	{

		Surface FindPivotSurface(Scene& scene, const EditorCamera& camera, std::span<const Entity> ignore)
		{
			if (const std::optional<SceneRayHit> hit = CastUnderTarget(scene, camera, ignore))
				return ToSurface(*hit);
			const Ray view(camera.GetPosition(), camera.GetForward());
			if (const std::optional<SceneRayHit> hit = SceneRaycast::Cast(scene, view, camera.GetFar(), ignore))
				return ToSurface(*hit);
			return Surface { camera.GetTarget(), glm::vec3(0.0f, 1.0f, 0.0f), false };
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
			return FindPivotSurface(scene, camera, ignore);
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

		AABB GetTemplateBounds(AssetHandle prefab)
		{
			if (!AssetManager::HasActive())
				return AABB();
			const AssetType type = AssetManager::GetAssetType(prefab);
			if (type != AssetType::Prefab && type != AssetType::Model)
				return AABB();
			const Ref<EntityTemplate> asset = std::static_pointer_cast<EntityTemplate>(AssetManager::GetActive()->LoadAssetSync(prefab));
			if (!asset)
				return AABB();
			// Made as prefab.instantiate makes it, in a scene of its own: its transforms as they will be, around the origin.
			Scene scene;
			const std::vector<Entity> roots = asset->Instantiate(scene);
			if (roots.empty())
				return AABB();
			scene.UpdateWorldTransforms();
			AABB bounds;
			AddSubtreeBounds(scene, roots.front(), bounds);
			if (!bounds.IsValid())
				return bounds;
			// Around the root's origin: the translation the root is given replaces its own.
			const glm::vec3 origin(scene.GetWorldTransform(roots.front())[3]);
			return AABB(bounds.Min - origin, bounds.Max - origin);
		}

	}

}
