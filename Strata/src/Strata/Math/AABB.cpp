#include "stpch.h"
#include "Strata/Math/AABB.h"

namespace Strata
{

	AABB AABB::Transformed(const glm::mat4& transform) const
	{
		if (!IsValid())
			return AABB();

		// Arvo's method: transform the center, then accumulate the absolute basis contributions of the extents.
		const glm::vec3 center = GetCenter();
		const glm::vec3 extents = GetExtents();
		const glm::vec3 newCenter = glm::vec3(transform * glm::vec4(center, 1.0f));

		glm::vec3 newExtents(0.0f);
		for (int axis = 0; axis < 3; axis++)
		{
			newExtents[axis] = glm::abs(transform[0][axis]) * extents.x
				+ glm::abs(transform[1][axis]) * extents.y
				+ glm::abs(transform[2][axis]) * extents.z;
		}
		return AABB(newCenter - newExtents, newCenter + newExtents);
	}

}
