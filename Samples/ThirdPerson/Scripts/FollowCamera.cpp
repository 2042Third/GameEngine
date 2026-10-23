// The third-person camera, on the course's camera: it orbits its target (the player) with the mouse (the cursor is
// locked while the course plays) or the Look action (the right stick), zooms with the scroll wheel, and shortens its arm
// where a wall or an island would come between it and the player (a ray from the player's head). The settings' mouse
// sensitivity and invert-Y apply. In the attract mode it swings behind the walking player by itself.

#include "ThirdPerson.h"

#include <algorithm>
#include <cmath>
#include <optional>

using namespace Strata;

class FollowCamera : public Script
{
public:
	Entity Target;
	float Distance = 6.5f;           // Meters from the pivot (the target's head)
	float MinDistance = 2.5f;
	float MaxDistance = 12.0f;
	float ZoomStep = 0.75f;          // Meters per scroll notch
	float Yaw = 0.0f;                // Degrees around +Y, 0: looking along -Z (kept across hot reloads)
	float Pitch = -18.0f;            // Degrees, negative looks down
	float MinPitch = -70.0f;
	float MaxPitch = 35.0f;
	float MouseDegreesPerPixel = 0.12f;
	float StickDegreesPerSecond = 160.0f;
	float PivotHeight = 1.5f;        // Above the target's feet
	float FollowSharpness = 14.0f;   // How quickly the pivot follows the target (1/s)
	float WallMargin = 0.3f;         // Meters kept between the camera and what its arm hit
	float ArmLength = 0.0f;          // The arm's length after walls shortened it

	void OnCreate() override
	{
		if (Target.IsValid())
			m_Pivot = Target.GetTransform().GetWorldPosition() + glm::vec3(0.0f, PivotHeight, 0.0f);
		Place();
	}

	void OnLateUpdate(float) override
	{
		const float deltaTime = Time::GetUnscaledDeltaTime();
		const bool paused = Time::GetTimeScale() <= 0.0f;
		if (!paused)
		{
			const ThirdPerson::GameSettings& settings = ThirdPerson::GetSettings();
			const float invert = settings.InvertY ? -1.0f : 1.0f;
			if (Input::GetCursorMode() == CursorMode::Locked || Input::IsMouseButtonDown(Mouse::ButtonRight))
			{
				const glm::vec2 delta = Input::GetMouseDelta();
				Yaw -= delta.x * MouseDegreesPerPixel * settings.MouseSensitivity;
				Pitch -= delta.y * MouseDegreesPerPixel * settings.MouseSensitivity * invert;
			}
			// The project's Look action (the right stick, +Y up): pushed up it looks up, as the mouse moved up does.
			const glm::vec2 look = Input::GetActionValue("Gameplay/Look");
			Yaw -= look.x * StickDegreesPerSecond * deltaTime;
			Pitch += look.y * StickDegreesPerSecond * deltaTime * invert;
			Pitch = std::clamp(Pitch, MinPitch, MaxPitch);
			Distance = std::clamp(Distance - Input::GetScrollDelta().y * ZoomStep, MinDistance, MaxDistance);
			if (ThirdPerson::IsAttractMode() && Target.IsValid())
			{
				// Behind the player's back, slowly (the attract mode's demo has nobody at the mouse).
				const glm::vec3 forward = Target.GetTransform().GetForward();
				const float behind = glm::degrees(std::atan2(-forward.x, -forward.z));
				Yaw += std::remainder(behind - Yaw, 360.0f) * std::min(1.0f, deltaTime * 1.5f);
			}
		}
		if (Target.IsValid())
		{
			const glm::vec3 pivot = Target.GetTransform().GetWorldPosition() + glm::vec3(0.0f, PivotHeight, 0.0f);
			// A respawn or a teleport moves the camera at once; walking is followed smoothly.
			if (glm::length(pivot - m_Pivot) > 8.0f)
				m_Pivot = pivot;
			else
				m_Pivot = glm::mix(m_Pivot, pivot, 1.0f - std::exp(-FollowSharpness * deltaTime));
		}
		Place();
	}
private:
	void Place()
	{
		const glm::quat rotation = glm::angleAxis(glm::radians(Yaw), glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(glm::radians(Pitch), glm::vec3(1.0f, 0.0f, 0.0f));
		const glm::vec3 back = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
		ArmLength = Distance;
		if (const std::optional<RaycastHit> hit = Physics::Raycast(m_Pivot, back, Distance + WallMargin, Physics::c_AllLayers, Target))
			ArmLength = std::max(0.4f, hit->Distance - WallMargin);
		TransformComponent transform = GetTransform();
		transform.SetWorldPosition(m_Pivot + back * ArmLength);
		transform.SetWorldRotation(rotation);
	}
private:
	glm::vec3 m_Pivot = glm::vec3(0.0f);
};

ST_SCRIPT_CLASS(FollowCamera)
{
	ST_SCRIPT_FIELD(Target);
	ST_SCRIPT_FIELD(Distance);
	ST_SCRIPT_FIELD(MinDistance);
	ST_SCRIPT_FIELD(MaxDistance);
	ST_SCRIPT_FIELD(ZoomStep);
	ST_SCRIPT_FIELD(Yaw);
	ST_SCRIPT_FIELD(Pitch);
	ST_SCRIPT_FIELD(MinPitch);
	ST_SCRIPT_FIELD(MaxPitch);
	ST_SCRIPT_FIELD(MouseDegreesPerPixel);
	ST_SCRIPT_FIELD(StickDegreesPerSecond);
	ST_SCRIPT_FIELD(PivotHeight);
	ST_SCRIPT_FIELD(FollowSharpness);
	ST_SCRIPT_FIELD(WallMargin);
	ST_SCRIPT_FIELD(ArmLength);
}
