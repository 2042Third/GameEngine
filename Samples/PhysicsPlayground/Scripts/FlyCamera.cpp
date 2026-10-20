// The playground's camera: WASD moves, Space up and Q down, Shift faster, the right mouse button looks around, and the
// keys 1 to 6 fly to the stations (entities "Station1" to "Station6" mark where to look from). It moves on unscaled time,
// so slow motion slows the bodies, not the camera.

#include "StrataScript/StrataScript.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace Strata;

class FlyCamera : public Script
{
public:
	float MoveSpeed = 8.0f;     // m/s
	float FastFactor = 3.0f;    // Holding Shift
	float LookSpeed = 0.15f;    // Degrees per pixel of mouse movement
	float Yaw = 0.0f;           // Degrees, kept across hot reloads
	float Pitch = -20.0f;
	float TravelTime = 0.6f;    // Seconds a jump to a station takes

	void OnCreate() override { ReadAngles(); }

	void OnUpdate(float) override
	{
		const float deltaTime = Time::GetUnscaledDeltaTime();
		TransformComponent transform = GetTransform();
		for (int32_t station = 0; station < 6; station++)
		{
			if (Input::IsKeyPressed(static_cast<KeyCode>(Key::D1 + station)))
				FlyTo(station + 1);
		}

		if (m_Travel < 1.0f)
		{
			m_Travel = std::min(1.0f, m_Travel + deltaTime / std::max(TravelTime, 0.01f));
			const float t = m_Travel * m_Travel * (3.0f - 2.0f * m_Travel);
			transform.SetWorldPosition(glm::mix(m_From, m_To, t));
			transform.SetWorldRotation(glm::slerp(m_FromRotation, m_ToRotation, t));
			if (m_Travel >= 1.0f)
				ReadAngles();
			return;
		}

		if (Input::IsMouseButtonDown(Mouse::ButtonRight))
		{
			const glm::vec2 delta = Input::GetMouseDelta();
			Yaw -= delta.x * LookSpeed;
			Pitch = std::clamp(Pitch - delta.y * LookSpeed, -89.0f, 89.0f);
		}
		const glm::quat rotation = glm::angleAxis(glm::radians(Yaw), glm::vec3(0.0f, 1.0f, 0.0f))
			* glm::angleAxis(glm::radians(Pitch), glm::vec3(1.0f, 0.0f, 0.0f));
		transform.SetWorldRotation(rotation);

		glm::vec3 move(0.0f);
		if (Input::IsKeyDown(Key::W)) move += transform.GetForward();
		if (Input::IsKeyDown(Key::S)) move -= transform.GetForward();
		if (Input::IsKeyDown(Key::D)) move += transform.GetRight();
		if (Input::IsKeyDown(Key::A)) move -= transform.GetRight();
		if (Input::IsKeyDown(Key::Space)) move.y += 1.0f;
		if (Input::IsKeyDown(Key::Q)) move.y -= 1.0f;
		if (glm::length(move) > 0.0f)
		{
			const float speed = MoveSpeed * (Input::IsKeyDown(Key::LeftShift) ? FastFactor : 1.0f);
			transform.SetWorldPosition(transform.GetWorldPosition() + glm::normalize(move) * speed * deltaTime);
		}
	}
private:
	void FlyTo(int32_t station)
	{
		Entity marker = Scene::FindEntityByName("Station" + std::to_string(station));
		if (!marker.IsValid())
			return;
		TransformComponent transform = GetTransform();
		m_From = transform.GetWorldPosition();
		m_FromRotation = transform.GetWorldRotation();
		m_To = marker.GetTransform().GetWorldPosition();
		m_ToRotation = marker.GetTransform().GetWorldRotation();
		m_Travel = 0.0f;
	}

	// Yaw and pitch of the camera's current rotation (after a jump, mouse look goes on from there).
	void ReadAngles()
	{
		const glm::vec3 forward = GetTransform().GetForward();
		Yaw = glm::degrees(std::atan2(-forward.x, -forward.z));
		Pitch = glm::degrees(std::asin(std::clamp(forward.y, -1.0f, 1.0f)));
	}
private:
	float m_Travel = 1.0f;
	glm::vec3 m_From = glm::vec3(0.0f);
	glm::vec3 m_To = glm::vec3(0.0f);
	glm::quat m_FromRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	glm::quat m_ToRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
};

ST_SCRIPT_CLASS(FlyCamera)
{
	ST_SCRIPT_FIELD(MoveSpeed);
	ST_SCRIPT_FIELD(FastFactor);
	ST_SCRIPT_FIELD(LookSpeed);
	ST_SCRIPT_FIELD(Yaw);
	ST_SCRIPT_FIELD(Pitch);
	ST_SCRIPT_FIELD(TravelTime);
}
