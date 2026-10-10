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
		// Clamped before the conversion: a tiny fraction makes the ratio infinite, which no integer holds.
		const float levels = std::floor(std::log2(lodThreshold / screenFraction));
		const float lod = 1.0f + std::min(levels, static_cast<float>(lodCount));
		return std::min(static_cast<uint32_t>(lod), lodCount - 1);
	}

}
