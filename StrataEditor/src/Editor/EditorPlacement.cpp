#include "Editor/EditorPlacement.h"

#include "Editor/AssetBounds.h"
#include "Editor/EditorCamera.h"
#include "Editor/EditorContext.h"
#include "Editor/SceneBounds.h"

#include <Strata/Animation/AnimationComponents.h>
#include <Strata/Animation/AnimationPreview.h>
#include <Strata/Asset/AssetManager.h>
#include <Strata/Asset/EditorAssetManager.h>
#include <Strata/Renderer/Mesh.h>
#include <Strata/Renderer/SceneRenderer.h>
#include <Strata/Scene/Components.h>
#include <Strata/Scene/Prefab.h>
#include <Strata/Scene/Scene.h>

#include <glm/gtc/matrix_transform.hpp>

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
					// A skinned mesh in its animator's pose (the rest pose GetTemplateBounds gives the scene), as it is drawn.
					const Ref<Mesh> loaded = current.HasComponent<SkinComponent>() ? AssetManager::GetAsset<Mesh>(renderer->Mesh) : nullptr;
					const AABB posed = loaded ? SceneBounds::GetPosedMeshBounds(scene, current, *loaded) : AABB();
					const AABB mesh = posed.IsValid() ? posed : EditorPlacement::GetMeshBounds(renderer->Mesh);
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

		const char* PlacementSourceToString(PlacementSource source)
		{
			switch (source)
			{
				case PlacementSource::Surface: return "surface";
				case PlacementSource::Grid: return "grid";
				case PlacementSource::InFront: return "inFront";
				case PlacementSource::Point: return "point";
			}
			return "surface";
		}

		Ray MakeViewRay(const SceneCamera& camera, const glm::vec2& ndc)
		{
			// Through the rotation-only view: the ray's origin stays near zero, so the camera's position is added exactly once
			// instead of being lost in the inverse of a matrix that holds it.
			const glm::mat4 rotation(glm::mat3(camera.View));
			const Ray relative = Ray::FromNDC(ndc, glm::inverse(camera.Projection * rotation));
			return Ray(camera.Position + relative.Origin, relative.Direction);
		}

		float GetFallbackDistance(const EditorCamera* editorCamera)
		{
			const float distance = editorCamera ? editorCamera->GetDistance() : c_DefaultFallbackDistance;
			if (!std::isfinite(distance))
				return c_DefaultFallbackDistance;
			return std::min(std::max(distance, c_MinFallbackDistance), c_GridFallbackDistance);
		}

		PlacementRequest MakeRayRequest(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, float fallbackDistance,
			std::optional<float> snap)
		{
			PlacementRequest request;
			request.CastRay = Ray(origin, direction);
			request.MaxDistance = maxDistance;
			request.FallbackDistance = std::min(std::max(fallbackDistance, c_MinFallbackDistance), c_GridFallbackDistance);
			request.Snap = snap;
			return request;
		}

		std::optional<Placement> Place(Scene& scene, const PlacementRequest& request, const AABB& bounds)
		{
			Placement placement;
			if (bounds.IsValid())
				placement.PlacedBounds = bounds.Transformed(glm::mat4_cast(request.Rotation) * glm::scale(glm::mat4(1.0f), glm::vec3(request.Scale)));

			// Where: the given point, else what the ray hits, else the grid plane, else in front of the camera.
			const Ray& ray = request.CastRay;
			if (request.At)
			{
				placement.Source = PlacementSource::Point;
				placement.RestingOn = *request.At;
				const float length = glm::length(placement.RestingOn.Normal);
				placement.RestingOn.Normal = length > 0.0f && std::isfinite(length) ? placement.RestingOn.Normal / length : glm::vec3(0.0f, 1.0f, 0.0f);
			}
			else if (const std::optional<SceneRayHit> hit = SceneRaycast::Cast(scene, ray, request.MaxDistance))
			{
				placement.Source = PlacementSource::Surface;
				placement.RestingOn = ToSurface(*hit);
				placement.HitEntity = hit->HitEntity;
			}
			else
			{
				if (request.RequireSurface)
					return std::nullopt;
				const float reach = std::min(request.MaxDistance, c_GridFallbackDistance);
				const float gridDistance = std::abs(ray.Direction.y) > 1.0e-6f ? -ray.Origin.y / ray.Direction.y : -1.0f;
				if (gridDistance > 0.0f && gridDistance <= reach)
				{
					placement.Source = PlacementSource::Grid;
					placement.RestingOn = Surface { ray.GetPoint(gridDistance), glm::vec3(0.0f, ray.Direction.y < 0.0f ? 1.0f : -1.0f, 0.0f), false };
				}
				else
				{
					// Never std::clamp with bounds that may cross: a reach under the minimum distance wins.
					const float distance = std::min(std::max(request.FallbackDistance, c_MinFallbackDistance), reach);
					placement.Source = PlacementSource::InFront;
					placement.RestingOn = Surface { ray.GetPoint(distance), glm::vec3(0.0f, 1.0f, 0.0f), false };
				}
			}

			// Snapping: along the surface (the normal's dominant axis keeps its value), as the gizmo snaps translations.
			if (request.Snap && *request.Snap > 0.0f)
			{
				const float step = *request.Snap;
				const glm::vec3 normal = placement.RestingOn.Normal;
				const glm::vec3 magnitude = glm::abs(normal);
				int axis = 1;
				if (placement.Source == PlacementSource::Surface || placement.Source == PlacementSource::Point)
					axis = magnitude.x > magnitude.y && magnitude.x > magnitude.z ? 0 : (magnitude.y >= magnitude.z ? 1 : 2);
				glm::vec3 snapped = placement.RestingOn.Point;
				for (int i = 0; i < 3; i++)
				{
					if (i != axis)
						snapped[i] = std::round(snapped[i] / step) * step;
				}
				placement.RestingOn.Point = snapped;
				// On ground facing up, the height under the snapped point (still at the hit's height when nothing is there).
				if (placement.Source == PlacementSource::Surface && axis == 1 && normal.y > 0.0f)
				{
					const Ray down(snapped + glm::vec3(0.0f, c_SnapLift, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
					if (const std::optional<SceneRayHit> under = SceneRaycast::Cast(scene, down, c_SnapLift + c_SnapDepth))
					{
						placement.RestingOn = ToSurface(*under);
						placement.HitEntity = under->HitEntity;
					}
				}
				placement.Snapped = true;
			}

			// InFront has nothing to rest on: the box straddles the point, so the ghost surrounds the pointer.
			if (placement.Source == PlacementSource::InFront)
				placement.Translation = placement.PlacedBounds.IsValid() ? placement.RestingOn.Point - placement.PlacedBounds.GetCenter() : placement.RestingOn.Point;
			else
				placement.Translation = RestOn(placement.RestingOn, placement.PlacedBounds);
			return placement;
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
			// Made as prefab.instantiate makes it, in a scene of its own: its transforms as they will be, around the origin, and
			// its skinned meshes in their skeleton's rest pose, as the edited scene shows them.
			Scene scene;
			const std::vector<Entity> roots = asset->Instantiate(scene);
			if (roots.empty())
				return AABB();
			// The preview requests the animators' skeletons (never waiting for them: a frame is never held on an asset).
			AnimationPreviewSpecification specification;
			specification.RestPoses = true;
			AnimationPreview poses(scene, specification);
			poses.Update();
			scene.UpdateWorldTransforms();
			AABB bounds;
			AddSubtreeBounds(scene, roots.front(), bounds);
			if (!bounds.IsValid())
				return bounds;
			// Around the root's origin: the translation the root is given replaces its own.
			const glm::vec3 origin(scene.GetWorldTransform(roots.front())[3]);
			return AABB(bounds.Min - origin, bounds.Max - origin);
		}

		AABB GetPlacementBounds(EditorContext& context, AssetHandle prefab)
		{
			if (EditorAssetManager* assets = context.GetAssetManager())
			{
				const AssetBounds::Result measured = context.GetAssetBounds().Measure(*assets, prefab);
				if (!measured.Pending)
					return measured.Bounds;
			}
			return GetTemplateBounds(prefab);
		}

	}

}
