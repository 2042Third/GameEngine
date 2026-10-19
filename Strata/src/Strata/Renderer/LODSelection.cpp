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

	float GetErrorThreshold(float errorPixels, float viewportHeight)
	{
		if (!(errorPixels > 0.0f) || !(viewportHeight > 0.0f) || !std::isfinite(errorPixels) || !std::isfinite(viewportHeight))
			return 0.0f;
		return errorPixels / (viewportHeight * 0.5f);
	}

	float GetErrorPixels(float screenFraction, float relativeError, float viewportHeight)
	{
		return screenFraction * relativeError * viewportHeight * 0.5f;
	}

	namespace
	{

		// One rounding, as on the GPU: the same fraction and error compare the same there.
		bool IsErrorWithin(float screenFraction, float relativeError, float errorThreshold)
		{
			const float projected = screenFraction * relativeError;
			return projected <= errorThreshold;
		}

	}

	uint32_t SelectLODByError(float screenFraction, std::span<const float> relativeErrors, float errorThreshold)
	{
		if (relativeErrors.size() <= 1 || !(screenFraction > 0.0f) || !std::isfinite(screenFraction))
			return 0;
		for (size_t level = relativeErrors.size() - 1; level > 0; level--)
		{
			if (IsErrorWithin(screenFraction, relativeErrors[level], errorThreshold))
				return static_cast<uint32_t>(level);
		}
		return 0;
	}

	uint32_t SelectLOD(const Submesh& submesh, const AABB& meshBounds, float screenFraction, const LODCriteria& criteria)
	{
		const size_t count = submesh.LODs.size();
		if (count <= 1)
			return 0;
		if (!MeshUtils::HasLODErrors(submesh))
			return SelectLOD(screenFraction, criteria.LODThreshold, static_cast<uint32_t>(count));
		if (!(screenFraction > 0.0f) || !std::isfinite(screenFraction))
			return 0;
		for (size_t level = count - 1; level > 0; level--)
		{
			if (IsErrorWithin(screenFraction, MeshUtils::GetRelativeLODError(submesh.LODs[level].Error, meshBounds), criteria.ErrorThreshold))
				return static_cast<uint32_t>(level);
		}
		return 0;
	}

}
