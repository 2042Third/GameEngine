// Outpost's walk and flight. The walk moves the body every frame (smooth at any frame rate): a ray along the step slides it
// along walls, a ray down puts it on the ground when the rise is a step's at most. The flight moves the camera freely, at
// least 0.3 m above the ground. Bookmarks fly to the tour's keys and the rock macro.

#include "Outpost.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

using namespace Strata;
using namespace Showcase;

namespace
{

	constexpr float c_StepHeight = 0.45f;    // The highest rise a step of the walk climbs
	constexpr float c_WallMargin = 0.35f;    // How far ahead of the body the walk looks for walls
	constexpr float c_FlightClearance = 0.3f; // The flight keeps the eye at least this high above the ground
	const glm::vec3 c_Up(0.0f, 1.0f, 0.0f);

}

glm::vec3 Outpost::GetEyePosition() const
{
	return Eye.IsValid() ? Eye.GetTransform().GetWorldPosition() : glm::vec3(0.0f);
}

void Outpost::SetMode(int32_t mode)
{
	const int32_t previous = Mode;
	Mode = mode;
	if (mode == 1 && previous != 1)
		DropToGround();
	// Mouse look with a locked cursor while walking; the flight looks while the right button is held, the photo mode
	// needs the pointer.
	Input::SetCursorMode(mode == 1 && !PhotoOpen ? CursorMode::Locked : CursorMode::Normal);
	if (mode != previous && mode == 1)
		Log::Info("Showcase: walking");
	else if (mode != previous && mode == 2)
		Log::Info("Showcase: flying");
}

void Outpost::ApplyLook()
{
	if (Player.IsValid())
		Player.GetTransform().SetWorldRotation(glm::angleAxis(glm::radians(m_Yaw), c_Up));
	if (Eye.IsValid())
		Eye.GetTransform().SetRotation(glm::angleAxis(glm::radians(m_Pitch), glm::vec3(1.0f, 0.0f, 0.0f)));
}

void Outpost::UpdateLook(float realDeltaTime, bool mouse)
{
	if (mouse)
	{
		const glm::vec2 delta = Input::GetMouseDelta();
		m_Yaw -= delta.x * LookSpeed;
		m_Pitch -= delta.y * LookSpeed;
	}
	const glm::vec2 stick = Input::GetActionValue("Gameplay/Look");
	m_Yaw -= stick.x * StickTurnSpeed * realDeltaTime;
	m_Pitch += stick.y * StickTurnSpeed * realDeltaTime;
	m_Yaw = std::fmod(m_Yaw, 360.0f);
	m_Pitch = std::clamp(m_Pitch, -85.0f, 85.0f);
	ApplyLook();
}

void Outpost::DropToGround()
{
	if (!Player.IsValid())
		return;
	// The body under the eye, on whatever the eye stands over (a ray from 300 m up).
	const glm::vec3 eye = GetEyePosition();
	if (const std::optional<RaycastHit> ground = Physics::Raycast(glm::vec3(eye.x, eye.y + 300.0f, eye.z), -c_Up, 900.0f, m_GroundMask))
		Player.GetTransform().SetWorldPosition(glm::vec3(eye.x, ground->Point.y, eye.z));
}

