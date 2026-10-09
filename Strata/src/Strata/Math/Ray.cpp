#include "stpch.h"
#include "Strata/Math/Ray.h"

namespace Strata
{

	Ray Ray::FromNDC(const glm::vec2& ndc, const glm::mat4& inverseViewProjection)
	{
		// Reversed Z: depth 1 is the near plane. Depth 0.5 is a second point on the same ray that stays
		// finite even with an infinite far plane.
		const glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndc, 1.0f, 1.0f);
		const glm::vec4 farPoint = inverseViewProjection * glm::vec4(ndc, 0.5f, 1.0f);
		const glm::vec3 origin = glm::vec3(nearPoint) / nearPoint.w;
		const glm::vec3 target = glm::vec3(farPoint) / farPoint.w;
		return Ray(origin, target - origin);
	}

	std::optional<float> IntersectRayAABB(const Ray& ray, const AABB& box)
	{
		if (!box.IsValid())
			return std::nullopt;

		float entry = 0.0f;
		float exit = std::numeric_limits<float>::max();
		for (int axis = 0; axis < 3; axis++)
		{
			const float direction = ray.Direction[axis];
			const float origin = ray.Origin[axis];
			if (glm::abs(direction) < 1e-12f)
			{
				// Parallel to this slab: miss unless the origin lies within it.
				if (origin < box.Min[axis] || origin > box.Max[axis])
					return std::nullopt;
				continue;
			}

			const float inverse = 1.0f / direction;
			float slabEntry = (box.Min[axis] - origin) * inverse;
			float slabExit = (box.Max[axis] - origin) * inverse;
			if (slabEntry > slabExit)
				std::swap(slabEntry, slabExit);

			entry = std::max(entry, slabEntry);
			exit = std::min(exit, slabExit);
			if (entry > exit)
				return std::nullopt;
		}
		return entry;
	}

	std::optional<float> IntersectRaySphere(const Ray& ray, const glm::vec3& center, float radius)
	{
		const glm::vec3 toOrigin = ray.Origin - center;
		const float b = glm::dot(toOrigin, ray.Direction);
		const float c = glm::dot(toOrigin, toOrigin) - radius * radius;
		if (c > 0.0f && b > 0.0f)
			return std::nullopt; // Outside and pointing away

		const float discriminant = b * b - c;
		if (discriminant < 0.0f)
			return std::nullopt;

		const float root = glm::sqrt(discriminant);
		const float distance = -b - root;
		return distance >= 0.0f ? distance : -b + root;
	}

	std::optional<float> IntersectRayPlane(const Ray& ray, const glm::vec4& plane)
	{
		const glm::vec3 normal(plane);
		const float denominator = glm::dot(normal, ray.Direction);
		if (glm::abs(denominator) < 1e-8f)
			return std::nullopt;

		const float distance = -(glm::dot(normal, ray.Origin) + plane.w) / denominator;
		if (distance < 0.0f)
			return std::nullopt;
		return distance;
	}

	std::optional<float> IntersectRayTriangle(const Ray& ray, const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2, bool cullBackFaces)
	{
		constexpr float epsilon = 1e-8f;
		const glm::vec3 edge1 = v1 - v0;
		const glm::vec3 edge2 = v2 - v0;
		const glm::vec3 p = glm::cross(ray.Direction, edge2);
		const float determinant = glm::dot(edge1, p);

		// Positive determinant: the ray hits the counter-clockwise (front) side.
		if (cullBackFaces ? determinant < epsilon : glm::abs(determinant) < epsilon)
			return std::nullopt;

		const float inverseDeterminant = 1.0f / determinant;
		const glm::vec3 t = ray.Origin - v0;
		const float u = glm::dot(t, p) * inverseDeterminant;
		if (u < 0.0f || u > 1.0f)
			return std::nullopt;

		const glm::vec3 q = glm::cross(t, edge1);
		const float v = glm::dot(ray.Direction, q) * inverseDeterminant;
		if (v < 0.0f || u + v > 1.0f)
			return std::nullopt;

		const float distance = glm::dot(edge2, q) * inverseDeterminant;
		if (distance < 0.0f)
			return std::nullopt;
		return distance;
	}

}
