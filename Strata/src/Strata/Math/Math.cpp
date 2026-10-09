#include "stpch.h"
#include "Strata/Math/Math.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Strata::Math
{

	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outTranslation, glm::quat& outRotation, glm::vec3& outScale)
	{
		// Outputs are only written on success, so callers keep their previous values for degenerate input.
		for (int column = 0; column < 4; column++)
		{
			for (int row = 0; row < 4; row++)
			{
				if (!std::isfinite(transform[column][row]))
					return false;
			}
		}

		glm::mat4 localMatrix(transform);
		if (glm::abs(localMatrix[3][3]) < glm::epsilon<float>())
			return false;

		// Normalize so the homogeneous coordinate is 1. Projective components are ignored.
		if (glm::abs(localMatrix[3][3] - 1.0f) > glm::epsilon<float>())
			localMatrix /= localMatrix[3][3];

		const glm::vec3 translation = glm::vec3(localMatrix[3]);
		glm::vec3 columns[3] = { glm::vec3(localMatrix[0]), glm::vec3(localMatrix[1]), glm::vec3(localMatrix[2]) };
		glm::vec3 scale(glm::length(columns[0]), glm::length(columns[1]), glm::length(columns[2]));
		if (scale.x < glm::epsilon<float>() || scale.y < glm::epsilon<float>() || scale.z < glm::epsilon<float>())
			return false;

		columns[0] /= scale.x;
		columns[1] /= scale.y;
		columns[2] /= scale.z;

		// A left-handed basis means an odd number of negative scale axes; fold the reflection into X.
		if (glm::dot(columns[0], glm::cross(columns[1], columns[2])) < 0.0f)
		{
			scale.x = -scale.x;
			columns[0] = -columns[0];
		}

		const glm::quat rotation = glm::normalize(glm::quat_cast(glm::mat3(columns[0], columns[1], columns[2])));
		if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w))
			return false;

		outTranslation = translation;
		outRotation = rotation;
		outScale = scale;
		return true;
	}

	glm::mat4 ComposeTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale)
	{
		glm::mat4 result = glm::mat4_cast(rotation);
		result[0] *= scale.x;
		result[1] *= scale.y;
		result[2] *= scale.z;
		result[3] = glm::vec4(translation, 1.0f);
		return result;
	}

	glm::mat4 PerspectiveReverseZ(float verticalFov, float aspectRatio, float nearClip, float farClip)
	{
		// A standard [0, 1] depth projection with near and far swapped maps near -> 1 and far -> 0.
		return glm::perspectiveRH_ZO(verticalFov, aspectRatio, farClip, nearClip);
	}

	glm::mat4 PerspectiveReverseZInfinite(float verticalFov, float aspectRatio, float nearClip)
	{
		const float focalLength = 1.0f / glm::tan(verticalFov * 0.5f);
		glm::mat4 result(0.0f);
		result[0][0] = focalLength / aspectRatio;
		result[1][1] = focalLength;
		result[2][3] = -1.0f;
		result[3][2] = nearClip; // depth = near / -z_view
		return result;
	}

	glm::mat4 OrthographicReverseZ(float left, float right, float bottom, float top, float nearClip, float farClip)
	{
		return glm::orthoRH_ZO(left, right, bottom, top, farClip, nearClip);
	}

	glm::quat EulerDegreesToQuat(const glm::vec3& eulerDegrees)
	{
		return glm::quat(glm::radians(eulerDegrees));
	}

	glm::vec3 QuatToEulerDegrees(const glm::quat& rotation)
	{
		return glm::degrees(glm::eulerAngles(rotation));
	}

	glm::quat LookRotation(const glm::vec3& direction, const glm::vec3& up)
	{
		const float length = glm::length(direction);
		if (length < glm::epsilon<float>())
			return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);

		const glm::vec3 forward = direction / length;
		glm::vec3 upHint = up;
		// Fall back to another up axis when looking (almost) straight along the hint.
		if (glm::abs(glm::dot(forward, glm::normalize(upHint))) > 0.9999f)
			upHint = glm::abs(forward.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(0.0f, 0.0f, 1.0f);

		const glm::vec3 right = glm::normalize(glm::cross(forward, upHint));
		const glm::vec3 orthogonalUp = glm::cross(right, forward);
		// Basis columns: +X = right, +Y = up, +Z = -forward.
		return glm::normalize(glm::quat_cast(glm::mat3(right, orthogonalUp, -forward)));
	}

}