void Outpost::UpdateWalk(float realDeltaTime)
{
	if (!Player.IsValid())
		return;
	UpdateLook(realDeltaTime, !PhotoOpen);
	glm::vec2 move = Input::GetActionValue("Gameplay/Move");
	if (glm::length(move) > 1.0f)
		move = glm::normalize(move);
	const float speed = Input::IsActionDown("Gameplay/Run") ? RunSpeed : WalkSpeed;
	const float yaw = glm::radians(m_Yaw);
	const glm::vec3 forward(-std::sin(yaw), 0.0f, -std::cos(yaw));
	const glm::vec3 right(std::cos(yaw), 0.0f, -std::sin(yaw));
	glm::vec3 step = (forward * move.y + right * move.x) * speed * realDeltaTime;
	const glm::vec3 body = Player.GetTransform().GetWorldPosition();

	// Walls: slide along the first one in the way (along its normal's horizontal part), stop at a second.
	for (int32_t attempt = 0; attempt < 2 && glm::length(step) > 1e-5f; attempt++)
	{
		const float length = glm::length(step);
		const std::optional<RaycastHit> wall = Physics::Raycast(body + glm::vec3(0.0f, 0.5f, 0.0f), step / length, length + c_WallMargin, m_GroundMask);
		if (!wall)
			break;
		glm::vec3 normal(wall->Normal.x, 0.0f, wall->Normal.z);
		if (attempt == 1 || glm::length(normal) < 1e-4f)
		{
			step = glm::vec3(0.0f);
			break;
		}
		normal = glm::normalize(normal);
		step -= normal * glm::dot(step, normal);
	}

	glm::vec3 target = body + step;
	const glm::vec2 flat(target.x, target.z);
	if (glm::length(flat) > WalkRadius)
	{
		const glm::vec2 kept = glm::normalize(flat) * WalkRadius;
		target.x = kept.x;
		target.z = kept.y;
	}
	// The ground under the target: climbed when it rises a step at most, else the body stays.
	const std::optional<RaycastHit> ground = Physics::Raycast(target + glm::vec3(0.0f, 1.0f, 0.0f), -c_Up, 3.0f, m_GroundMask);
	Grounded = ground.has_value();
	if (ground && ground->Point.y - body.y <= c_StepHeight)
	{
		const glm::vec3 next(target.x, ground->Point.y, target.z);
		DistanceWalked += glm::length(glm::vec2(next.x - body.x, next.z - body.z));
		if (next != body)
			Player.GetTransform().SetWorldPosition(next);
	}
}

void Outpost::UpdateFlight(float realDeltaTime)
{
	if (!Player.IsValid())
		return;
	// Mouse look while the right button is held (the pointer stays free), and the right stick.
	UpdateLook(realDeltaTime, Input::IsMouseButtonDown(Mouse::ButtonRight));
	glm::vec2 move = Input::GetActionValue("Gameplay/Move");
	if (glm::length(move) > 1.0f)
		move = glm::normalize(move);
	const float yaw = glm::radians(m_Yaw);
	const float pitch = glm::radians(m_Pitch);
	const glm::vec3 forward(-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
	const glm::vec3 right(std::cos(yaw), 0.0f, -std::sin(yaw));
	float rise = 0.0f;
	if (Input::IsActionDown("Gameplay/Up"))
		rise += 1.0f;
	if (Input::IsActionDown("Gameplay/Down"))
		rise -= 1.0f;
	const float speed = FlySpeed * (Input::IsActionDown("Gameplay/Run") ? FastFactor : 1.0f);
	glm::vec3 eye = GetEyePosition() + (forward * move.y + right * move.x + c_Up * rise) * speed * realDeltaTime;
	if (const std::optional<RaycastHit> ground = Physics::Raycast(glm::vec3(eye.x, eye.y + 300.0f, eye.z), -c_Up, 900.0f, m_GroundMask))
		eye.y = std::max(eye.y, ground->Point.y + c_FlightClearance);
	const glm::quat body = glm::angleAxis(yaw, c_Up);
	Player.GetTransform().SetWorldPosition(eye - body * Eye.GetTransform().GetTranslation());
	Grounded = false;
}

void Outpost::TakeBookmark(int32_t number)
{
	if (number < 1 || number > 7 || !Player.IsValid())
		return;
	if (Mode == 0)
		EndTour("taken over");
	const bool macro = number == 7;
	const ShotMarkers& markers = macro ? m_Macro : m_Shots[static_cast<size_t>(number - 1)];
	// A bookmark is a viewpoint: the camera stays where it is put (the flight), at the shot's time of day.
	SetMode(2);
	PlaceCamera(markers.Key, markers.Look, macro ? 30.0f : GetShot(number).Fov);
	SetPhotoHour(GetStation(macro ? StationId::Golden : GetShot(number).Station).Hour);
	if (macro && Eye.IsValid())
	{
		// The rocks' colliders are boxes at 80% of their bounds: a ray would focus behind the surface. The key stands 0.45 m
		// from the boulder's face.
		m_Autofocus = false;
		Eye.SetProperty("Camera", "FocusDistance", glm::length(markers.Look - markers.Key));
		Eye.SetProperty("Camera", "DepthOfField", true);
		Eye.SetProperty("Camera", "Aperture", c_FNumbers[2]);
	}
	Bookmark = number;
	RefreshPhotoPanel();
	Log::Info("Showcase: bookmark ", number);
}
