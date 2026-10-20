// The island's explorer, on the "Player" entity (a CharacterController, a StreamingSource, and the camera as its child
// "Eye"): it flies (WASD, mouse look with the right button, Shift fast, E up and Q down) or walks on the terrain (Tab),
// teleports to the landmarks (1 to 5) once the cells around them and the ground under them are in, and starts with a
// camera tour over the "TourPoint1" to "TourPoint8" markers that any key or click hands over to the player. The streaming
// source moves with it, so the world's cells load around wherever it is.

#include "StrataScript/StrataScript.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>

using namespace Strata;

namespace
{

	constexpr int32_t c_TourPoints = 8;
	constexpr int32_t c_Landmarks = 5;
	const glm::vec3 c_Down(0.0f, -1.0f, 0.0f);

	// The keys and buttons that hand the tour over to the player.
	bool AnyInputPressed()
	{
		constexpr KeyCode c_Keys[] = { Key::W, Key::A, Key::S, Key::D, Key::Q, Key::E, Key::Space, Key::Tab, Key::Enter, Key::LeftShift, Key::D1, Key::D2,
			Key::D3, Key::D4, Key::D5 };
		for (const KeyCode key : c_Keys)
		{
			if (Input::IsKeyPressed(key))
				return true;
		}
		return Input::IsMouseButtonPressed(Mouse::ButtonLeft) || Input::IsMouseButtonPressed(Mouse::ButtonRight);
	}

	glm::vec3 CatmullRom(const glm::vec3& p0, const glm::vec3& p1, const glm::vec3& p2, const glm::vec3& p3, float t)
	{
		const float t2 = t * t;
		const float t3 = t2 * t;
		return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
	}

}

class Explorer : public Script
{
public:
	float FlySpeed = 25.0f;        // m/s
	float FastFactor = 4.0f;       // Holding Shift
	float WalkSpeed = 5.0f;
	float LookSpeed = 0.15f;       // Degrees per pixel
	float Yaw = 0.0f;              // Degrees, kept across hot reloads
	float Pitch = -10.0f;
	bool TourOnStart = true;
	float TourSeconds = 24.0f;     // The whole tour over the eight points
	Entity FadeImage;              // A full-screen UI image faded in while a teleport waits for the cells
	Entity ModeText;

	// What the explorer does (the HUD, and the sample's regression test)
	bool Walking = false;
	bool Touring = false;
	bool Grounded = false;
	float HeightAboveGround = 0.0f; // Meters above the terrain under it (negative: below it)
	float DistanceWalked = 0.0f;    // Meters walked since it was last set to 0
	glm::vec3 StartPosition = glm::vec3(0.0f);
	bool StartAreaLoaded = false;   // The cells around StartPosition are loaded
	int32_t TeleportsDone = 0;

	void OnCreate() override
	{
		StartPosition = GetTransform().GetWorldPosition();
		m_LastPosition = StartPosition;
		m_TerrainMask = TerrainMask();
		SetControllerEnabled(Walking);
		if (TourOnStart)
			StartTour();
		else
			ReadAngles();
	}

	void OnReload() override
	{
		m_TerrainMask = TerrainMask();
		m_LastPosition = GetTransform().GetWorldPosition();
		Touring = false;
	}

	void OnUpdate(float) override
	{
		const float deltaTime = Time::GetUnscaledDeltaTime();
		if (Time::GetTimeScale() <= 0.0f)
			return; // Paused (the pause menu)
		// The controller moves the explorer in the physics step after the scripts: what it walked shows from frame to frame.
		const glm::vec3 now = GetTransform().GetWorldPosition();
		const float step = glm::length(glm::vec2(now.x - m_LastPosition.x, now.z - m_LastPosition.z));
		if (Walking && step < 10.0f) // A teleport is no walk
			DistanceWalked += step;
		if (Touring)
			UpdateTour(deltaTime);
		else if (m_Teleporting)
			UpdateTeleport(deltaTime);
		else
			UpdateControl(deltaTime);

		const glm::vec3 after = GetTransform().GetWorldPosition();
		m_LastPosition = after;
		if (const std::optional<RaycastHit> ground = Physics::Raycast(after + glm::vec3(0.0f, 300.0f, 0.0f), c_Down, 900.0f, m_TerrainMask))
			HeightAboveGround = after.y - ground->Point.y;
		Grounded = Walking && GetEntity().GetCharacterController().IsGrounded();
		StartAreaLoaded = World::IsLoadedAround(StartPosition, 32.0f);
		if (ModeText.IsValid())
			ModeText.GetUI().SetText(Touring ? "Tour  (any key: take over)" : (Walking ? "Walking  (Tab: fly)" : "Flying  (Tab: walk)"));
	}
private:
	static uint32_t TerrainMask()
	{
		const int32_t layer = Physics::GetLayerIndex("Terrain");
		return layer >= 0 ? (1u << static_cast<uint32_t>(layer)) : Physics::c_AllLayers;
	}

	void SetControllerEnabled(bool enabled)
	{
		GetEntity().SetProperty("CharacterController", "Enabled", enabled);
	}

