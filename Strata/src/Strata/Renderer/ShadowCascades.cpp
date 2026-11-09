#include "stpch.h"
#include "Strata/Renderer/ShadowCascades.h"

#include <algorithm>
#include <cmath>

namespace Strata
{

	namespace ShadowCascades
	{

		float GetSplit(float rangeNear, float rangeFar, float distribution, uint32_t index, uint32_t count)
		{
			// A blend of logarithmic and uniform splits.
			const float fraction = static_cast<float>(index + 1) / static_cast<float>(count);
			const float logarithmic = rangeNear * std::pow(rangeFar / rangeNear, fraction);
			const float uniform = rangeNear + (rangeFar - rangeNear) * fraction;
			return distribution * logarithmic + (1.0f - distribution) * uniform;
		}

		float GetSplit(const ShadowCascadeView& view, uint32_t index)
		{
			const uint32_t count = std::clamp(view.CascadeCount, 1u, RenderData::c_MaxCascades);
			if (index + 1 >= count)
				return view.ShadowDistance;
			// From a fixed distance rather than the near clip, which would move every split with the camera's lens (a near clip of
			// 1 cm against one of 10 cm halves the first split).
			const float rangeNear = std::min(std::max(c_SplitStart, view.Near), 0.5f * view.ShadowDistance);
			const float distribution = std::isfinite(view.Distribution) ? std::clamp(view.Distribution, 0.0f, 1.0f) : 0.9f;
			const float split = GetSplit(rangeNear, view.ShadowDistance, distribution, index, count);
			return index == 0 ? std::min(split, c_MaxFirstSplit) : split;
		}

		void GetSliceSphere(const ShadowCascadeView& view, float sliceNear, float sliceFar, glm::vec3& center, float& radius)
		{
			// The slice is the hull of its eight corners, symmetric about the view axis, so its smallest sphere is centered on the
			// axis where the nearest and farthest corners are equally far, or at the far end where they cannot be (wide views):
			// its size depends on the slice and the lens alone, never on where the camera looks.
			const double a = static_cast<double>(sliceNear);
			const double b = std::max(static_cast<double>(sliceFar), a);
			double nearHalf2 = 0.0; // The squared half-diagonal of the slice's near and far ends
			double farHalf2 = 0.0;
			if (view.Orthographic)
			{
				const double halfHeight = 0.5 * static_cast<double>(view.OrthographicHeight);
				const double halfWidth = halfHeight * static_cast<double>(view.Aspect);
				nearHalf2 = halfHeight * halfHeight + halfWidth * halfWidth;
				farHalf2 = nearHalf2;
			}
			else
			{
				const double tanY = static_cast<double>(view.TanHalfFOV);
				const double reach2 = tanY * tanY * (1.0 + static_cast<double>(view.Aspect) * static_cast<double>(view.Aspect));
				nearHalf2 = reach2 * a * a;
				farHalf2 = reach2 * b * b;
			}
			double z = a;
			if (b > a)
				z = std::clamp((b * b - a * a + farHalf2 - nearHalf2) / (2.0 * (b - a)), a, b);
			const double extent = std::max(std::sqrt((z - a) * (z - a) + nearHalf2), std::sqrt((b - z) * (b - z) + farHalf2));
			center = view.CameraRotation * glm::vec3(0.0f, 0.0f, -static_cast<float>(z));
			radius = static_cast<float>(std::ceil(extent * 16.0) / 16.0);
		}

		ShadowCascadeLayout GetLayout(const ShadowCascadeView& view)
		{
			ShadowCascadeLayout layout;
			const uint32_t count = std::clamp(view.CascadeCount, 1u, RenderData::c_MaxCascades);
			// The square covers the sphere and, beyond it, the filter's reach and the texel the snapping may move it by.
			constexpr uint32_t margin = 2 * (c_FilterReachTexels + 1);
			const double usableTexels = static_cast<double>(std::max(view.MapSize, 2 * margin) - margin);
			const float blend = std::isfinite(view.CascadeBlend) ? std::clamp(view.CascadeBlend, 0.0f, 1.0f) : 0.0f;
			float sliceNear = view.Near;
			float previousSplit = 0.0f;
			for (uint32_t cascade = 0; cascade < count; cascade++)
			{
				// Each cascade blends into the next over the last CascadeBlend of its slice before its split (the first one's measured
				// from the camera, at 0: Include/Shadows.slang, CascadeBlendWeight), so the next slice starts at that band.
				const float split = std::max(GetSplit(view, cascade), sliceNear);
				ShadowCascadeSlice& slice = layout.Cascades[cascade];
				slice.SliceNear = sliceNear;
				slice.SliceFar = split;
				GetSliceSphere(view, sliceNear, split, slice.SphereCenter, slice.Radius);
				slice.WorldPerTexel = 2.0 * static_cast<double>(slice.Radius) / usableTexels;
				sliceNear = std::max(split - blend * (split - previousSplit), view.Near);
				previousSplit = split;
			}
			layout.CascadeCount = count;
			return layout;
		}

		ShadowCascadeSquare GetSquare(const ShadowCascadeSlice& slice, const glm::dvec3& cameraLight, const glm::dmat3& lightRotation, uint32_t mapSize)
		{
			// In absolute light space and double precision, so the texel grid stays put however far the camera is from the origin.
			const glm::dvec3 sphere = cameraLight + lightRotation * glm::dvec3(slice.SphereCenter);
			const double texel = slice.WorldPerTexel;
			ShadowCascadeSquare square;
			square.Center = texel > 0.0 ? glm::floor(glm::dvec2(sphere) / texel) * texel : glm::dvec2(sphere);
			square.HalfSide = 0.5 * texel * static_cast<double>(mapSize);
			square.SphereZ = sphere.z;
			return square;
		}

	}

}
