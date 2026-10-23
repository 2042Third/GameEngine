#pragma once

#include "StrataScript/StrataScript.h"

#include <cstdint>
#include <vector>

// The player, on the "Player" entity (a CharacterController whose child "Robot" is the animated model), driven by the
// project's input actions (the Gameplay map, which players may rebind): it walks where the camera looks (Move: WASD, the
// arrow keys or the left stick), runs while Run (Shift, the right bumper) is held, jumps (Jump: Space, gamepad A) with a
// short coyote time after leaving a ledge, waves (Wave: 1) and dances (Dance: 2) while it stands, and lands with a sound.
// It goes back to its last checkpoint ("Checkpoint1", "Checkpoint2"... markers it reached) when it falls off the islands
// or with Respawn (R). In the attract mode it walks itself along the "AttractPoint1", "AttractPoint2"... markers.
class ThirdPersonPlayer : public Strata::Script
{
public:
	// Tuning
	float WalkSpeed = 3.0f;          // m/s
	float RunSpeed = 6.5f;
	float JumpSpeed = 5.5f;          // m/s up: about 1.5 m high
	float CoyoteSeconds = 0.15f;     // A jump still counts this long after walking off a ledge
	float TurnSpeed = 720.0f;        // Degrees per second the body turns toward where it walks
	float FallHeight = -15.0f;       // Below this height it is back at its last checkpoint
	float AttractSpeed = 3.0f;       // How fast the attract mode walks
	Strata::Entity Robot;            // The model with the Animator
	Strata::Entity Camera;           // The FollowCamera whose view the movement follows
	Strata::AssetHandle JumpSound;
	Strata::AssetHandle LandSound;

	// What the player does (the HUD, and the sample's regression test)
	int32_t AnimatorState = -1;      // ThirdPerson::c_AnimatorStates: Idle 0, Walk 1, Run 2, Jump 3, Wave 4, Dance 5; -1 another
	float DistanceWalkedForward = 0.0f; // Meters along the camera's forward since it was last set to 0
	float MaxJumpHeight = 0.0f;      // Meters the latest jump rose above where it started
	bool Grounded = false;
	int32_t Checkpoint = 0;          // The last checkpoint reached (0: the start)
	int32_t Respawns = 0;
	bool Attract = false;            // Walking itself (the attract mode)
	int32_t AttractTarget = 1;       // The attract point it walks to
	bool AttractFinished = false;    // It reached the last attract point
	glm::vec3 StartPosition = glm::vec3(0.0f); // Where it started (a field, so that a hot reload keeps it)

	void OnCreate() override;
	void OnReload() override;
	void OnUpdate(float deltaTime) override;

	// Throws the player up at `speed` m/s (a jump pad).
	void Launch(float speed);
	// Back to the last checkpoint (or the start).
	void Respawn();
private:
	void FindMarkers();
	void ReadYaw();
	// The camera's view direction on the ground plane (-Z without a camera).
	glm::vec3 GetViewForward() const;
	glm::vec3 GetMoveInput(bool& outRunning) const;
	glm::vec3 GetAttractMove(const glm::vec3& position);
	void UpdateCheckpoints(const glm::vec3& position);
	void StartJump(float speed, const glm::vec3& position, bool withSound);
private:
	glm::vec3 m_LastPosition = glm::vec3(0.0f);
	glm::vec2 m_LastForward = glm::vec2(0.0f, -1.0f);
	float m_Yaw = 0.0f;              // Degrees, 0: facing -Z
	float m_AirSeconds = 0.0f;       // Since it last stood on the ground
	float m_JumpCooldown = 0.0f;     // Seconds before another jump (the controller leaves the ground a step after Jump)
	bool m_Jumping = false;
	float m_JumpStartHeight = 0.0f;
	std::vector<glm::vec3> m_Checkpoints;
	std::vector<glm::vec3> m_AttractPoints;
};
