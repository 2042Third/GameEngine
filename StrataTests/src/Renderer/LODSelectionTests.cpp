#include <doctest/doctest.h>

#include "Strata/Renderer/LODSelection.h"
#include "Strata/Renderer/Mesh.h"
#include "Strata/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

using namespace Strata;

namespace
{

	// The rule as a logarithm: 1 + floor(log2(threshold / fraction)) below the threshold. It counts a fraction that doubles
	// exactly onto the threshold one level further than SelectLOD, which keeps doublings strictly below it.
	uint32_t SelectLODByLogarithm(float screenFraction, float lodThreshold, uint32_t lodCount)
	{
		if (lodCount <= 1 || !(screenFraction > 0.0f && screenFraction < lodThreshold))
			return 0;
		const float levels = std::floor(std::log2(lodThreshold / screenFraction));
		return std::min(static_cast<uint32_t>(1.0f + std::min(levels, static_cast<float>(lodCount))), lodCount - 1);
	}

	// Whether the fraction doubles onto the threshold or within a few float steps of it (fraction * 2^n close to the threshold
	// for some n >= 1): there the division and the logarithm round, and the logarithm may count a level the exact doublings do
	// not.
	bool NearBoundary(float screenFraction, float lodThreshold)
	{
		for (float doubled = screenFraction * 2.0f; doubled <= lodThreshold * 2.0f; doubled *= 2.0f)
		{
			if (std::abs(doubled / lodThreshold - 1.0f) < 1.0e-5f)
				return true;
		}
		return false;
	}

}

