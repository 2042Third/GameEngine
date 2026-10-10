#include "Editor/EditorCamera.h"

#include <Strata/Core/Log.h>
#include <Strata/Math/Math.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Strata
{

	namespace
	{

		bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		// Keeps yaw in [-180, 180) so it never loses precision after many turns. Values in range stay exactly as they are.
		float WrapDegrees(float degrees)
		{
			if (degrees >= -180.0f && degrees < 180.0f)
				return degrees;
			const float wrapped = std::fmod(degrees + 180.0f, 360.0f);
			return (wrapped < 0.0f ? wrapped + 360.0f : wrapped) - 180.0f;
		}

	}

	void EditorCamera::Reset()
	{
		*this = EditorCamera();
	}

	////////////////////////////////////////////////////////////////////////////////
	// Controls
	////////////////////////////////////////////////////////////////////////////////

	void EditorCamera::Update(const EditorCameraInput& input, float timestep, const glm::vec2& viewportSize)
	{
		if (input.Fly)
		{
			Look(input.MouseDelta);
			AdjustFlySpeed(input.Scroll);
			const glm::vec3 direction(
				(input.MoveRight ? 1.0f : 0.0f) - (input.MoveLeft ? 1.0f : 0.0f),
				(input.MoveUp ? 1.0f : 0.0f) - (input.MoveDown ? 1.0f : 0.0f),
				(input.MoveForward ? 1.0f : 0.0f) - (input.MoveBackward ? 1.0f : 0.0f));
			if (glm::dot(direction, direction) > 0.0f && timestep > 0.0f)
			{
				// Diagonal movement is as fast as straight movement.
				const float speed = m_FlySpeed * (input.Fast ? c_FastFlyMultiplier : 1.0f);
				Move(glm::normalize(direction), speed * timestep);
			}
			return;
		}

		if (input.Orbit)
			Orbit(input.MouseDelta);
		else if (input.Pan)
			Pan(input.MouseDelta, viewportSize.y);
		if (input.Scroll != 0.0f)
			Dolly(input.Scroll);
	}

	void EditorCamera::Orbit(const glm::vec2& deltaPixels)
	{
		// Dragging right turns the view to the right around the target, so the scene follows the mouse.
		SetOrientation(m_Yaw - deltaPixels.x * c_RotationPerPixel, m_Pitch - deltaPixels.y * c_RotationPerPixel);
	}

	void EditorCamera::Look(const glm::vec2& deltaPixels)
	{
		const glm::vec3 position = GetPosition();
		m_Yaw = WrapDegrees(m_Yaw - deltaPixels.x * c_RotationPerPixel);
		m_Pitch = std::clamp(m_Pitch - deltaPixels.y * c_RotationPerPixel, -c_MaxPitch, c_MaxPitch);
		m_Target = position + GetForward() * m_Distance;
	}

	void EditorCamera::Pan(const glm::vec2& deltaPixels, float viewportHeight)
	{
		if (!(viewportHeight > 0.0f))
			return;
		// World units per pixel at the target's depth.
		const float unitsPerPixel = 2.0f * m_Distance * std::tan(glm::radians(m_FOV) * 0.5f) / viewportHeight;
		SetTarget(m_Target - GetRight() * (deltaPixels.x * unitsPerPixel) + GetUp() * (deltaPixels.y * unitsPerPixel));
	}

	void EditorCamera::Dolly(float steps)
	{
		if (!std::isfinite(steps))
			return;
		// In double precision and clamped there: many steps overflow to infinity, which clamps to the limit.
		const double distance = static_cast<double>(m_Distance) * std::pow(static_cast<double>(c_DollyPerStep), static_cast<double>(steps));
		m_Distance = static_cast<float>(std::clamp(distance, static_cast<double>(c_MinDistance), static_cast<double>(c_MaxDistance)));
	}

	void EditorCamera::Move(const glm::vec3& localDirection, float distance)
	{
		const glm::vec3 offset = (GetRight() * localDirection.x + glm::vec3(0.0f, 1.0f, 0.0f) * localDirection.y + GetForward() * localDirection.z) * distance;
		if (IsFinite(offset))
			SetTarget(m_Target + offset);
	}

	void EditorCamera::AdjustFlySpeed(float steps)
	{
		if (steps == 0.0f || !std::isfinite(steps))
			return;
		const double speed = static_cast<double>(m_FlySpeed) * std::pow(static_cast<double>(c_FlySpeedPerStep), static_cast<double>(steps));
		m_FlySpeed = static_cast<float>(std::clamp(speed, static_cast<double>(c_MinFlySpeed), static_cast<double>(c_MaxFlySpeed)));
	}

	bool EditorCamera::Focus(const AABB& bounds, float aspectRatio)
	{
		if (!bounds.IsValid() || !IsFinite(bounds.Min) || !IsFinite(bounds.Max))
			return false;
		// The bounding sphere fits inside the narrower of the vertical and horizontal fields of view.
		const float radius = std::max(glm::length(bounds.GetExtents()), c_MinFocusRadius);
		const float halfVertical = glm::radians(m_FOV) * 0.5f;
		const float aspect = aspectRatio > 0.0f && std::isfinite(aspectRatio) ? aspectRatio : 1.0f;
		const float halfHorizontal = std::atan(std::tan(halfVertical) * aspect);
		const float halfAngle = std::min(halfVertical, halfHorizontal);
		SetTarget(bounds.GetCenter());
		SetDistance(radius * c_FocusMargin / std::sin(halfAngle));
		// Large bounds would end beyond the far plane: it moves out so everything framed stays visible.
		const float farthest = m_Distance + radius;
		if (farthest > m_Far)
			m_Far = std::min(farthest * c_FocusMargin, c_MaxFar);
		return true;
	}

	bool EditorCamera::FitBounds(const AABB& bounds, float aspectRatio)
	{
		if (!bounds.IsValid() || !IsFinite(bounds.Min) || !IsFinite(bounds.Max))
			return false;
		const float aspect = aspectRatio > 0.0f && std::isfinite(aspectRatio) ? aspectRatio : 1.0f;
		const float tanVertical = std::tan(glm::radians(m_FOV) * 0.5f);
		const float tanHorizontal = tanVertical * aspect;
		const glm::vec3 center = bounds.GetCenter();
		const glm::vec3 forward = GetForward();
		const glm::vec3 right = GetRight();
		const glm::vec3 up = GetUp();

		// A corner at (x, y) across the view and z along it (from the center) is inside the view, with the margin, once
		// the camera is far enough from the center: |x| <= tanHorizontal * (distance + z) / margin, likewise for y. It must
		// also stay beyond the near plane.
		float distance = c_MinFocusRadius * c_FocusMargin / std::min(tanVertical, tanHorizontal);
		float farthest = 0.0f;
		for (int corner = 0; corner < 8; corner++)
		{
			const glm::vec3 point((corner & 1) ? bounds.Max.x : bounds.Min.x, (corner & 2) ? bounds.Max.y : bounds.Min.y, (corner & 4) ? bounds.Max.z : bounds.Min.z);
			const glm::vec3 offset = point - center;
			const float x = std::abs(glm::dot(offset, right));
			const float y = std::abs(glm::dot(offset, up));
			const float z = glm::dot(offset, forward);
			distance = std::max({ distance, c_FocusMargin * x / tanHorizontal - z, c_FocusMargin * y / tanVertical - z, m_Near * 2.0f - z });
			farthest = std::max(farthest, z);
		}
		if (!std::isfinite(distance))
			return false;
		SetTarget(center);
		SetDistance(distance);
		if (m_Distance + farthest > m_Far)
			m_Far = std::min((m_Distance + farthest) * c_FocusMargin, c_MaxFar);
		return true;
	}

	bool EditorCamera::LookAt(const glm::vec3& position, const glm::vec3& target)
	{
		const glm::vec3 direction = target - position;
		const float distance = glm::length(direction);
		if (!IsFinite(position) || !IsFinite(target) || !std::isfinite(distance) || distance < c_MinDistance || distance > c_MaxDistance)
			return false;
		m_Yaw = WrapDegrees(glm::degrees(std::atan2(-direction.x, -direction.z)));
		m_Pitch = std::clamp(glm::degrees(std::asin(std::clamp(direction.y / distance, -1.0f, 1.0f))), -c_MaxPitch, c_MaxPitch);
		m_Target = glm::clamp(target, glm::vec3(-c_MaxCoordinate), glm::vec3(c_MaxCoordinate));
		SetDistance(distance);
		return true;
	}

	void EditorCamera::SetTarget(const glm::vec3& target)
	{
		if (IsFinite(target))
			m_Target = glm::clamp(target, glm::vec3(-c_MaxCoordinate), glm::vec3(c_MaxCoordinate));
	}

	void EditorCamera::SetPosition(const glm::vec3& position)
	{
		if (IsFinite(position))
			SetTarget(position + GetForward() * m_Distance);
	}

	void EditorCamera::SetOrientation(float yawDegrees, float pitchDegrees)
	{
		if (!std::isfinite(yawDegrees) || !std::isfinite(pitchDegrees))
			return;
		m_Yaw = WrapDegrees(yawDegrees);
		m_Pitch = std::clamp(pitchDegrees, -c_MaxPitch, c_MaxPitch);
	}

	void EditorCamera::SetDistance(float distance)
	{
		if (std::isfinite(distance))
			m_Distance = std::clamp(distance, c_MinDistance, c_MaxDistance);
	}

	void EditorCamera::SetFOV(float degrees)
	{
		if (std::isfinite(degrees))
			m_FOV = std::clamp(degrees, c_MinFOV, c_MaxFOV);
	}

	bool EditorCamera::SetClipPlanes(float nearClip, float farClip)
	{
		if (!std::isfinite(nearClip) || !std::isfinite(farClip) || nearClip < c_MinNear || farClip > c_MaxFar || !(nearClip < farClip))
			return false;
		m_Near = nearClip;
		m_Far = farClip;
		return true;
	}

	void EditorCamera::SetFlySpeed(float speed)
	{
		if (std::isfinite(speed))
			m_FlySpeed = std::clamp(speed, c_MinFlySpeed, c_MaxFlySpeed);
	}

	////////////////////////////////////////////////////////////////////////////////
	// View
	////////////////////////////////////////////////////////////////////////////////

	glm::quat EditorCamera::GetOrientation() const
	{
		return glm::angleAxis(glm::radians(m_Yaw), glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(glm::radians(m_Pitch), glm::vec3(1.0f, 0.0f, 0.0f));
	}

	glm::vec3 EditorCamera::GetForward() const
	{
		return Math::GetForwardDirection(GetOrientation());
	}

	glm::vec3 EditorCamera::GetRight() const
	{
		return Math::GetRightDirection(GetOrientation());
	}

	glm::vec3 EditorCamera::GetUp() const
	{
		return Math::GetUpDirection(GetOrientation());
	}

	glm::vec3 EditorCamera::GetPosition() const
	{
		return m_Target - GetForward() * m_Distance;
	}

	glm::mat4 EditorCamera::GetViewMatrix() const
	{
		// Inverse of the camera's world transform (a rotation followed by a translation).
		return glm::mat4_cast(glm::conjugate(GetOrientation())) * glm::translate(glm::mat4(1.0f), -GetPosition());
	}

	glm::mat4 EditorCamera::GetProjectionMatrix(float aspectRatio) const
	{
		const float aspect = aspectRatio > 0.0f && std::isfinite(aspectRatio) ? aspectRatio : 1.0f;
		return Math::PerspectiveReverseZ(glm::radians(m_FOV), aspect, m_Near, m_Far);
	}

	SceneCamera EditorCamera::GetSceneCamera(float aspectRatio) const
	{
		SceneCamera camera;
		camera.View = GetViewMatrix();
		camera.Projection = GetProjectionMatrix(aspectRatio);
		camera.Position = GetPosition();
		camera.Near = m_Near;
		camera.Far = m_Far;
		camera.VerticalFOV = glm::radians(m_FOV);
		camera.Orthographic = false;
		return camera;
	}

	////////////////////////////////////////////////////////////////////////////////
	// Persistence
	////////////////////////////////////////////////////////////////////////////////

	nlohmann::json EditorCamera::ToJson() const
	{
		return {
			{ "Target", { m_Target.x, m_Target.y, m_Target.z } },
			{ "Distance", m_Distance },
			{ "Yaw", m_Yaw },
			{ "Pitch", m_Pitch },
			{ "FOV", m_FOV },
			{ "Near", m_Near },
			{ "Far", m_Far },
			{ "FlySpeed", m_FlySpeed }
		};
	}

	bool EditorCamera::FromJson(const nlohmann::json& json, std::string* outError)
	{
		auto fail = [outError](std::string message)
		{
			if (outError)
				*outError = std::move(message);
			return false;
		};
		if (!json.is_object())
			return fail("the camera must be an object");

		// Every value is checked before anything changes.
		EditorCamera result = *this;
		auto readNumber = [&json](const char* key, float minimum, float maximum, float& outValue, std::string& outProblem)
		{
			const auto it = json.find(key);
			if (it == json.end())
				return true;
			// Read as double: converting a number beyond the float range to float is undefined.
			const double value = it->is_number() ? it->get<double>() : std::numeric_limits<double>::quiet_NaN();
			if (!std::isfinite(value) || value < minimum || value > maximum)
			{
				outProblem = fmt::format("'{}' must be a number between {} and {}", key, minimum, maximum);
				return false;
			}
			outValue = static_cast<float>(value);
			return true;
		};

		std::string problem;
		if (const auto target = json.find("Target"); target != json.end())
		{
			if (!target->is_array() || target->size() != 3 || !(*target)[0].is_number() || !(*target)[1].is_number() || !(*target)[2].is_number())
				return fail("'Target' must be an array of three numbers");
			const glm::dvec3 value((*target)[0].get<double>(), (*target)[1].get<double>(), (*target)[2].get<double>());
			if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)
				|| glm::any(glm::greaterThan(glm::abs(value), glm::dvec3(c_MaxCoordinate))))
			{
				return fail(fmt::format("'Target' must be finite and within +-{}", c_MaxCoordinate));
			}
			result.m_Target = glm::vec3(value);
		}
		float nearClip = result.m_Near;
		float farClip = result.m_Far;
		if (!readNumber("Distance", c_MinDistance, c_MaxDistance, result.m_Distance, problem)
			|| !readNumber("Yaw", -1.0e6f, 1.0e6f, result.m_Yaw, problem)
			|| !readNumber("Pitch", -c_MaxPitch, c_MaxPitch, result.m_Pitch, problem)
			|| !readNumber("FOV", c_MinFOV, c_MaxFOV, result.m_FOV, problem)
			|| !readNumber("Near", c_MinNear, c_MaxFar, nearClip, problem)
			|| !readNumber("Far", c_MinNear, c_MaxFar, farClip, problem)
			|| !readNumber("FlySpeed", c_MinFlySpeed, c_MaxFlySpeed, result.m_FlySpeed, problem))
		{
			return fail(problem);
		}
		if (!result.SetClipPlanes(nearClip, farClip))
			return fail(fmt::format("'Near' ({}) must be below 'Far' ({})", nearClip, farClip));
		result.m_Yaw = WrapDegrees(result.m_Yaw);
		*this = result;
		return true;
	}

}
