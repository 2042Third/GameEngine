#pragma once

#include <Strata/Asset/AssetTypes.h>
#include <Strata/Math/AABB.h>
#include <Strata/Scene/Entity.h>

#include <span>
#include <vector>

namespace Strata
{

	class Mesh;
	class Scene;

	// World-space bounds of scene content for framing the editor camera. Bounds cover what renders: mesh renderers and
	// world-space text. Skinned meshes are measured in their animator's pose where the scene has one (its ISkinningPoses:
	// the editor's rest poses and previews, the animation system while it plays), as the renderer bounds them. Content that renders nothing (cameras, lights, empty entities, screen-space text) only counts
	// when nothing in the measured set renders, as a placeholder box around each entity's origin, so a selected light
	// can still be framed; entities and selections (not the whole scene) that render nothing but have a physical shape (a
	// character controller's capsule, colliders) are framed by that shape first.
	namespace SceneBounds
	{

		// Half extents of the box (in the entity's local space) that stands for content without known bounds: entities
		// that render nothing, and meshes that are still loading.
		constexpr float c_PlaceholderExtent = 0.5f;

		// Bounds of an entity's content (with includeDescendants, united with its descendants'): its renderable content,
		// else the shapes of character controllers and colliders among them, else the placeholder boxes. Mesh bounds come from loaded meshes (a mesh that is
		// still loading stands as the placeholder box). Updates the scene's world transforms. Invalid for an invalid
		// entity.
		AABB GetEntityBounds(Scene& scene, Entity entity, bool includeDescendants = true);
		// Bounds of several entities, each with its descendants, by the same rule.
		AABB GetEntitiesBounds(Scene& scene, std::span<const Entity> entities);
		// Bounds of the renderable content of every entity that is active in the hierarchy, or of the placeholder boxes
		// of all of them when nothing renders; invalid for a scene without active entities. outPendingMeshes receives the
		// meshes that were still loading (their placeholder boxes stood in for them) and the skeletons of skinned meshes whose
		// pose was not there yet (their bind pose stood in): measure again once none of them is loading.
		AABB GetSceneBounds(Scene& scene, std::vector<AssetHandle>* outPendingMeshes = nullptr);

		// The bounds of a skinned mesh renderer's mesh in its animator's pose, in the entity's local space: each skin joint's
		// bounds through its pose (MeshSkinBinding::JointBounds[j] transformed by animatorToMesh * pose[Joints[j]] *
		// InverseBindMatrices[j], the expression RenderScene::ComputePalette bounds proxies with). Uses the cached world
		// transforms. Invalid when it is not posed: no SkinComponent, a mesh without a skin, no animator or no pose for it
		// in the scene's ISkinningPoses. outWaitingSkeleton receives the animator's skeleton when the scene has poses but none
		// for it yet (the skeleton loads; it is requested).
		AABB GetPosedMeshBounds(const Scene& scene, Entity entity, const Mesh& mesh, AssetHandle* outWaitingSkeleton = nullptr);

	}

}