	void UpdateControl(float deltaTime)
	{
		TransformComponent transform = GetTransform();
		if (Input::IsKeyPressed(Key::Tab))
		{
			Walking = !Walking;
			if (Walking)
			{
				// Down onto the terrain under it, then walking.
				const glm::vec3 position = transform.GetWorldPosition();
				if (const std::optional<RaycastHit> ground = Physics::Raycast(position + glm::vec3(0.0f, 300.0f, 0.0f), c_Down, 900.0f, m_TerrainMask))
				{
					SetControllerEnabled(true);
					GetEntity().GetCharacterController().Teleport(ground->Point + glm::vec3(0.0f, 0.05f, 0.0f));
				}
				else
				{
					Walking = false;
				}
			}
			else
			{
				SetControllerEnabled(false);
			}
			Log::Info("OpenWorld: ", Walking ? "walking" : "flying");
		}
		for (int32_t landmark = 0; landmark < c_Landmarks; landmark++)
		{
			if (Input::IsKeyPressed(static_cast<KeyCode>(Key::D1 + landmark)))
				BeginTeleport(landmark + 1);
		}

		if (Input::IsMouseButtonDown(Mouse::ButtonRight))
		{
			const glm::vec2 delta = Input::GetMouseDelta();
			Yaw -= delta.x * LookSpeed;
			Pitch = std::clamp(Pitch - delta.y * LookSpeed, -89.0f, 89.0f);
		}
		ApplyLook();

		const glm::vec3 forward = transform.GetForward();
		const glm::vec3 flatForward = glm::length(glm::vec2(forward.x, forward.z)) > 0.001f ? glm::normalize(glm::vec3(forward.x, 0.0f, forward.z)) : glm::vec3(0.0f, 0.0f, -1.0f);
		const glm::vec3 right = glm::normalize(glm::cross(flatForward, glm::vec3(0.0f, 1.0f, 0.0f)));
		glm::vec3 move(0.0f);
		if (Input::IsKeyDown(Key::W)) move += Walking ? flatForward : forward;
		if (Input::IsKeyDown(Key::S)) move -= Walking ? flatForward : forward;
		if (Input::IsKeyDown(Key::D)) move += right;
		if (Input::IsKeyDown(Key::A)) move -= right;
		if (!Walking)
		{
			if (Input::IsKeyDown(Key::E)) move.y += 1.0f;
			if (Input::IsKeyDown(Key::Q)) move.y -= 1.0f;
		}
		const float speed = (Walking ? WalkSpeed : FlySpeed) * (Input::IsKeyDown(Key::LeftShift) ? (Walking ? 2.0f : FastFactor) : 1.0f);
		const glm::vec3 velocity = glm::length(move) > 0.0f ? glm::normalize(move) * speed : glm::vec3(0.0f);
		if (Walking)
		{
			CharacterController controller = GetEntity().GetCharacterController();
			controller.Move(velocity);
			if (controller.IsGrounded() && Input::IsKeyPressed(Key::Space))
				controller.Jump(5.0f);
		}
		else
		{
			transform.SetWorldPosition(transform.GetWorldPosition() + velocity * deltaTime);
		}
	}

	// The body turns with the yaw (the controller walks along it), the eye with the pitch.
	void ApplyLook()
	{
		GetTransform().SetWorldRotation(glm::angleAxis(glm::radians(Yaw), glm::vec3(0.0f, 1.0f, 0.0f)));
		if (Entity eye = Scene::FindEntityByName("Eye"); eye.IsValid())
			eye.GetTransform().SetRotation(glm::angleAxis(glm::radians(Pitch), glm::vec3(1.0f, 0.0f, 0.0f)));
	}

	void ReadAngles()
	{
		const glm::vec3 forward = GetTransform().GetForward();
		Yaw = glm::degrees(std::atan2(-forward.x, -forward.z));
	}

	void BeginTeleport(int32_t landmark)
	{
		Entity marker = Scene::FindEntityByName("Teleport" + std::to_string(landmark));
		if (!marker.IsValid())
			return;
		m_Target = marker.GetTransform().GetWorldPosition();
		m_TargetYaw = glm::degrees(std::atan2(-marker.GetTransform().GetForward().x, -marker.GetTransform().GetForward().z));
		m_TargetName = "Teleport" + std::to_string(landmark);
		m_Request = World::RequestCellsAround(m_Target, 48.0f);
		m_Teleporting = true;
		m_Fade = 0.0f;
		Log::Info("OpenWorld: teleporting to landmark ", landmark);
	}

