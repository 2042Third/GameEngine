#include "stpch.h"
#include "Strata/Renderer/ShadowFit.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Strata
{

	namespace
	{

		constexpr double c_Sqrt2 = 1.4142135623730951;

		// Include/OrderedFloat.slang's DecodeOrderedFloat.
		double DecodeOrderedFloat(uint32_t encoded)
		{
			const uint32_t bits = (encoded & 0x80000000u) != 0u ? (encoded & 0x7FFFFFFFu) : ~encoded;
			return static_cast<double>(std::bit_cast<float>(bits));
		}

		// The bounding sphere of a slice of the view frustum, relative to the camera: its size does not change when the camera
		// turns, which keeps the texels' footprint (and so the shadows' edges) steady. Single precision, as since Strata 0.6.0.
		void GetSliceSphere(const ShadowFitView& view, float sliceNear, float sliceFar, glm::vec3& center, float& radius)
		{
			std::array<glm::vec3, 8> corners;
			size_t cornerIndex = 0;
			for (float distance : { sliceNear, sliceFar })
			{
				const float halfHeight = view.Orthographic ? view.OrthographicHeight * 0.5f : distance * view.TanHalfFOV;
				const float halfWidth = halfHeight * view.Aspect;
				for (float x : { -1.0f, 1.0f })
				{
					for (float y : { -1.0f, 1.0f })
						corners[cornerIndex++] = view.CameraRotation * glm::vec3(x * halfWidth, y * halfHeight, -distance);
				}
			}
			center = glm::vec3(0.0f);
			for (const glm::vec3& corner : corners)
				center += corner;
			center /= 8.0f;
			radius = 0.0f;
			for (const glm::vec3& corner : corners)
				radius = std::max(radius, glm::length(corner - center));
			radius = std::ceil(radius * 16.0f) / 16.0f;
		}

		// The cascades' slices: the first `slicedCount` divide [firstNear, rangeFar], any further one reaches from there to the
		// shadow distance. Each cascade blends into the next over the last CascadeBlend of its slice before its split (the first
		// one's measured from the camera, at 0: Include/Shadows.slang, CascadeBlendWeight), so the next slice starts at that band.
		void LayOutSlices(const ShadowFitView& view, ShadowFitLayout& layout, float firstNear, float rangeFar, float distribution, uint32_t slicedCount)
		{
			const uint32_t count = std::min(view.CascadeCount, RenderData::c_MaxCascades);
			float sliceNear = firstNear;
			float previousSplit = 0.0f;
			for (uint32_t cascade = 0; cascade < count; cascade++)
			{
				const float split = cascade < slicedCount ? ShadowFitTracker::GetSplit(firstNear, rangeFar, distribution, cascade, slicedCount) : view.ShadowDistance;
				ShadowFitCascade& slice = layout.Cascades[cascade];
				slice = {};
				slice.SliceNear = sliceNear;
				slice.SliceFar = split;
				GetSliceSphere(view, sliceNear, split, slice.SphereCenter, slice.Radius);
				sliceNear = std::max(split - view.CascadeBlend * (split - previousSplit), view.Near);
				previousSplit = split;
			}
			layout.CascadeCount = count;
		}

		// k of a view: the sphere of a slice [a, D] holds every point of the view nearer than D when a <= D k, k = tan^2(fovY / 2)
		// + tan^2(fovX / 2) (the squared distance to the sphere's center is convex along the view, so the frustum's end points
		// bound it). An orthographic view's is 0: a sphere holds no point nearer than its slice.
		double GetSphereReach(const ShadowFitView& view)
		{
			if (view.Orthographic)
				return 0.0;
			const double tanY = static_cast<double>(view.TanHalfFOV);
			return tanY * tanY * (1.0 + static_cast<double>(view.Aspect) * static_cast<double>(view.Aspect));
		}

		bool Differs(const ShadowFitLayout& a, const ShadowFitLayout& b)
		{
			if (a.Fitted != b.Fitted || a.CascadeCount != b.CascadeCount)
				return true;
			for (uint32_t cascade = 0; cascade < a.CascadeCount; cascade++)
			{
				const ShadowFitCascade& first = a.Cascades[cascade];
				const ShadowFitCascade& second = b.Cascades[cascade];
				if (first.SliceNear != second.SliceNear || first.SliceFar != second.SliceFar || first.Radius != second.Radius || first.Fitted != second.Fitted
					|| first.Side != second.Side)
					return true;
			}
			return false;
		}

	}

	float ShadowFitTracker::GetSplit(float rangeNear, float rangeFar, float distribution, uint32_t index, uint32_t count)
	{
		// A blend of logarithmic and uniform splits.
		const float fraction = static_cast<float>(index + 1) / static_cast<float>(count);
		const float logarithmic = rangeNear * std::pow(rangeFar / rangeNear, fraction);
		const float uniform = rangeNear + (rangeFar - rangeNear) * fraction;
		return distribution * logarithmic + (1.0f - distribution) * uniform;
	}

	int32_t ShadowFitTracker::QuartersBelow(double value)
	{
		// The tolerance keeps a value that is a quarter stop already on it.
		return static_cast<int32_t>(std::floor(4.0 * std::log2(value) + 1e-9));
	}

	int32_t ShadowFitTracker::QuartersAbove(double value)
	{
		return static_cast<int32_t>(std::ceil(4.0 * std::log2(value) - 1e-9));
	}

	double ShadowFitTracker::FromQuarters(int32_t quarters)
	{
		return std::exp2(static_cast<double>(quarters) * 0.25);
	}

	ShadowFitLayout ShadowFitTracker::GetUnfittedLayout(const ShadowFitView& view)
	{
		ShadowFitLayout layout;
		LayOutSlices(view, layout, view.Near, view.ShadowDistance, c_UnfittedDistribution, view.CascadeCount);
		layout.Near = view.Near;
		layout.Far = view.ShadowDistance;
		return layout;
	}

	void ShadowFitTracker::Reset()
	{
		m_Epoch++;
		m_LayoutVersion++;
		m_Settled = false;
		m_HasReadback = false;
		m_Fitted = false;
		m_BinCount = 0;
		m_NearState = {};
		m_FarState = {};
		m_SideStates = {};
	}

	void ShadowFitTracker::Follow(int32_t target, int32_t& current, Hysteresis& state, bool newReadback, bool larger)
	{
		const bool grows = larger ? target > current : target < current;
		if (grows)
		{
			current = target;
			state = {};
			return;
		}
		if (!newReadback)
			return; // Shrinking is counted in readbacks, not frames
		if (target == current)
		{
			state = {};
			return;
		}
		state.Wanted = state.Count == 0 ? target : (larger ? std::max(state.Wanted, target) : std::min(state.Wanted, target));
		if (++state.Count >= c_ShrinkReadbacks)
		{
			current = state.Wanted;
			state = {};
		}
	}

	double ShadowFitTracker::GetFittedNearest(const ShadowFitView& view) const
	{
		const RenderData::ShadowFitResult& result = m_Readback.Result;
		if (result.SurfacePixels == 0)
			return 0.0;
		const double nearest = DecodeOrderedFloat(~result.Nearest);
		// The bins in order of distance: the first one that takes the count past the outliers' share holds the nearest receiver
		// the fit keeps (the nearest one overall when the first bin alone does).
		const uint64_t outliers = static_cast<uint64_t>(c_NearOutlierShare * static_cast<double>(result.SurfacePixels));
		uint64_t counted = 0;
		double kept = nearest;
		for (const RenderData::ShadowFitBin& bin : result.Bins)
		{
			if (bin.MaxD == 0)
				continue;
			counted += bin.Pixels;
			if (counted > outliers)
			{
				kept = DecodeOrderedFloat(~bin.MinD);
				break;
			}
		}
		if (!(kept > nearest))
			return nearest;
		// The receivers left out fall to the last cascade (containment), whose sphere holds every point of the view nearer than
		// the shadow distance only while the near bound they give is at most D k (Update): narrow fields of view and orthographic
		// views keep them, so the near bound comes to them as to any receiver.
		const double nearBound = std::max(static_cast<double>(view.Near), std::min(kept, static_cast<double>(view.NearestTransparent)) / c_Sqrt2);
		if (!(FromQuarters(QuartersBelow(nearBound)) <= static_cast<double>(view.ShadowDistance) * GetSphereReach(view)))
			return nearest;
		return kept;
	}

	bool ShadowFitTracker::GetBoundTargets(const ShadowFitView& view, int32_t& nearQuarters, int32_t& farQuarters) const
	{
		const RenderData::ShadowFitResult& result = m_Readback.Result;
		if (result.SurfacePixels == 0 || result.Farthest == 0)
			return false; // No receivers: nothing to fit to
		const double nearest = GetFittedNearest(view);
		const double farthest = DecodeOrderedFloat(result.Farthest);
		if (!std::isfinite(nearest) || !std::isfinite(farthest) || !(nearest <= farthest))
			return false;
		// Margins of half a stop each way for what moves until the next readback; the transparent surfaces' nearest too.
		const double distance = view.ShadowDistance;
		const double nearTarget = std::max(static_cast<double>(view.Near), std::min(nearest, static_cast<double>(view.NearestTransparent)) / c_Sqrt2);
		if (!(nearTarget * c_Sqrt2 < distance))
			return false; // Every receiver lies beyond the shadow distance
		nearQuarters = QuartersBelow(nearTarget);
		const double farTarget = std::min(distance, farthest * c_Sqrt2);
		// A far bound near the shadow distance is the shadow distance (the layout of a view whose receivers reach it); a single
		// cascade always ends there.
		if (view.CascadeCount < 2 || farTarget >= static_cast<double>(c_FarToDistance) * distance)
			farQuarters = c_DistanceQuarters;
		else
			farQuarters = std::max(QuartersAbove(farTarget), nearQuarters + 2); // F at least N 2^0.5
		return true;
	}

	void ShadowFitTracker::Reproject(const ShadowFitView& view)
	{
		// Each bin's view-space box through the readback's camera into world space relative to this frame's camera (in double:
		// worlds are large), then into this frame's light view space, and its distance along this frame's view direction.
		m_BinCount = 0;
		const glm::dmat3& readRotation = m_Readback.CameraRotation;
		const glm::dvec3 offset = m_Readback.CameraPosition - view.CameraPosition;
		const glm::dvec3 forward = -glm::dmat3(view.CameraRotation)[2];
		const double nearest = GetFittedNearest(view);
		for (const RenderData::ShadowFitBin& bin : m_Readback.Result.Bins)
		{
			if (bin.MaxD == 0 || DecodeOrderedFloat(bin.MaxD) < nearest)
				continue; // Empty, or only receivers the fit leaves out
			const double minX = DecodeOrderedFloat(~bin.MinX);
			const double minY = DecodeOrderedFloat(~bin.MinY);
			const double minD = DecodeOrderedFloat(~bin.MinD);
			const double maxX = DecodeOrderedFloat(bin.MaxX);
			const double maxY = DecodeOrderedFloat(bin.MaxY);
			const double maxD = DecodeOrderedFloat(bin.MaxD);
			if (!std::isfinite(minX) || !std::isfinite(minY) || !std::isfinite(minD) || !std::isfinite(maxX) || !std::isfinite(maxY) || !std::isfinite(maxD))
				continue;
			ReprojectedBin& out = m_Bins[m_BinCount++];
			out.Min = glm::dvec2(std::numeric_limits<double>::infinity());
			out.Max = glm::dvec2(-std::numeric_limits<double>::infinity());
			out.Nearest = std::numeric_limits<double>::infinity();
			out.Farthest = -std::numeric_limits<double>::infinity();
			for (const double x : { minX, maxX })
			{
				for (const double y : { minY, maxY })
				{
					for (const double d : { minD, maxD })
					{
						const glm::dvec3 world = readRotation * glm::dvec3(x, y, -d) + offset;
						const glm::dvec3 light = view.LightRotation * world;
						out.Min = glm::min(out.Min, glm::dvec2(light));
						out.Max = glm::max(out.Max, glm::dvec2(light));
						const double distance = glm::dot(forward, world);
						out.Nearest = std::min(out.Nearest, distance);
						out.Farthest = std::max(out.Farthest, distance);
					}
				}
			}
		}
	}

	int32_t ShadowFitTracker::GetSideTarget(const ShadowFitView& view, const ShadowFitLayout& layout, uint32_t cascade, glm::dvec2& boxMin, glm::dvec2& boxMax) const
	{
		const ShadowFitCascade& slice = layout.Cascades[cascade];
		if (view.MapSize <= 4 * c_FilterReachTexels)
			return c_SphereQuarters; // Too few texels to keep the filter's reach inside a fitted square
		// The receivers of the bins whose distance now overlaps the slice (the first slice's from the camera on: the first cascade
		// is selected for everything nearer than its split).
		const double sliceNear = cascade == 0 ? 0.0 : static_cast<double>(slice.SliceNear);
		const double sliceFar = static_cast<double>(slice.SliceFar);
		boxMin = glm::dvec2(std::numeric_limits<double>::infinity());
		boxMax = glm::dvec2(-std::numeric_limits<double>::infinity());
		bool any = false;
		for (uint32_t index = 0; index < m_BinCount; index++)
		{
			const ReprojectedBin& bin = m_Bins[index];
			if (bin.Farthest < sliceNear || bin.Nearest > sliceFar)
				continue;
			boxMin = glm::min(boxMin, bin.Min);
			boxMax = glm::max(boxMax, bin.Max);
			any = true;
		}
		if (!any)
			return c_SphereQuarters; // None seen in this slice: the sphere covers it

		// A camera that turned since the readback may show receivers within the distance the slice's corners moved.
		const glm::dmat3 turn = glm::dmat3(view.CameraRotation) - m_Readback.CameraRotation;
		glm::dvec2 margin(0.0);
		for (const double distance : { static_cast<double>(slice.SliceNear), sliceFar })
		{
			const double halfHeight = distance * static_cast<double>(view.TanHalfFOV);
			const double halfWidth = halfHeight * static_cast<double>(view.Aspect);
			for (const double x : { -1.0, 1.0 })
			{
				for (const double y : { -1.0, 1.0 })
				{
					const glm::dvec3 moved = view.LightRotation * (turn * glm::dvec3(x * halfWidth, y * halfHeight, -distance));
					margin = glm::max(margin, glm::abs(glm::dvec2(moved)));
				}
			}
		}
		boxMin -= margin;
		boxMax += margin;

		// A square around them, 5% larger and with the filter's reach in its own texels on each side, in quarter stops: at least a
		// sixteenth of the sphere, which is used once the square would be as large.
		const glm::dvec2 extent = boxMax - boxMin;
		const double mapSize = static_cast<double>(view.MapSize);
		const double side = c_SquareGrowth * std::max({ extent.x, extent.y, 1e-6 }) / (1.0 - 2.0 * static_cast<double>(c_FilterReachTexels) / mapSize);
		const double diameter = 2.0 * static_cast<double>(slice.Radius);
		const int32_t quarters = std::max(QuartersAbove(side), QuartersAbove(diameter / 16.0));
		if (!(FromQuarters(quarters) < diameter))
			return c_SphereQuarters;
		return quarters;
	}

	const ShadowFitLayout& ShadowFitTracker::Update(const ShadowFitView& view, const ShadowFitReadback* readback)
	{
		const bool newReadback = readback && readback->Epoch == m_Epoch;
		if (newReadback)
		{
			m_Readback = *readback;
			m_HasReadback = true;
		}

		const uint32_t count = std::min(view.CascadeCount, RenderData::c_MaxCascades);
		const double distance = static_cast<double>(view.ShadowDistance);
		ShadowFitLayout layout;
		int32_t nearTarget = 0;
		int32_t farTarget = 0;
		bool fitted = m_HasReadback && count > 0 && GetBoundTargets(view, nearTarget, farTarget);
		if (fitted)
		{
			// The first readback of an epoch, or after unfitted frames, sets the bounds at once: the unfitted layout is no state to
			// shrink from.
			const bool direct = !m_Fitted;
			if (direct)
			{
				m_NearQuarters = nearTarget;
				m_FarQuarters = farTarget;
				m_NearState = {};
				m_FarState = {};
			}
			else
			{
				Follow(nearTarget, m_NearQuarters, m_NearState, newReadback, false);
				Follow(farTarget, m_FarQuarters, m_FarState, newReadback, true);
			}
			const double fittedNear = std::max(static_cast<double>(view.Near), FromQuarters(m_NearQuarters));
			double fittedFar = m_FarQuarters == c_DistanceQuarters ? distance : std::max(FromQuarters(m_FarQuarters), fittedNear * c_Sqrt2);
			if (count < 2 || fittedFar >= distance)
				fittedFar = distance;
			fitted = fittedNear * c_Sqrt2 < distance; // Else the shadow distance shrank below the bounds: unfitted this frame
			if (fitted)
			{
				layout.Fitted = true;
				layout.Near = static_cast<float>(fittedNear);
				layout.Far = static_cast<float>(fittedFar);
				const float distribution = std::isfinite(view.Distribution) ? std::clamp(view.Distribution, 0.0f, 1.0f) : c_UnfittedDistribution;
				if (fittedFar < distance)
					LayOutSlices(view, layout, layout.Near, layout.Far, distribution, count - 1);
				else
					LayOutSlices(view, layout, layout.Near, view.ShadowDistance, distribution, count);

				// The last cascade's sphere holds every point of the view nearer than the shadow distance when its slice [a, D]
				// starts at a <= D k (GetSphereReach): any a < D at the usual fields of view. Narrower views (and orthographic ones)
				// take the sphere of [N, D], which holds every point from N on, and every point when N <= D k (GetFittedNearest
				// leaves no receiver out otherwise).
				ShadowFitCascade& last = layout.Cascades[count - 1];
				if (count > 1 && !(static_cast<double>(last.SliceNear) <= distance * GetSphereReach(view)))
					GetSliceSphere(view, layout.Near, view.ShadowDistance, last.SphereCenter, last.Radius);

				// The squares of every cascade but the last, perspective views only (an orthographic view keeps its spheres). Slices
				// that moved start their sides afresh.
				bool slicesMoved = direct || !m_Layout.Fitted || m_Layout.CascadeCount != count;
				for (uint32_t cascade = 0; !slicesMoved && cascade < count; cascade++)
				{
					const ShadowFitCascade& slice = layout.Cascades[cascade];
					const ShadowFitCascade& previous = m_Layout.Cascades[cascade];
					slicesMoved = slice.SliceNear != previous.SliceNear || slice.SliceFar != previous.SliceFar || slice.Radius != previous.Radius;
				}
				if (!view.Orthographic)
					Reproject(view);
				const glm::dvec3 cameraLight = view.LightRotation * view.CameraPosition;
				for (uint32_t cascade = 0; cascade + 1 < count; cascade++)
				{
					glm::dvec2 boxMin(0.0);
					glm::dvec2 boxMax(0.0);
					const int32_t target = view.Orthographic ? c_SphereQuarters : GetSideTarget(view, layout, cascade, boxMin, boxMax);
					if (slicesMoved)
					{
						m_SideQuarters[cascade] = target;
						m_SideStates[cascade] = {};
					}
					else
					{
						Follow(target, m_SideQuarters[cascade], m_SideStates[cascade], newReadback, true);
					}
					// A square is kept only while the frame's own target is one (a sphere target grows to the sphere at once), so
					// the receivers' center is this frame's.
					ShadowFitCascade& slice = layout.Cascades[cascade];
					const int32_t quarters = m_SideQuarters[cascade];
					if (quarters == c_SphereQuarters || target == c_SphereQuarters)
						continue;
					const double side = FromQuarters(quarters);
					if (!(side < 2.0 * static_cast<double>(slice.Radius)))
						continue;
					// The square stays where it is while it holds the receivers by the filter's reach (a still view whose readbacks
					// differ by the temporal jitter's sub-pixel offsets keeps its squares, so its cached cascades stay); else it is
					// centered on them, on whole texels of absolute light space, so edges do not shimmer while the camera moves.
					const double worldPerTexel = side / static_cast<double>(view.MapSize);
					const glm::dvec2 receiversMin = glm::dvec2(cameraLight) + boxMin;
					const glm::dvec2 receiversMax = glm::dvec2(cameraLight) + boxMax;
					const ShadowFitCascade& previous = m_Layout.Cascades[cascade];
					const double inner = 0.5 * side - static_cast<double>(c_FilterReachTexels) * worldPerTexel;
					const bool holds = m_Layout.Fitted && cascade < m_Layout.CascadeCount && previous.Fitted && previous.Side == static_cast<float>(side)
						&& glm::all(glm::greaterThanEqual(receiversMin, previous.Center - inner)) && glm::all(glm::lessThanEqual(receiversMax, previous.Center + inner));
					slice.Fitted = true;
					slice.Side = static_cast<float>(side);
					slice.Center = holds ? previous.Center : glm::floor(0.5 * (receiversMin + receiversMax) / worldPerTexel) * worldPerTexel;
					layout.SquaresFitted = true;
				}
			}
		}
		if (!fitted)
			layout = GetUnfittedLayout(view);
		m_Fitted = fitted;

		layout.Changed = m_Layout.CascadeCount != 0 && Differs(layout, m_Layout);
		if (layout.Changed)
		{
			m_LayoutVersion++;
			m_Changes++;
			m_Settled = false;
		}
		else if (newReadback && readback->LayoutVersion == m_LayoutVersion)
		{
			m_Settled = true; // A frame drawn with this layout measured receivers that keep it
		}
		m_Layout = layout;
		return m_Layout;
	}

}
