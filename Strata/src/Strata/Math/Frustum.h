#pragma once

#include "Strata/Math/AABB.h"

#include <glm/glm.hpp>

#include <array>

namespace Strata
{

	// View frustum as six inward-facing planes (xyz = normal, w = distance), extracted from a
	// view-projection matrix in Strata's clip-space convention (depth [0, 1], either depth direction).
	// For infinite projections the far plane degenerates to "always inside".
	class Frustum
	{
	public:
		Frustum() = default;
		explicit Frustum(const glm::mat4& viewProjection);

		void Update(const glm::mat4& viewProjection);

		bool IsPointVisible(const glm::vec3& point) const;
		bool IsSphereVisible(const glm::vec3& center, float radius) const;
		bool IsAABBVisible(const AABB& box) const;

		const std::array<glm::vec4, 6>& GetPlanes() const { return m_Planes; }
	private:
		std::array<glm::vec4, 6> m_Planes = {};
	};

}
