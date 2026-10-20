#include "ThirdPersonPlayer.h"

#include "ThirdPerson.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace Strata;

namespace
{

	constexpr int32_t c_MaxMarkers = 64;
	constexpr float c_CheckpointRadius = 3.0f;  // Meters on the ground plane
	constexpr float c_AttractPointRadius = 0.6f;
	const glm::vec3 c_Up(0.0f, 1.0f, 0.0f);

	std::vector<glm::vec3> FindNumberedMarkers(const std::string& prefix)
	{
		std::vector<glm::vec3> positions;
		for (int32_t index = 1; index <= c_MaxMarkers; index++)
		{
			const Entity marker = Scene::FindEntityByName(prefix + std::to_string(index));
			if (!marker.IsValid())
				break;
			positions.push_back(marker.GetTransform().GetWorldPosition());
		}
		return positions;
	}

}

void ThirdPersonPlayer::OnCreate()
{
	StartPosition = GetTransform().GetWorldPosition();
	m_LastPosition = StartPosition;
	ReadYaw();
	FindMarkers();
	Attract = ThirdPerson::IsAttractMode();
	AttractTarget = 1;
	AttractFinished = false;
	Assets::RequestLoad(JumpSound);
	Assets::RequestLoad(LandSound);
}

void ThirdPersonPlayer::OnReload()
{
	FindMarkers();
	ReadYaw();
	m_LastPosition = GetTransform().GetWorldPosition();
}

void ThirdPersonPlayer::OnUpdate(float deltaTime)
{
	TransformComponent transform = GetTransform();
	CharacterController controller = GetEntity().GetCharacterController();
	const glm::vec3 position = transform.GetWorldPosition();

	// The controller moves the player in the fixed steps after the scripts' update: what it walked shows from one update to
	// the next, measured along the view of the update that asked for it.
	const glm::vec3 moved = position - m_LastPosition;
	if (glm::length(moved) < 5.0f) // A respawn or a teleport is no walk
		DistanceWalkedForward += glm::dot(glm::vec2(moved.x, moved.z), m_LastForward);
	m_LastPosition = position;
	const glm::vec3 viewForward = GetViewForward();
	m_LastForward = glm::vec2(viewForward.x, viewForward.z);

	Grounded = controller.IsGrounded();
	if (m_Jumping)
		MaxJumpHeight = std::max(MaxJumpHeight, position.y - m_JumpStartHeight);
	Animator animator = Robot.GetAnimator();
	AnimatorState = Robot.IsValid() ? ThirdPerson::GetAnimatorStateIndex(animator.GetCurrentState()) : -1;
	if (deltaTime <= 0.0f)
		return; // Paused (the pause menu sets the time scale to 0)

	m_JumpCooldown = std::max(0.0f, m_JumpCooldown - deltaTime);
	if (Grounded && m_JumpCooldown <= 0.0f)
	{
		if (m_AirSeconds > 0.3f)
			Audio::PlayOneShotAt(LandSound, position, 0.7f);
		m_AirSeconds = 0.0f;
		m_Jumping = false;
	}
	else
	{
		m_AirSeconds += deltaTime;
	}

	bool running = false;
	const glm::vec3 move = Attract ? GetAttractMove(position) : GetMoveInput(running);
	const float speed = Attract ? AttractSpeed : (running ? RunSpeed : WalkSpeed);
	controller.Move(move * speed);
	if (glm::length(move) > 0.05f)
	{
		// Turn toward where it walks (the controller moves the position, the script turns the body).
		const float target = glm::degrees(std::atan2(-move.x, -move.z));
		const float difference = std::remainder(target - m_Yaw, 360.0f);
		const float maxTurn = TurnSpeed * deltaTime;
		m_Yaw += std::clamp(difference, -maxTurn, maxTurn);
		transform.SetWorldRotation(glm::angleAxis(glm::radians(m_Yaw), c_Up));
	}

	const glm::vec3 velocity = controller.GetVelocity();
	const float groundSpeed = glm::length(glm::vec2(velocity.x, velocity.z));
	if (!Attract)
	{
		const bool jumpPressed = Input::IsKeyPressed(Key::Space) || Input::IsGamepadButtonPressed(GamepadButton::A);
		if (jumpPressed && !m_Jumping && m_JumpCooldown <= 0.0f && (Grounded || m_AirSeconds <= CoyoteSeconds))
			StartJump(JumpSpeed, position, true);
		const bool standing = Grounded && groundSpeed < 0.1f && glm::length(move) < 0.05f;
		if (standing && Robot.IsValid())
		{
			if (Input::IsKeyPressed(Key::D1) || Input::IsGamepadButtonPressed(GamepadButton::X))
			{
				animator.SetTrigger("Wave");
				Log::Info("ThirdPerson: wave");
			}
			if (Input::IsKeyPressed(Key::D2) || Input::IsGamepadButtonPressed(GamepadButton::Y))
			{
				animator.SetTrigger("Dance");
				Log::Info("ThirdPerson: dance");
			}
		}
		if (Input::IsKeyPressed(Key::R))
			Respawn();
	}

	if (Robot.IsValid())
	{
		animator.SetFloat("Speed", groundSpeed);
		animator.SetBool("Grounded", Grounded);
	}
	UpdateCheckpoints(position);
	if (position.y < FallHeight)
		Respawn();
}

