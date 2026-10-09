#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Strata::Math
{

	// Engine conventions: right-handed coordinates, +Y up, -Z forward (glTF). Clip space follows the
	// D3D convention NVRHI exposes on every backend: NDC +Y up, depth in [0, 1], and Strata uses
	// reversed Z, so the near plane maps to depth 1 and the far plane (or infinity) to depth 0.

	// Splits a transform into translation, rotation and scale. Returns false (leaving the outputs untouched) for
	// degenerate or non-finite matrices.
	bool DecomposeTransform(const glm::mat4& transform, glm::vec3& outTranslation, glm::quat& outRotation, glm::vec3& outScale);
	glm::mat4 ComposeTransform(const glm::vec3& translation, const glm::quat& rotation, const glm::vec3& scale);

	// Reversed-Z perspective projection. verticalFov is in radians.
	glm::mat4 PerspectiveReverseZ(float verticalFov, float aspectRatio, float nearClip, float farClip);
	// Reversed-Z perspective projection with the far plane at infinity (best depth precision).
	glm::mat4 PerspectiveReverseZInfinite(float verticalFov, float aspectRatio, float nearClip);
	// Reversed-Z orthographic projection; nearClip/farClip are distances along -Z in view space.
	glm::mat4 OrthographicReverseZ(float left, float right, float bottom, float top, float nearClip, float farClip);

	// Euler angles in degrees (pitch about X, yaw about Y, roll about Z), matching glm's quaternion conversions.
	glm::quat EulerDegreesToQuat(const glm::vec3& eulerDegrees);
	glm::vec3 QuatToEulerDegrees(const glm::quat& rotation);

	inline glm::vec3 GetForwardDirection(const glm::quat& rotation) { return rotation * glm::vec3(0.0f, 0.0f, -1.0f); }
	inline glm::vec3 GetRightDirection(const glm::quat& rotation) { return rotation * glm::vec3(1.0f, 0.0f, 0.0f); }
	inline glm::vec3 GetUpDirection(const glm::quat& rotation) { return rotation * glm::vec3(0.0f, 1.0f, 0.0f); }

	// Rotation that makes -Z point along direction (normalized internally), keeping `up` as close to +Y as possible.
	glm::quat LookRotation(const glm::vec3& direction, const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f));

	inline bool IsNearlyEqual(float a, float b, float epsilon = 1e-5f)
	{
		return glm::abs(a - b) <= epsilon;
	}

	inline bool IsNearlyEqual(const glm::vec3& a, const glm::vec3& b, float epsilon = 1e-5f)
	{
		return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(epsilon)));
	}

	// Quaternions q and -q describe the same rotation.
	inline bool IsNearlyEqual(const glm::quat& a, const glm::quat& b, float epsilon = 1e-5f)
	{
		return glm::abs(glm::abs(glm::dot(a, b)) - 1.0f) <= epsilon;
	}

}
