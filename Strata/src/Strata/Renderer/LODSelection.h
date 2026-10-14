#pragma once

#include "Strata/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>

#include <cstdint>

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
	// GPU's culling (Include/LODSelection.glsl) chooses the same level for the same fraction.
	uint32_t SelectLOD(float screenFraction, float lodThreshold, uint32_t lodCount);

}