void ThirdPersonPlayer::Launch(float speed)
{
	StartJump(speed, GetTransform().GetWorldPosition(), false);
}

void ThirdPersonPlayer::Respawn()
{
	const bool atCheckpoint = Checkpoint > 0 && Checkpoint <= static_cast<int32_t>(m_Checkpoints.size());
	const glm::vec3 target = atCheckpoint ? m_Checkpoints[static_cast<size_t>(Checkpoint - 1)] : StartPosition;
	GetEntity().GetCharacterController().Teleport(target + glm::vec3(0.0f, 0.05f, 0.0f));
	m_LastPosition = target;
	m_Jumping = false;
	m_AirSeconds = 0.0f;
	Respawns++;
	if (atCheckpoint)
		Log::Info("ThirdPerson: back to checkpoint ", Checkpoint);
	else
		Log::Info("ThirdPerson: back to the start");
}

void ThirdPersonPlayer::FindMarkers()
{
	m_Checkpoints = FindNumberedMarkers("Checkpoint");
	m_AttractPoints = FindNumberedMarkers("AttractPoint");
}

void ThirdPersonPlayer::ReadYaw()
{
	const glm::vec3 forward = GetTransform().GetForward();
	m_Yaw = glm::degrees(std::atan2(-forward.x, -forward.z));
}

glm::vec3 ThirdPersonPlayer::GetViewForward() const
{
	const Entity camera = Camera.IsValid() ? Camera : Scene::GetPrimaryCamera();
	if (!camera.IsValid())
		return glm::vec3(0.0f, 0.0f, -1.0f);
	const glm::vec3 forward = camera.GetTransform().GetForward();
	const glm::vec2 flat(forward.x, forward.z);
	if (glm::length(flat) < 0.001f)
		return glm::vec3(0.0f, 0.0f, -1.0f);
	const glm::vec2 direction = glm::normalize(flat);
	return glm::vec3(direction.x, 0.0f, direction.y);
}

glm::vec3 ThirdPersonPlayer::GetMoveInput(bool& outRunning) const
{
	glm::vec2 input(0.0f);
	if (Input::IsKeyDown(Key::W) || Input::IsKeyDown(Key::Up)) input.y += 1.0f;
	if (Input::IsKeyDown(Key::S) || Input::IsKeyDown(Key::Down)) input.y -= 1.0f;
	if (Input::IsKeyDown(Key::D) || Input::IsKeyDown(Key::Right)) input.x += 1.0f;
	if (Input::IsKeyDown(Key::A) || Input::IsKeyDown(Key::Left)) input.x -= 1.0f;
	input.x += Input::GetGamepadAxis(GamepadAxis::LeftX);
	input.y -= Input::GetGamepadAxis(GamepadAxis::LeftY); // The stick's Y grows downward
	if (glm::length(input) > 1.0f)
		input = glm::normalize(input);
	outRunning = Input::IsKeyDown(Key::LeftShift) || Input::IsKeyDown(Key::RightShift) || Input::IsGamepadButtonDown(GamepadButton::RightBumper);
	const glm::vec3 forward = GetViewForward();
	const glm::vec3 right = glm::normalize(glm::cross(forward, c_Up));
	return forward * input.y + right * input.x;
}

