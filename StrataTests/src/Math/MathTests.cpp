#include <doctest/doctest.h>

#include "Strata/Math/AABB.h"
#include "Strata/Math/Frustum.h"
#include "Strata/Math/Math.h"
#include "Strata/Math/Random.h"
#include "Strata/Math/Ray.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace Strata;

namespace
{
	glm::vec3 ProjectToNDC(const glm::mat4& viewProjection, const glm::vec3& point)
	{
		const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
		return glm::vec3(clip) / clip.w;
	}
}

TEST_SUITE("Math")
{
	TEST_CASE("Compose and decompose transforms round trip")
	{
		const glm::vec3 translation(1.0f, -2.0f, 3.5f);
		const glm::quat rotation = Math::EulerDegreesToQuat(glm::vec3(30.0f, 45.0f, -60.0f));
		const glm::vec3 scale(2.0f, 0.5f, 3.0f);

		const glm::mat4 transform = Math::ComposeTransform(translation, rotation, scale);
		glm::vec3 outTranslation;
		glm::quat outRotation;
		glm::vec3 outScale;
		REQUIRE(Math::DecomposeTransform(transform, outTranslation, outRotation, outScale));
		CHECK(Math::IsNearlyEqual(outTranslation, translation, 1e-4f));
		CHECK(Math::IsNearlyEqual(outScale, scale, 1e-4f));
		CHECK(Math::IsNearlyEqual(outRotation, rotation, 1e-4f));

		// Matches the equivalent glm construction.
		const glm::mat4 expected = glm::translate(glm::mat4(1.0f), translation) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
		for (int column = 0; column < 4; column++)
			CHECK(Math::IsNearlyEqual(glm::vec3(transform[column]), glm::vec3(expected[column]), 1e-4f));
	}

	TEST_CASE("Decompose handles negative scale and rejects degenerate matrices")
	{
		const glm::mat4 mirrored = Math::ComposeTransform(glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 1.0f, 1.0f));
		glm::vec3 translation;
		glm::quat rotation;
		glm::vec3 scale;
		REQUIRE(Math::DecomposeTransform(mirrored, translation, rotation, scale));
		const glm::mat4 recomposed = Math::ComposeTransform(translation, rotation, scale);
		for (int column = 0; column < 4; column++)
			CHECK(Math::IsNearlyEqual(glm::vec3(recomposed[column]), glm::vec3(mirrored[column]), 1e-5f));

		CHECK_FALSE(Math::DecomposeTransform(glm::mat4(0.0f), translation, rotation, scale));
	}

	TEST_CASE("Euler conversions round trip")
	{
		const glm::vec3 euler(10.0f, 20.0f, 30.0f);
		CHECK(Math::IsNearlyEqual(Math::QuatToEulerDegrees(Math::EulerDegreesToQuat(euler)), euler, 1e-3f));
		CHECK(Math::IsNearlyEqual(Math::GetForwardDirection(glm::quat(1.0f, 0.0f, 0.0f, 0.0f)), glm::vec3(0.0f, 0.0f, -1.0f)));
		// Yaw 90 degrees turns -Z forward into -X.
		CHECK(Math::IsNearlyEqual(Math::GetForwardDirection(Math::EulerDegreesToQuat({ 0.0f, 90.0f, 0.0f })), glm::vec3(-1.0f, 0.0f, 0.0f), 1e-5f));
	}

	TEST_CASE("LookRotation points -Z along the direction")
	{
		const glm::vec3 directions[] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 2.0f, -3.0f }, { 0.0f, -1.0f, 0.0f } };
		for (const glm::vec3& direction : directions)
		{
			const glm::quat rotation = Math::LookRotation(direction);
			CHECK(Math::IsNearlyEqual(Math::GetForwardDirection(rotation), glm::normalize(direction), 1e-4f));
		}
		CHECK(Math::IsNearlyEqual(Math::LookRotation(glm::vec3(0.0f)), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
	}

	TEST_CASE("Reversed-Z perspective maps near to 1 and far to 0")
	{
		const glm::mat4 projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
		CHECK(ProjectToNDC(projection, { 0.0f, 0.0f, -0.1f }).z == doctest::Approx(1.0f).epsilon(1e-5));
		CHECK(ProjectToNDC(projection, { 0.0f, 0.0f, -100.0f }).z == doctest::Approx(0.0f).epsilon(1e-5));
		// Depth decreases with distance.
		CHECK(ProjectToNDC(projection, { 0.0f, 0.0f, -1.0f }).z > ProjectToNDC(projection, { 0.0f, 0.0f, -10.0f }).z);
		// +Y in view space is +Y in NDC.
		CHECK(ProjectToNDC(projection, { 0.0f, 1.0f, -5.0f }).y > 0.0f);

		const glm::mat4 infinite = Math::PerspectiveReverseZInfinite(glm::radians(60.0f), 1.0f, 0.1f);
		CHECK(ProjectToNDC(infinite, { 0.0f, 0.0f, -0.1f }).z == doctest::Approx(1.0f));
		CHECK(ProjectToNDC(infinite, { 0.0f, 0.0f, -1.0e7f }).z == doctest::Approx(0.0f).epsilon(1e-5));
	}

	TEST_CASE("Reversed-Z orthographic projection")
	{
		const glm::mat4 projection = Math::OrthographicReverseZ(-10.0f, 10.0f, -5.0f, 5.0f, 0.5f, 50.0f);
		CHECK(ProjectToNDC(projection, { 0.0f, 0.0f, -0.5f }).z == doctest::Approx(1.0f));
		CHECK(ProjectToNDC(projection, { 0.0f, 0.0f, -50.0f }).z == doctest::Approx(0.0f).epsilon(1e-5));
		CHECK(ProjectToNDC(projection, { 10.0f, 5.0f, -1.0f }).x == doctest::Approx(1.0f));
		CHECK(ProjectToNDC(projection, { 10.0f, 5.0f, -1.0f }).y == doctest::Approx(1.0f));
	}

	TEST_CASE("AABB operations")
	{
		AABB box;
		CHECK_FALSE(box.IsValid());
		box.Expand(glm::vec3(1.0f, 2.0f, 3.0f));
		box.Expand(glm::vec3(-1.0f, 0.0f, 1.0f));
		CHECK(box.IsValid());
		CHECK(box.GetCenter() == glm::vec3(0.0f, 1.0f, 2.0f));
		CHECK(box.GetExtents() == glm::vec3(1.0f, 1.0f, 1.0f));
		CHECK(box.Contains(glm::vec3(0.0f, 1.0f, 2.0f)));
		CHECK_FALSE(box.Contains(glm::vec3(0.0f, 5.0f, 2.0f)));
		CHECK(box.Intersects(AABB({ 0.5f, 0.5f, 0.5f }, { 3.0f, 3.0f, 3.0f })));
		CHECK_FALSE(box.Intersects(AABB({ 5.0f, 5.0f, 5.0f }, { 6.0f, 6.0f, 6.0f })));

		// Rotating a unit cube 45 degrees about Y widens it to sqrt(2) on X and Z.
		const AABB unit({ -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f });
		const glm::mat4 rotation = glm::rotate(glm::mat4(1.0f), glm::radians(45.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const AABB rotated = unit.Transformed(glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f)) * rotation);
		CHECK(rotated.GetCenter().x == doctest::Approx(10.0f));
		CHECK(rotated.GetExtents().x == doctest::Approx(glm::sqrt(2.0f) * 0.5f));
		CHECK(rotated.GetExtents().y == doctest::Approx(0.5f));
		CHECK_FALSE(AABB().Transformed(rotation).IsValid());
	}

	TEST_CASE("Frustum culling with reversed-Z projections")
	{
		const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 projection = Math::PerspectiveReverseZ(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
		const Frustum frustum(projection * view);

		CHECK(frustum.IsPointVisible(glm::vec3(0.0f)));
		CHECK_FALSE(frustum.IsPointVisible(glm::vec3(0.0f, 0.0f, 20.0f))); // Behind the camera
		CHECK_FALSE(frustum.IsPointVisible(glm::vec3(0.0f, 0.0f, -200.0f))); // Beyond the far plane
		CHECK_FALSE(frustum.IsPointVisible(glm::vec3(100.0f, 0.0f, 0.0f))); // Outside the side planes

		CHECK(frustum.IsSphereVisible(glm::vec3(7.0f, 0.0f, 0.0f), 3.0f));
		CHECK_FALSE(frustum.IsSphereVisible(glm::vec3(50.0f, 0.0f, 0.0f), 1.0f));

		CHECK(frustum.IsAABBVisible(AABB({ -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f })));
		CHECK(frustum.IsAABBVisible(AABB({ 5.0f, -1.0f, -1.0f }, { 50.0f, 1.0f, 1.0f }))); // Straddles a side plane
		CHECK_FALSE(frustum.IsAABBVisible(AABB({ 40.0f, -1.0f, -1.0f }, { 50.0f, 1.0f, 1.0f })));
		CHECK_FALSE(frustum.IsAABBVisible(AABB()));

		// Infinite projection: nothing is culled by distance.
		const Frustum infinite(Math::PerspectiveReverseZInfinite(glm::radians(60.0f), 1.0f, 0.1f) * view);
		CHECK(infinite.IsPointVisible(glm::vec3(0.0f, 0.0f, -1.0e6f)));
		CHECK_FALSE(infinite.IsPointVisible(glm::vec3(0.0f, 0.0f, 20.0f)));
	}

	TEST_CASE("Ray from NDC passes through the projected point")
	{
		const glm::mat4 view = glm::lookAtRH(glm::vec3(3.0f, 4.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 projections[] = {
			Math::PerspectiveReverseZ(glm::radians(50.0f), 1.5f, 0.1f, 500.0f),
			Math::PerspectiveReverseZInfinite(glm::radians(50.0f), 1.5f, 0.1f)
		};
		for (const glm::mat4& projection : projections)
		{
			const glm::mat4 viewProjection = projection * view;
			const glm::vec3 target(1.0f, -0.5f, 2.0f);
			const glm::vec3 ndc = ProjectToNDC(viewProjection, target);
			const Ray ray = Ray::FromNDC(glm::vec2(ndc), glm::inverse(viewProjection));

			// The closest point on the ray to the target should be (nearly) the target itself.
			const float distanceAlongRay = glm::dot(target - ray.Origin, ray.Direction);
			CHECK(glm::length(ray.GetPoint(distanceAlongRay) - target) < 1e-2f);
		}
	}

	TEST_CASE("Ray intersections")
	{
		const Ray ray(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));

		const AABB box({ -1.0f, -1.0f, -1.0f }, { 1.0f, 1.0f, 1.0f });
		CHECK(IntersectRayAABB(ray, box).value() == doctest::Approx(9.0f));
		CHECK(IntersectRayAABB(Ray(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)), box).value() == doctest::Approx(0.0f));
		CHECK_FALSE(IntersectRayAABB(Ray(glm::vec3(5.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f)), box).has_value());
		CHECK_FALSE(IntersectRayAABB(Ray(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, 1.0f)), box).has_value());

		CHECK(IntersectRaySphere(ray, glm::vec3(0.0f), 2.0f).value() == doctest::Approx(8.0f));
		CHECK(IntersectRaySphere(Ray(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f)), glm::vec3(0.0f), 2.0f).value() == doctest::Approx(2.0f));
		CHECK_FALSE(IntersectRaySphere(ray, glm::vec3(10.0f, 0.0f, 0.0f), 1.0f).has_value());

		CHECK(IntersectRayPlane(ray, glm::vec4(0.0f, 0.0f, 1.0f, 0.0f)).value() == doctest::Approx(10.0f));
		CHECK_FALSE(IntersectRayPlane(ray, glm::vec4(1.0f, 0.0f, 0.0f, 0.0f)).has_value());

		// Counter-clockwise when viewed from +Z: front face for this ray.
		const glm::vec3 v0(-1.0f, -1.0f, 0.0f);
		const glm::vec3 v1(1.0f, -1.0f, 0.0f);
		const glm::vec3 v2(0.0f, 1.0f, 0.0f);
		CHECK(IntersectRayTriangle(ray, v0, v1, v2, true).value() == doctest::Approx(10.0f));
		CHECK_FALSE(IntersectRayTriangle(ray, v0, v2, v1, true).has_value()); // Back face culled
		CHECK(IntersectRayTriangle(ray, v0, v2, v1, false).value() == doctest::Approx(10.0f));
		CHECK_FALSE(IntersectRayTriangle(Ray(glm::vec3(5.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f)), v0, v1, v2).has_value());
	}

	TEST_CASE("Random values stay in range and seeding is reproducible")
	{
		Random::Seed(1234);
		const float first = Random::Float();
		Random::Seed(1234);
		CHECK(Random::Float() == first);

		for (int index = 0; index < 1000; index++)
		{
			const float value = Random::Float(-2.0f, 3.0f);
			CHECK((value >= -2.0f && value < 3.0f));
			const int32_t integer = Random::Int(-5, 5);
			CHECK((integer >= -5 && integer <= 5));
			CHECK(glm::length(Random::UnitVector()) == doctest::Approx(1.0f).epsilon(1e-4));
		}
	}
}