	void UpdateTeleport(float deltaTime)
	{
		m_Fade = std::min(1.0f, m_Fade + deltaTime * 4.0f);
		SetFade(m_Fade);
		if (m_Fade < 1.0f)
			return;
		// The cells around the destination and the ground under it (the terrain is in the persistent scene, its colliders
		// are there from the first step) before the explorer arrives.
		if (!World::IsLoadedAround(m_Target, 48.0f))
			return;
		const std::optional<RaycastHit> ground = Physics::Raycast(m_Target + glm::vec3(0.0f, 300.0f, 0.0f), c_Down, 900.0f, m_TerrainMask);
		if (!ground)
			return;
		if (Walking)
			GetEntity().GetCharacterController().Teleport(ground->Point + glm::vec3(0.0f, 0.05f, 0.0f));
		else
			GetTransform().SetWorldPosition(ground->Point + glm::vec3(0.0f, 3.0f, 0.0f));
		Yaw = m_TargetYaw;
		ApplyLook();
		m_Request.Release();
		m_Teleporting = false;
		SetFade(0.0f);
		TeleportsDone++;
		Log::Info("OpenWorld: arrived at ", m_TargetName);
	}

	void SetFade(float alpha)
	{
		if (!FadeImage.IsValid())
			return;
		FadeImage.SetActive(alpha > 0.0f);
		FadeImage.GetUI().SetColor(glm::vec4(0.0f, 0.0f, 0.0f, alpha));
	}

	void StartTour()
	{
		m_Points.fill(glm::vec3(0.0f));
		for (int32_t index = 0; index < c_TourPoints; index++)
		{
			Entity point = Scene::FindEntityByName("TourPoint" + std::to_string(index + 1));
			if (!point.IsValid())
				return;
			m_Points[static_cast<size_t>(index)] = point.GetTransform().GetWorldPosition();
		}
		Touring = true;
		m_TourTime = 0.0f;
		m_TourReached = 0;
		SetControllerEnabled(false);
		Walking = false;
		Log::Info("OpenWorld: tour started");
	}

	void UpdateTour(float deltaTime)
	{
		if (AnyInputPressed())
		{
			EndTour("taken over");
			return;
		}
		m_TourTime += deltaTime;
		const float segments = static_cast<float>(c_TourPoints - 1);
		const float progress = std::clamp(m_TourTime / std::max(TourSeconds, 1.0f), 0.0f, 1.0f) * segments;
		const int32_t segment = std::min(static_cast<int32_t>(progress), c_TourPoints - 2);
		const float t = progress - static_cast<float>(segment);
		const auto point = [&](int32_t index) { return m_Points[static_cast<size_t>(std::clamp(index, 0, c_TourPoints - 1))]; };
		const glm::vec3 position = CatmullRom(point(segment - 1), point(segment), point(segment + 1), point(segment + 2), t);
		const glm::vec3 ahead = CatmullRom(point(segment - 1), point(segment), point(segment + 1), point(segment + 2), std::min(t + 0.05f, 1.0f));
		GetTransform().SetWorldPosition(position);
		const glm::vec3 direction = ahead - position;
		if (glm::length(direction) > 0.01f)
		{
			Yaw = glm::degrees(std::atan2(-direction.x, -direction.z));
			Pitch = -12.0f;
			ApplyLook();
		}
		// Waypoint k is reached when the tour passes it (the first is where it starts).
		const int32_t reached = std::min(static_cast<int32_t>(progress + 0.001f) + 1, c_TourPoints);
		while (m_TourReached < reached)
		{
			m_TourReached++;
			Log::Info("OpenWorld: tour reached waypoint ", m_TourReached);
		}
		if (m_TourTime >= TourSeconds)
			EndTour("finished");
	}

	void EndTour(const char* how)
	{
		Touring = false;
		Log::Info("OpenWorld: tour ", how);
	}
private:
	uint32_t m_TerrainMask = Physics::c_AllLayers;
	glm::vec3 m_LastPosition = glm::vec3(0.0f);
	bool m_Teleporting = false;
	glm::vec3 m_Target = glm::vec3(0.0f);
	float m_TargetYaw = 0.0f;
	std::string m_TargetName;
	WorldCellRequest m_Request;
	float m_Fade = 0.0f;
	std::array<glm::vec3, c_TourPoints> m_Points {};
	float m_TourTime = 0.0f;
	int32_t m_TourReached = 0;
};

ST_SCRIPT_CLASS(Explorer)
{
	ST_SCRIPT_FIELD(FlySpeed);
	ST_SCRIPT_FIELD(FastFactor);
	ST_SCRIPT_FIELD(WalkSpeed);
	ST_SCRIPT_FIELD(LookSpeed);
	ST_SCRIPT_FIELD(Yaw);
	ST_SCRIPT_FIELD(Pitch);
	ST_SCRIPT_FIELD(TourOnStart);
	ST_SCRIPT_FIELD(TourSeconds);
	ST_SCRIPT_FIELD(FadeImage);
	ST_SCRIPT_FIELD(ModeText);
	ST_SCRIPT_FIELD(Walking);
	ST_SCRIPT_FIELD(Touring);
	ST_SCRIPT_FIELD(Grounded);
	ST_SCRIPT_FIELD(HeightAboveGround);
	ST_SCRIPT_FIELD(DistanceWalked);
	ST_SCRIPT_FIELD(StartPosition);
	ST_SCRIPT_FIELD(StartAreaLoaded);
	ST_SCRIPT_FIELD(TeleportsDone);
}
