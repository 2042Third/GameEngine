#include <doctest/doctest.h>

#include "Strata/Renderer/LODSelection.h"
#include "Strata/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

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
}
