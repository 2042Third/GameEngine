#pragma once

#include "Strata/Math/AABB.h"

#include <glm/glm.hpp>

#include <optional>

namespace Strata
{

	struct Ray
	{
		glm::vec3 Origin = glm::vec3(0.0f);
		glm::vec3 Direction = glm::vec3(0.0f, 0.0f, -1.0f); // Normalized

		Ray() = default;
		Ray(const glm::vec3& origin, const glm::vec3& direction)
			: Origin(origin), Direction(glm::normalize(direction))
		{
		}

		glm::vec3 GetPoint(float distance) const { return Origin + Direction * distance; }

		// Ray through a point in normalized device coordinates (x, y in [-1, 1], +Y up), starting on the
		// near plane. Works for finite and infinite reversed-Z projections.
		static Ray FromNDC(const glm::vec2& ndc, const glm::mat4& inverseViewProjection);
	};

	// Intersection tests return the distance along the ray to the first hit, or nullopt on a miss.
	// Rays starting inside a box or sphere report distance 0 for the box and the exit distance for the sphere.
	std::optional<float> IntersectRayAABB(const Ray& ray, const AABB& box);
	std::optional<float> IntersectRaySphere(const Ray& ray, const glm::vec3& center, float radius);
	// Plane given as (normal, distance) with dot(normal, p) + distance = 0.
	std::optional<float> IntersectRayPlane(const Ray& ray, const glm::vec4& plane);
	// Moller-Trumbore. Counter-clockwise triangles face the viewer; set cullBackFaces to ignore the other side.
	std::optional<float> IntersectRayTriangle(const Ray& ray, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, bool cullBackFaces = false);

}
