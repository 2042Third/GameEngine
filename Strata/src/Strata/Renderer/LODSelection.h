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
	// threshold, then one level more each time the fraction halves, at most lodCount - 1 (0 for a mesh without levels).
	uint32_t SelectLOD(float screenFraction, float lodThreshold, uint32_t lodCount);

}
