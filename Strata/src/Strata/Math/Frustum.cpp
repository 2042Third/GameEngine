#include "stpch.h"
#include "Strata/Math/Frustum.h"

namespace Strata
{

	Frustum::Frustum(const glm::mat4& viewProjection)
	{
		Update(viewProjection);
	}

	void Frustum::Update(const glm::mat4& viewProjection)
	{
		// Gribb/Hartmann plane extraction. glm is column-major: row i is (m[0][i], m[1][i], m[2][i], m[3][i]).
		const glm::vec4 row0(viewProjection[0][0], viewProjection[1][0], viewProjection[2][0], viewProjection[3][0]);
		const glm::vec4 row1(viewProjection[0][1], viewProjection[1][1], viewProjection[2][1], viewProjection[3][1]);
		const glm::vec4 row2(viewProjection[0][2], viewProjection[1][2], viewProjection[2][2], viewProjection[3][2]);
		const glm::vec4 row3(viewProjection[0][3], viewProjection[1][3], viewProjection[2][3], viewProjection[3][3]);

		m_Planes[0] = row3 + row0; // Left   (x >= -w)
		m_Planes[1] = row3 - row0; // Right  (x <= w)
		m_Planes[2] = row3 + row1; // Bottom (y >= -w)
		m_Planes[3] = row3 - row1; // Top    (y <= w)
		m_Planes[4] = row2;        // Depth 0 (z >= 0): far plane with reversed Z
		m_Planes[5] = row3 - row2; // Depth 1 (z <= w): near plane with reversed Z

		for (glm::vec4& plane : m_Planes)
		{
			const float length = glm::length(glm::vec3(plane));
			if (length > 0.0f)
				plane /= length;
			else
				plane = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); // Degenerate plane (infinite far): never culls
		}
	}

	bool Frustum::IsPointVisible(const glm::vec3& point) const
	{
		for (const glm::vec4& plane : m_Planes)
		{
			if (glm::dot(glm::vec3(plane), point) + plane.w < 0.0f)
				return false;
		}
		return true;
	}

	bool Frustum::IsSphereVisible(const glm::vec3& center, float radius) const
	{
		for (const glm::vec4& plane : m_Planes)
		{
			if (glm::dot(glm::vec3(plane), center) + plane.w < -radius)
				return false;
		}
		return true;
	}

	bool Frustum::IsAABBVisible(const AABB& box) const
	{
		if (!box.IsValid())
			return false;

		for (const glm::vec4& plane : m_Planes)
		{
			// Test the box corner furthest along the plane normal (the "positive vertex").
			const glm::vec3 positive(
				plane.x >= 0.0f ? box.Max.x : box.Min.x,
				plane.y >= 0.0f ? box.Max.y : box.Min.y,
				plane.z >= 0.0f ? box.Max.z : box.Min.z);
			if (glm::dot(glm::vec3(plane), positive) + plane.w < 0.0f)
				return false;
		}
		return true;
	}

}
