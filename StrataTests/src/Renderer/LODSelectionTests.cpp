#include <doctest/doctest.h>

#include "Strata/Renderer/LODSelection.h"
#include "Strata/Renderer/SceneRenderer.h"

#include <glm/glm.hpp>

#include <cmath>
#include <limits>

using namespace Strata;

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
		CHECK(LODSelection::SelectLOD(0.25f, 0.5f, 4) == 2);
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
}