glm::vec3 ThirdPersonPlayer::GetAttractMove(const glm::vec3& position)
{
	if (AttractFinished || m_AttractPoints.empty())
		return glm::vec3(0.0f);
	const glm::vec3 target = m_AttractPoints[static_cast<size_t>(std::clamp(AttractTarget, 1, static_cast<int32_t>(m_AttractPoints.size())) - 1)];
	const glm::vec3 toTarget(target.x - position.x, 0.0f, target.z - position.z);
	if (glm::length(toTarget) > c_AttractPointRadius)
		return glm::normalize(toTarget);
	AttractTarget++;
	if (AttractTarget > static_cast<int32_t>(m_AttractPoints.size()))
	{
		AttractFinished = true;
		Log::Info("ThirdPerson: the attract walk is over");
	}
	return glm::vec3(0.0f);
}

void ThirdPersonPlayer::UpdateCheckpoints(const glm::vec3& position)
{
	if (!Grounded)
		return;
	for (size_t index = static_cast<size_t>(std::max(Checkpoint, 0)); index < m_Checkpoints.size(); index++)
	{
		const glm::vec3& checkpoint = m_Checkpoints[index];
		if (glm::length(glm::vec2(position.x - checkpoint.x, position.z - checkpoint.z)) < c_CheckpointRadius && std::abs(position.y - checkpoint.y) < 2.5f)
		{
			Checkpoint = static_cast<int32_t>(index) + 1;
			Log::Info("ThirdPerson: checkpoint ", Checkpoint);
		}
	}
}

void ThirdPersonPlayer::StartJump(float speed, const glm::vec3& position, bool withSound)
{
	GetEntity().GetCharacterController().Jump(speed);
	if (Robot.IsValid())
		Robot.GetAnimator().SetTrigger("Jump");
	if (withSound)
		Audio::PlayOneShotAt(JumpSound, position, 0.6f);
	m_Jumping = true;
	m_JumpStartHeight = position.y;
	MaxJumpHeight = 0.0f;
	m_JumpCooldown = 0.2f;
}

ST_SCRIPT_CLASS(ThirdPersonPlayer)
{
	ST_SCRIPT_FIELD(WalkSpeed);
	ST_SCRIPT_FIELD(RunSpeed);
	ST_SCRIPT_FIELD(JumpSpeed);
	ST_SCRIPT_FIELD(CoyoteSeconds);
	ST_SCRIPT_FIELD(TurnSpeed);
	ST_SCRIPT_FIELD(FallHeight);
	ST_SCRIPT_FIELD(AttractSpeed);
	ST_SCRIPT_FIELD(Robot);
	ST_SCRIPT_FIELD(Camera);
	ST_SCRIPT_FIELD(JumpSound);
	ST_SCRIPT_FIELD(LandSound);
	ST_SCRIPT_FIELD(AnimatorState);
	ST_SCRIPT_FIELD(DistanceWalkedForward);
	ST_SCRIPT_FIELD(MaxJumpHeight);
	ST_SCRIPT_FIELD(Grounded);
	ST_SCRIPT_FIELD(Checkpoint);
	ST_SCRIPT_FIELD(Respawns);
	ST_SCRIPT_FIELD(Attract);
	ST_SCRIPT_FIELD(AttractTarget);
	ST_SCRIPT_FIELD(AttractFinished);
	ST_SCRIPT_FIELD(StartPosition);
}
