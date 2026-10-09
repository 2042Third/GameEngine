#pragma once

#include <glm/glm.hpp>

#include <limits>

namespace Strata
{

	// Axis-aligned bounding box. A default-constructed box is empty (invalid) and grows by expansion.
	struct AABB
	{
		glm::vec3 Min = glm::vec3(std::numeric_limits<float>::max());
		glm::vec3 Max = glm::vec3(std::numeric_limits<float>::lowest());

		AABB() = default;
		AABB(const glm::vec3& min, const glm::vec3& max)
			: Min(min), Max(max)
		{
		}

		bool IsValid() const { return Min.x <= Max.x && Min.y <= Max.y && Min.z <= Max.z; }
		glm::vec3 GetCenter() const { return (Min + Max) * 0.5f; }
		glm::vec3 GetExtents() const { return (Max - Min) * 0.5f; }
		glm::vec3 GetSize() const { return Max - Min; }

		void Expand(const glm::vec3& point)
		{
			Min = glm::min(Min, point);
			Max = glm::max(Max, point);
		}

		void Expand(const AABB& other)
		{
			if (!other.IsValid())
				return;
			Min = glm::min(Min, other.Min);
			Max = glm::max(Max, other.Max);
		}

		bool Contains(const glm::vec3& point) const
		{
			return glm::all(glm::greaterThanEqual(point, Min)) && glm::all(glm::lessThanEqual(point, Max));
		}

		bool Intersects(const AABB& other) const
		{
			return glm::all(glm::lessThanEqual(Min, other.Max)) && glm::all(glm::greaterThanEqual(Max, other.Min));
		}

		// Bounding box of this box after an affine transform (exact for the transformed box's extents).
		AABB Transformed(const glm::mat4& transform) const;
	};

}
