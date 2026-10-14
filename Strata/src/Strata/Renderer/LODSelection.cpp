#include "stpch.h"
#include "Strata/Renderer/LODSelection.h"

#include <algorithm>
#include <cmath>

namespace Strata::LODSelection
{

	float GetScreenFraction(const SceneCamera& camera, const glm::vec3& center, float radius)
	{
		if (camera.Orthographic)
			return (2.0f * radius) / std::max(camera.VerticalFOV, 1e-4f);
		const float halfFOVTangent = std::tan(std::max(camera.VerticalFOV, 1e-3f) * 0.5f);
		const float distance = glm::length(center - camera.Position);
		return radius / (std::max(distance, camera.Near) * halfFOVTangent);
	}

	uint32_t SelectLOD(float screenFraction, float lodThreshold, uint32_t lodCount)
	{
		if (lodCount <= 1 || !(screenFraction > 0.0f && screenFraction < lodThreshold))
			return 0;
		// Level 1 below the threshold, and one more for each doubling of the fraction that stays below it. Doubling a float is
		// exact, so these comparisons give the GPU's culling (Include/LODSelection.glsl) the same level for the same fraction,
		// which a logarithm would not.
		uint32_t lod = 1;
		float doubled = screenFraction * 2.0f;
		while (lod + 1 < lodCount && doubled < lodThreshold)
		{
			lod++;
			doubled *= 2.0f;
		}
		return lod;
	}

}
