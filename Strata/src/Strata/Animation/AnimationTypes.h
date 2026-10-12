#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <cstdint>
#include <span>

namespace Strata
{

	// A joint's transform relative to its parent joint (or, for root joints, to the animator's entity): translation,
	// rotation (unit quaternion) and scale, applied scale first, as TransformComponent.
	struct JointPose
	{
		glm::vec3 Translation = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 Scale = glm::vec3(1.0f);

		bool operator==(const JointPose&) const = default;
	};

	// Index of a joint in its skeleton; c_NoJoint where there is none (the parent of a root joint).
	using JointIndex = uint16_t;
	constexpr JointIndex c_NoJoint = 0xFFFF;

	namespace AnimationMath
	{

		// The quaternion of unit length closest to q; identity for a quaternion of (nearly) zero or non-finite length.
		inline glm::quat Normalize(const glm::quat& q)
		{
			const float lengthSquared = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
			if (!(lengthSquared > 1.0e-20f) || !std::isfinite(lengthSquared))
				return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			const float inverse = 1.0f / std::sqrt(lengthSquared);
			return glm::quat(q.w * inverse, q.x * inverse, q.y * inverse, q.z * inverse);
		}

		// Spherical linear interpolation along the shorter arc; normalized linear interpolation where the arc is too
		// short for the slerp formula to be accurate. The result has unit length.
		inline glm::quat Slerp(const glm::quat& a, glm::quat b, float t)
		{
			float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
			if (cosine < 0.0f)
			{
				b = -b;
				cosine = -cosine;
			}
			if (cosine > 0.9995f)
				return Normalize(glm::quat(a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t));
			const float angle = std::acos(cosine);
			const float sine = std::sin(angle);
			const float weightA = std::sin((1.0f - t) * angle) / sine;
			const float weightB = std::sin(t * angle) / sine;
			return Normalize(glm::quat(a.w * weightA + b.w * weightB, a.x * weightA + b.x * weightB, a.y * weightA + b.y * weightB, a.z * weightA + b.z * weightB));
		}

		// The joint's transform as a matrix (translation * rotation * scale).
		inline glm::mat4 ToMatrix(const JointPose& pose)
		{
			glm::mat4 matrix = glm::mat4_cast(pose.Rotation);
			matrix[0] *= pose.Scale.x;
			matrix[1] *= pose.Scale.y;
			matrix[2] *= pose.Scale.z;
			matrix[3] = glm::vec4(pose.Translation, 1.0f);
			return matrix;
		}

		// The product of two affine matrices (fourth rows 0, 0, 0, 1), without the work of the fourth row.
		inline glm::mat4 MultiplyAffine(const glm::mat4& a, const glm::mat4& b)
		{
			glm::mat4 result;
			for (int column = 0; column < 4; column++)
			{
				const glm::vec4& source = b[column];
				result[column] = a[0] * source.x + a[1] * source.y + a[2] * source.z + (column == 3 ? a[3] : glm::vec4(0.0f));
			}
			result[0].w = 0.0f;
			result[1].w = 0.0f;
			result[2].w = 0.0f;
			result[3].w = 1.0f;
			return result;
		}

	}

}
