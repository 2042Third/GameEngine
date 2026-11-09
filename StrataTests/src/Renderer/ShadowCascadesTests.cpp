#include <doctest/doctest.h>

#include "Strata/Renderer/ShadowCascades.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Strata;

namespace
{

	glm::mat3 LookRotation(const glm::vec3& forward)
	{
		const glm::vec3 direction = glm::normalize(forward);
		const glm::vec3 up = std::abs(direction.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		return glm::mat3(glm::inverse(glm::lookAt(glm::vec3(0.0f), direction, up))); // View space to world space
	}

	glm::dmat3 LightRotation(const glm::vec3& direction)
	{
		const glm::vec3 light = glm::normalize(direction);
		const glm::vec3 up = std::abs(light.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
		return glm::dmat3(glm::mat3(glm::lookAt(glm::vec3(0.0f), light, up)));
	}

	ShadowCascadeView MakeView(const glm::vec3& forward, float shadowDistance = 80.0f, uint32_t mapSize = 2048)
	{
		ShadowCascadeView view;
		view.CameraRotation = LookRotation(forward);
		view.Near = 0.1f;
		view.ShadowDistance = shadowDistance;
		view.TanHalfFOV = std::tan(glm::radians(30.0f));
		view.Aspect = 16.0f / 9.0f;
		view.CascadeCount = 4;
		view.MapSize = mapSize;
		return view;
	}

	// The corners of the view's slice [sliceNear, sliceFar] and points inside it, relative to the camera.
	std::vector<glm::vec3> SlicePoints(const ShadowCascadeView& view, float sliceNear, float sliceFar)
	{
		std::vector<glm::vec3> points;
		for (int step = 0; step <= 4; step++)
		{
			const float distance = sliceNear + (sliceFar - sliceNear) * static_cast<float>(step) / 4.0f;
			const float halfHeight = view.Orthographic ? 0.5f * view.OrthographicHeight : distance * view.TanHalfFOV;
			const float halfWidth = halfHeight * view.Aspect;
			for (int y = -2; y <= 2; y++)
			{
				for (int x = -2; x <= 2; x++)
					points.push_back(view.CameraRotation * glm::vec3(0.5f * static_cast<float>(x) * halfWidth, 0.5f * static_cast<float>(y) * halfHeight, -distance));
			}
		}
		return points;
	}

}

TEST_SUITE("Renderer.ShadowCascades")
{
	TEST_CASE("Splits follow the shadow distance and the light's distribution from a fixed start, never the near clip")
	{
		ShadowCascadeView view = MakeView(glm::vec3(0.0f, -0.2f, -1.0f));
		const ShadowCascadeLayout layout = ShadowCascades::GetLayout(view);
		REQUIRE(layout.CascadeCount == 4);
		for (uint32_t cascade = 0; cascade < 3; cascade++)
			CHECK(layout.Cascades[cascade].SliceFar == doctest::Approx(ShadowCascades::GetSplit(ShadowCascades::c_SplitStart, 80.0f, 0.9f, cascade, 4)));
		CHECK(layout.Cascades[3].SliceFar == 80.0f);
		// 0.9 of logarithmic splits from 1.5 m to 80 m: 5.76, 13.93 and 32.68 m.
		CHECK(layout.Cascades[0].SliceFar == doctest::Approx(5.761f).epsilon(0.001));
		CHECK(layout.Cascades[1].SliceFar == doctest::Approx(13.934f).epsilon(0.001));
		CHECK(layout.Cascades[2].SliceFar == doctest::Approx(32.680f).epsilon(0.001));
		// The first slice starts at the near clip, the others at the band before the previous split.
		CHECK(layout.Cascades[0].SliceNear == 0.1f);
		for (uint32_t cascade = 1; cascade < 4; cascade++)
		{
			const float previous = cascade > 1 ? layout.Cascades[cascade - 2].SliceFar : 0.0f;
			const float split = layout.Cascades[cascade - 1].SliceFar;
			CHECK(layout.Cascades[cascade].SliceNear == doctest::Approx(split - 0.1f * (split - previous)));
		}

		// A long shadow distance keeps the first cascade within c_MaxFirstSplit; the others follow the distribution.
		ShadowCascadeView distant = view;
		distant.ShadowDistance = 200.0f;
		const ShadowCascadeLayout farLayout = ShadowCascades::GetLayout(distant);
		CHECK(ShadowCascades::GetSplit(ShadowCascades::c_SplitStart, 200.0f, 0.9f, 0, 4) > ShadowCascades::c_MaxFirstSplit);
		CHECK(farLayout.Cascades[0].SliceFar == ShadowCascades::c_MaxFirstSplit);
		CHECK(farLayout.Cascades[1].SliceFar == doctest::Approx(ShadowCascades::GetSplit(ShadowCascades::c_SplitStart, 200.0f, 0.9f, 1, 4)));
		CHECK(farLayout.Cascades[2].SliceFar == doctest::Approx(ShadowCascades::GetSplit(ShadowCascades::c_SplitStart, 200.0f, 0.9f, 2, 4)));
		CHECK(farLayout.Cascades[1].SliceNear == doctest::Approx(ShadowCascades::c_MaxFirstSplit * 0.9f));

		// Another lens's near clip moves no split.
		for (const float nearClip : { 0.01f, 0.3f })
		{
			ShadowCascadeView other = view;
			other.Near = nearClip;
			const ShadowCascadeLayout otherLayout = ShadowCascades::GetLayout(other);
			for (uint32_t cascade = 0; cascade < 4; cascade++)
				CHECK(otherLayout.Cascades[cascade].SliceFar == layout.Cascades[cascade].SliceFar);
		}

		// Three cascades (Low) over the same range; a shadow distance shorter than twice the start splits from half of it.
		view.CascadeCount = 3;
		const ShadowCascadeLayout three = ShadowCascades::GetLayout(view);
		REQUIRE(three.CascadeCount == 3);
		CHECK(ShadowCascades::GetSplit(ShadowCascades::c_SplitStart, 80.0f, 0.9f, 0, 3) > ShadowCascades::c_MaxFirstSplit);
		CHECK(three.Cascades[0].SliceFar == ShadowCascades::c_MaxFirstSplit);
		CHECK(three.Cascades[1].SliceFar == doctest::Approx(ShadowCascades::GetSplit(ShadowCascades::c_SplitStart, 80.0f, 0.9f, 1, 3)));
		CHECK(three.Cascades[2].SliceFar == 80.0f);
		view.ShadowDistance = 1.0f;
		const ShadowCascadeLayout shortLayout = ShadowCascades::GetLayout(view);
		CHECK(shortLayout.Cascades[0].SliceFar == doctest::Approx(ShadowCascades::GetSplit(0.5f, 1.0f, 0.9f, 0, 3)));
		for (uint32_t cascade = 0; cascade < 3; cascade++)
		{
			CHECK(shortLayout.Cascades[cascade].SliceNear <= shortLayout.Cascades[cascade].SliceFar);
			CHECK(shortLayout.Cascades[cascade].Radius > 0.0f);
		}
	}

	TEST_CASE("A turning camera keeps every cascade's slice, sphere and texels")
	{
		const ShadowCascadeLayout reference = ShadowCascades::GetLayout(MakeView(glm::vec3(0.0f, 0.0f, -1.0f)));
		for (const glm::vec3& forward : { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.3f, -0.8f, 0.2f), glm::vec3(-0.7f, 0.4f, 0.6f), glm::vec3(0.0f, -1.0f, 0.001f) })
		{
			CAPTURE(forward);
			const ShadowCascadeView view = MakeView(forward);
			const ShadowCascadeLayout layout = ShadowCascades::GetLayout(view);
			REQUIRE(layout.CascadeCount == reference.CascadeCount);
			for (uint32_t cascade = 0; cascade < layout.CascadeCount; cascade++)
			{
				const ShadowCascadeSlice& slice = layout.Cascades[cascade];
				CHECK(slice.SliceNear == reference.Cascades[cascade].SliceNear);
				CHECK(slice.SliceFar == reference.Cascades[cascade].SliceFar);
				CHECK(slice.Radius == reference.Cascades[cascade].Radius);
				CHECK(slice.WorldPerTexel == reference.Cascades[cascade].WorldPerTexel);
				// The sphere turns with the camera: along the view, as far as before.
				CHECK(glm::length(slice.SphereCenter) == doctest::Approx(glm::length(reference.Cascades[cascade].SphereCenter)).epsilon(1e-5));
				CHECK(glm::dot(glm::normalize(slice.SphereCenter), view.CameraRotation * glm::vec3(0.0f, 0.0f, -1.0f)) == doctest::Approx(1.0f).epsilon(1e-5));
			}
		}
	}

	TEST_CASE("A sphere is the smallest around its slice and holds all of it")
	{
		for (const bool orthographic : { false, true })
		{
			CAPTURE(orthographic);
			ShadowCascadeView view = MakeView(glm::vec3(0.4f, -0.5f, -1.0f), 200.0f);
			view.Orthographic = orthographic;
			view.OrthographicHeight = 30.0f;
			const ShadowCascadeLayout layout = ShadowCascades::GetLayout(view);
			for (uint32_t cascade = 0; cascade < layout.CascadeCount; cascade++)
			{
				CAPTURE(cascade);
				const ShadowCascadeSlice& slice = layout.Cascades[cascade];
				float farthest = 0.0f;
				for (const glm::vec3& point : SlicePoints(view, slice.SliceNear, slice.SliceFar))
					farthest = std::max(farthest, glm::length(point - slice.SphereCenter));
				CHECK(farthest <= slice.Radius * 1.00001f);
				// Rounded up to 1/16 m from the farthest corner (one of the points above): no larger.
				CHECK(slice.Radius <= farthest + 0.0625f + 1e-4f);
			}
		}

		// A wide view's sphere sits at its slice's far end, its radius the far end's half-diagonal: 1.178 times the split
		// at 60 degrees and 16:9, under the eight corners' centroid sphere of earlier releases.
		const ShadowCascadeView view = MakeView(glm::vec3(0.0f, 0.0f, -1.0f));
		const ShadowCascadeLayout layout = ShadowCascades::GetLayout(view);
		const float split = layout.Cascades[0].SliceFar;
		const float halfDiagonal = split * view.TanHalfFOV * std::sqrt(1.0f + view.Aspect * view.Aspect);
		CHECK(layout.Cascades[0].SphereCenter.z == doctest::Approx(-split));
		CHECK(layout.Cascades[0].Radius == doctest::Approx(std::ceil(halfDiagonal * 16.0f) / 16.0f));
	}

	TEST_CASE("Squares cover their spheres by the filter's reach and move in whole texels with the camera")
	{
		const glm::dmat3 lightRotation = LightRotation(glm::vec3(-0.3f, -1.0f, -0.2f));
		for (const uint32_t mapSize : { 1024u, 4096u, 8192u })
		{
			CAPTURE(mapSize);
			const ShadowCascadeView view = MakeView(glm::vec3(0.2f, -0.3f, -1.0f), 80.0f, mapSize);
			const ShadowCascadeLayout layout = ShadowCascades::GetLayout(view);
			for (uint32_t cascade = 0; cascade < layout.CascadeCount; cascade++)
			{
				CAPTURE(cascade);
				const ShadowCascadeSlice& slice = layout.Cascades[cascade];
				const double texel = slice.WorldPerTexel;
				CHECK(texel * static_cast<double>(mapSize) == doctest::Approx(2.0 * slice.Radius + 2.0 * (ShadowCascades::c_FilterReachTexels + 1) * texel));
				ShadowCascadeSquare previous;
				// A camera far from the origin (12 km), sliding 7 mm a frame.
				for (int frame = 0; frame < 40; frame++)
				{
					const glm::dvec3 camera = glm::dvec3(12000.0, 35.0, -8000.0) + glm::dvec3(0.007 * frame, 0.0, 0.003 * frame);
					const glm::dvec3 cameraLight = lightRotation * camera;
					const ShadowCascadeSquare square = ShadowCascades::GetSquare(slice, cameraLight, lightRotation, mapSize);
					CHECK(square.HalfSide == doctest::Approx(0.5 * texel * mapSize));
					// On a corner of the grid of whole texels of absolute light space.
					for (int axis = 0; axis < 2; axis++)
					{
						const double texels = square.Center[axis] / texel;
						CHECK(std::abs(texels - std::round(texels)) < 1e-6);
					}
					if (frame > 0)
					{
						const glm::dvec2 moved = (square.Center - previous.Center) / texel;
						CHECK(std::abs(moved.x - std::round(moved.x)) < 1e-6);
						CHECK(std::abs(moved.y - std::round(moved.y)) < 1e-6);
					}
					// Every point of the slice lies c_FilterReachTexels texels inside the square, and within the sphere's depth.
					const glm::dvec3 sphere = cameraLight + lightRotation * glm::dvec3(slice.SphereCenter);
					for (const glm::vec3& point : SlicePoints(view, slice.SliceNear, slice.SliceFar))
					{
						const glm::dvec3 light = cameraLight + lightRotation * glm::dvec3(point);
						const glm::dvec2 offset = glm::abs(glm::dvec2(light) - square.Center);
						CHECK(std::max(offset.x, offset.y) <= square.HalfSide - ShadowCascades::c_FilterReachTexels * texel + 1e-6);
						CHECK(std::abs(light.z - sphere.z) <= slice.Radius + 1e-4);
					}
					previous = square;
				}
			}
		}
	}
}
