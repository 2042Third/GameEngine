#pragma once

#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

// How SceneRenderer chooses a mesh's level of detail, as pure functions, so tools show the level the renderer draws
// (e.g. the editor's LOD view) without rendering.
namespace Strata::LODSelection
{

	// The projected diameter of a bounding sphere as a fraction of the viewport height: perspective cameras divide by the
	// distance (at least the near plane) and the field of view, orthographic ones by the view's height.
	float GetScreenFraction(const SceneCamera& camera, const glm::vec3& center, float radius);

	// The level of detail drawn for a screen fraction (SceneRendererSpecification::LODThreshold): 0 at or above the
	// threshold, else 1 plus the number of times the fraction can be doubled while staying below the threshold, at most
	// lodCount - 1 (0 for a mesh without levels, or a fraction that is not positive). Only comparisons and exact doublings: the
	// GPU's culling (Include/LODSelection.glsl) chooses the same level for the same fraction. Submeshes whose levels have no
	// errors (MeshUtils::HasLODErrors) choose this way.
	uint32_t SelectLOD(float screenFraction, float lodThreshold, uint32_t lodCount);

	// The screen fraction times relative error (MeshUtils::GetRelativeLODError) a level may reach and still be drawn: an error
	// of errorPixels (SceneRendererSpecification::LODErrorPixels) in a viewport this many pixels high, as the fraction is of
	// the viewport height and the relative error of the bounds' radius (half the diameter the fraction measures). 0 (full
	// detail always) when either is not positive.
	float GetErrorThreshold(float errorPixels, float viewportHeight);
	// The pixels of the viewport height a level's error spans at a screen fraction.
	float GetErrorPixels(float screenFraction, float relativeError, float viewportHeight);

	// The coarsest level whose projected error stays within the threshold: the last level, scanning down to 1, whose
	// relative error (relativeErrors[level]; entry 0, full detail, is not read) times the screen fraction is at most
	// errorThreshold, else 0. 0 for a fraction that is not positive or not finite. One multiplication and a comparison per
	// level: the GPU's culling (Include/LODSelection.glsl) chooses the same level for the same fraction and errors.
	uint32_t SelectLODByError(float screenFraction, std::span<const float> relativeErrors, float errorThreshold);

	// The level a renderer draws of a submesh of a mesh with these bounds (Mesh::GetBounds): by its levels' errors where
	// they are known (MeshUtils::HasLODErrors), else by halving the screen fraction.
	uint32_t SelectLOD(const Submesh& submesh, const AABB& meshBounds, float screenFraction, const LODCriteria& criteria);

}