TEST_SUITE("Renderer.LODSelection")
{
	TEST_CASE("The screen fraction is the projected diameter over the viewport height")
	{
		SceneCamera camera;
		camera.Position = glm::vec3(0.0f);
		camera.VerticalFOV = glm::radians(90.0f); // tan(45 degrees) = 1: the view is 2 * distance high
		camera.Near = 0.5f;
		CHECK(LODSelection::GetScreenFraction(camera, glm::vec3(0.0f, 0.0f, -10.0f), 1.0f) == doctest::Approx(0.1f));
		CHECK(LODSelection::GetScreenFraction(camera, glm::vec3(0.0f, 0.0f, -20.0f), 1.0f) == doctest::Approx(0.05f));
		// Closer than the near plane counts as on it, so the fraction stays finite.
		CHECK(LODSelection::GetScreenFraction(camera, glm::vec3(0.0f), 1.0f) == doctest::Approx(2.0f));

		// Orthographic: the vertical extent of the view, whatever the distance.
		camera.Orthographic = true;
		camera.VerticalFOV = 8.0f;
		CHECK(LODSelection::GetScreenFraction(camera, glm::vec3(0.0f, 0.0f, -100.0f), 1.0f) == doctest::Approx(0.25f));
	}

	TEST_CASE("Each halving of the screen fraction below the threshold adds a level of detail, up to the last")
	{
		CHECK(LODSelection::SelectLOD(0.6f, 0.5f, 4) == 0);
		CHECK(LODSelection::SelectLOD(0.5f, 0.5f, 4) == 0);
		CHECK(LODSelection::SelectLOD(0.4f, 0.5f, 4) == 1);
		// Each level from the fraction that doubles to below the threshold: 0.25 doubles onto it, so it stays at 1.
		CHECK(LODSelection::SelectLOD(0.25f, 0.5f, 4) == 1);
		CHECK(LODSelection::SelectLOD(0.24f, 0.5f, 4) == 2);
		CHECK(LODSelection::SelectLOD(0.2f, 0.5f, 4) == 2);
		CHECK(LODSelection::SelectLOD(0.1f, 0.5f, 4) == 3);
		CHECK(LODSelection::SelectLOD(0.01f, 0.5f, 4) == 3);
		// Tiny fractions make the ratio overflow; the level stays the last one.
		CHECK(LODSelection::SelectLOD(1.0e-30f, 0.5f, 4) == 3);
		CHECK(LODSelection::SelectLOD(std::numeric_limits<float>::denorm_min(), 0.5f, 4) == 3);
		// Meshes with one level (or none) always draw level 0, and so do fractions that are not positive.
		CHECK(LODSelection::SelectLOD(0.01f, 0.5f, 1) == 0);
		CHECK(LODSelection::SelectLOD(0.01f, 0.5f, 0) == 0);
		CHECK(LODSelection::SelectLOD(0.0f, 0.5f, 4) == 0);
		CHECK(LODSelection::SelectLOD(-1.0f, 0.5f, 4) == 0);
		CHECK(LODSelection::SelectLOD(std::nanf(""), 0.5f, 4) == 0);
	}

	TEST_CASE("The level counts exact doublings of the fraction below the threshold, as the logarithm does away from its boundaries")
	{
		// A fraction that doubles exactly onto the threshold stays a level nearer than the logarithm puts it.
		CHECK(LODSelection::SelectLOD(0.125f, 0.5f, 8) == 2);
		CHECK(SelectLODByLogarithm(0.125f, 0.5f, 8) == 3);
		CHECK(LODSelection::SelectLOD(std::nextafter(0.125f, 0.0f), 0.5f, 8) == 3);

		std::mt19937 random(31);
		std::uniform_real_distribution<float> exponent(-30.0f, 2.0f);
		const float thresholds[] = { 0.5f, 0.3f, 1.0f, 0.05f };
		uint32_t compared = 0;
		for (int sample = 0; sample < 100'000; sample++)
		{
			const float fraction = std::exp2(exponent(random));
			const float threshold = thresholds[sample % std::size(thresholds)];
			const uint32_t lodCount = 1 + static_cast<uint32_t>(sample % 12);
			if (NearBoundary(fraction, threshold))
				continue;
			CAPTURE(fraction);
			CAPTURE(threshold);
			CAPTURE(lodCount);
			REQUIRE(LODSelection::SelectLOD(fraction, threshold, lodCount) == SelectLODByLogarithm(fraction, threshold, lodCount));
			compared++;
		}
		CHECK(compared > 99'000);
	}

	TEST_CASE("Levels with errors: the coarsest one whose error spans at most LODErrorPixels on screen")
	{
		// 1 pixel of a 1000-pixel viewport is 1/500 of its half height: the fraction (of the height) times the relative error
		// (of the radius, half the diameter the fraction measures) may reach that.
		CHECK(LODSelection::GetErrorThreshold(1.0f, 1000.0f) == 1.0f / 500.0f);
		CHECK(LODSelection::GetErrorThreshold(0.0f, 1000.0f) == 0.0f);
		CHECK(LODSelection::GetErrorThreshold(1.0f, 0.0f) == 0.0f);
		CHECK(LODSelection::GetErrorThreshold(std::nanf(""), 1000.0f) == 0.0f);
		CHECK(LODSelection::GetErrorPixels(0.5f, 0.01f, 1000.0f) == doctest::Approx(2.5f));

		const std::array<float, 4> errors = { 0.0f, 0.001f, 0.004f, 0.016f };
		const float threshold = LODSelection::GetErrorThreshold(1.0f, 1000.0f); // 0.002
		CHECK(LODSelection::SelectLODByError(4.0f, errors, threshold) == 0);   // Level 1 would span 2 pixels
		CHECK(LODSelection::SelectLODByError(2.0f, errors, threshold) == 1);   // 1 pixel: within
		CHECK(LODSelection::SelectLODByError(0.4f, errors, threshold) == 2);   // 0.8 pixels; level 3 would be 3.2
		CHECK(LODSelection::SelectLODByError(0.1f, errors, threshold) == 3);
		CHECK(LODSelection::SelectLODByError(1.0e-30f, errors, threshold) == 3);
		// Fractions that are not positive or not finite, a threshold of 0 and meshes without levels draw full detail.
		CHECK(LODSelection::SelectLODByError(0.0f, errors, threshold) == 0);
		CHECK(LODSelection::SelectLODByError(-1.0f, errors, threshold) == 0);
		CHECK(LODSelection::SelectLODByError(std::nanf(""), errors, threshold) == 0);
		CHECK(LODSelection::SelectLODByError(std::numeric_limits<float>::infinity(), errors, threshold) == 0);
		CHECK(LODSelection::SelectLODByError(0.1f, errors, 0.0f) == 0);
		CHECK(LODSelection::SelectLODByError(0.1f, std::span<const float>(errors.data(), 1), threshold) == 0);
		// A coarser level with a smaller error (simplification is not monotonic) wins: it is both cheaper and closer.
		const std::array<float, 3> uneven = { 0.0f, 0.004f, 0.001f };
		CHECK(LODSelection::SelectLODByError(2.0f, uneven, threshold) == 2);
	}

	TEST_CASE("The error rule is the projected error in pixels against LODErrorPixels, away from rounding at the boundaries")
	{
		std::mt19937 random(17);
		std::uniform_real_distribution<float> exponent(-20.0f, 2.0f);
		std::uniform_real_distribution<float> errorExponent(-12.0f, 0.0f);
		uint32_t compared = 0;
		for (int sample = 0; sample < 50'000; sample++)
		{
			const float fraction = std::exp2(exponent(random));
			const float viewportHeight = sample % 2 == 0 ? 1080.0f : 333.0f;
			const float pixels = sample % 3 == 0 ? 1.0f : 0.37f;
			std::vector<float> errors = { 0.0f };
			const uint32_t levels = 1 + static_cast<uint32_t>(sample % 6);
			for (uint32_t level = 0; level < levels; level++)
				errors.push_back(std::exp2(errorExponent(random)));
			std::sort(errors.begin() + 1, errors.end());
			// The reference in double: the coarsest level whose error spans at most `pixels`.
			uint32_t expected = 0;
			bool nearBoundary = false;
			for (uint32_t level = 1; level < errors.size(); level++)
			{
				const double projected = static_cast<double>(fraction) * errors[level] * viewportHeight * 0.5;
				nearBoundary = nearBoundary || std::abs(projected - pixels) <= pixels * 1e-5;
				if (projected <= pixels)
					expected = level;
			}
			if (nearBoundary)
				continue;
			CAPTURE(fraction);
			REQUIRE(LODSelection::SelectLODByError(fraction, errors, LODSelection::GetErrorThreshold(pixels, viewportHeight)) == expected);
			compared++;
		}
		CHECK(compared > 49'000);
	}

	TEST_CASE("A submesh chooses by its levels' errors only when every simplified level has one")
	{
		Submesh submesh;
		submesh.LODs = { MeshLOD { 0, 3, 0.0f }, MeshLOD { 3, 3, 0.01f }, MeshLOD { 6, 3, 0.04f } };
		const AABB bounds(glm::vec3(-1.0f), glm::vec3(1.0f)); // Radius sqrt(3)
		CHECK(MeshUtils::HasLODErrors(submesh));
		CHECK(MeshUtils::GetRelativeLODError(0.01f, bounds) == 0.01f / std::sqrt(3.0f));
		CHECK(MeshUtils::GetRelativeLODError(0.0f, bounds) == 0.0f);
		CHECK(MeshUtils::GetRelativeLODError(0.01f, AABB()) == 0.0f);
		CHECK(MeshUtils::GetRelativeLODError(0.01f, AABB(glm::vec3(1.0f), glm::vec3(1.0f))) == 0.0f);

		LODCriteria criteria;
		criteria.LODThreshold = 0.5f;
		criteria.ErrorThreshold = LODSelection::GetErrorThreshold(1.0f, 1000.0f);
		// By error: the same levels as the relative errors give.
		const std::array<float, 3> relative = { 0.0f, MeshUtils::GetRelativeLODError(0.01f, bounds), MeshUtils::GetRelativeLODError(0.04f, bounds) };
		for (const float fraction : { 1.0f, 0.3f, 0.2f, 0.05f, 0.01f, 0.4f })
		{
			CAPTURE(fraction);
			CHECK(LODSelection::SelectLOD(submesh, bounds, fraction, criteria) == LODSelection::SelectLODByError(fraction, relative, criteria.ErrorThreshold));
		}
		CHECK(LODSelection::SelectLOD(submesh, bounds, 1.0f, criteria) == 0);
		CHECK(LODSelection::SelectLOD(submesh, bounds, 0.01f, criteria) == 2);

		// A level without an error (a version 2 mesh's): halving, by LODThreshold.
		submesh.LODs[2].Error = 0.0f;
		CHECK_FALSE(MeshUtils::HasLODErrors(submesh));
		for (const float fraction : { 1.0f, 0.3f, 0.2f, 0.05f, 0.01f })
		{
			CAPTURE(fraction);
			CHECK(LODSelection::SelectLOD(submesh, bounds, fraction, criteria) == LODSelection::SelectLOD(fraction, 0.5f, 3));
		}
		Submesh single;
		single.LODs = { MeshLOD { 0, 3, 0.0f } };
		CHECK_FALSE(MeshUtils::HasLODErrors(single));
		CHECK(LODSelection::SelectLOD(single, bounds, 0.01f, criteria) == 0);
	}
}
