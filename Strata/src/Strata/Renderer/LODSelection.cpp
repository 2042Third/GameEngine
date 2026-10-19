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

	namespace
	{

		constexpr uint32_t c_FadeFrameBits = 19;
		constexpr uint32_t c_FadeFrameMask = (1u << c_FadeFrameBits) - 1;
		constexpr uint32_t c_MaxFadedLOD = 15; // Levels kept in four bits

	}

	LODFade UpdateLODFade(LODFadeState state, uint32_t target, uint32_t lodCount, uint32_t entityID, uint32_t frame, bool enabled)
	{
		constexpr uint32_t c_Frames = RenderData::c_LODFadeFrames;
		LODFade result;
		result.From = target;
		if (!enabled || target > c_MaxFadedLOD)
			return result; // No history kept: drawn at once, now and when fades start again
		uint32_t lod = state.Packed & 15u;
		uint32_t from = (state.Packed >> 4) & 15u;
		uint32_t step = (state.Packed >> 8) & 31u;
		const uint32_t last = state.Packed >> 13;
		const bool continuing = state.Packed != 0 && state.EntityID == entityID && last == ((frame - 1u) & c_FadeFrameMask) && lod < lodCount && from < lodCount
			&& step >= 1 && step <= c_Frames;
		if (!continuing)
		{
			lod = target;
			from = target;
			step = c_Frames;
		}
		else if (target != lod)
		{
			if (step < c_Frames && target == from)
			{
				// Back to the level fading out: each keeps the pixels it has.
				from = lod;
				step = c_Frames - step;
			}
			else
			{
				// From the level on more pixels now.
				from = step < c_Frames && 2 * step < c_Frames ? from : lod;
				step = 1;
			}
			lod = target;
		}
		else if (step < c_Frames)
		{
			step++;
		}
		if (from == lod)
			step = c_Frames;
		result.State.Packed = lod | (from << 4) | (step << 8) | ((frame & c_FadeFrameMask) << 13);
		result.State.EntityID = entityID;
		result.Code = step < c_Frames ? step : 0u;
		result.From = from;
		return result;
	}

	uint32_t GetLODFadeThreshold(uint32_t x, uint32_t y)
	{
		constexpr uint32_t c_Bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
		return c_Bayer[(y & 3u) * 4u + (x & 3u)];
	}

	bool IsLODFadeDrawn(uint32_t code, uint32_t x, uint32_t y)
	{
		if (code == 0)
			return true;
		const uint32_t threshold = GetLODFadeThreshold(x, y);
		return code < RenderData::c_LODFadeOutgoing ? threshold < code : threshold >= code - RenderData::c_LODFadeOutgoing;
	}

}
