// The character controller SDK (CharacterController) on "Walker": walking, the ground state, jumping, teleporting, and the
// contacts of a controller reaching its script.

#include "FeatureScript.h"

using namespace Strata;
using namespace FeatureTest;

// On "Walker": a character controller standing on the ground. It walks along +X, jumps, lands and is teleported back to
// its start. The first physics steps can be held while mesh colliders load (the game runtime streams the platform's mesh
// in), so each phase waits for the steps it needs, a bounded number of times.
class CharacterFeatures : public FeatureScript
{
public:
	static constexpr float c_WalkSpeed = 1.0f;
	static constexpr float c_JumpSpeed = 4.0f;
	static constexpr int32_t c_MaxWaitSteps = 200;

	glm::vec3 Start = glm::vec3(0.0f);
	int32_t Phase = 0;
	int32_t WaitSteps = 0;
	int32_t GroundContacts = 0;
	int32_t GroundExits = 0;

	void OnCreate() override
	{
		Journal(*this, "CharacterFeatures", "OnCreate");
		Start = GetTransform().GetWorldPosition();
		CharacterController controller = GetEntity().GetCharacterController();
		Expect(controller.GetEntity() == GetEntity(), "CharacterController::GetEntity");
		// Physics runs before the scripts start: the controller stands on the ground in OnCreate already.
		Expect(controller.IsGrounded() && controller.GetGroundState() == GroundState::OnGround, "the walker starts on the ground");
		Expect(controller.GetGroundEntity() == Scene::FindEntityByName("Ground"), "it stands on the ground entity");
		Expect(Near(controller.GetGroundNormal(), glm::vec3(0.0f, 1.0f, 0.0f), 1e-3f), "the ground is level");
		Expect(controller.GetGroundVelocity() == glm::vec3(0.0f), "the ground stands still");
		Expect(controller.Move({ c_WalkSpeed, 0.0f, 0.0f }), "CharacterController::Move");
	}

	void OnFixedUpdate(float) override
	{
		if (Completed)
			return;

		CharacterController controller = GetEntity().GetCharacterController();
		const glm::vec3 position = GetTransform().GetWorldPosition();
		if (Phase == 0 && Waited(position.x > Start.x + 0.5f))
		{
			Expect(Near(controller.GetVelocity(), glm::vec3(c_WalkSpeed, 0.0f, 0.0f), 0.05f), "it walks at the velocity it was given");
			Expect(Near(position.y, Start.y, 0.02f), "it walks on the ground");
			Expect(controller.Jump(c_JumpSpeed), "CharacterController::Jump");
			Phase = 1;
		}
		else if (Phase == 1 && Waited(!controller.IsGrounded()))
		{
			Expect(controller.GetGroundState() == GroundState::InAir, "a jump takes it into the air");
			Expect(controller.GetVelocity().y > 0.0f, "it rises");
			Phase = 2;
		}
		else if (Phase == 2 && Waited(controller.IsGrounded()))
		{
			Expect(controller.Move(glm::vec3(0.0f)), "stop walking");
			Expect(controller.Teleport(Start), "CharacterController::Teleport");
			Expect(Near(GetTransform().GetWorldPosition(), Start), "a teleport moves the entity at once");
			Expect(GroundContacts >= 2 && GroundExits >= 1, "leaving the ground and landing reach the walker's script");
			Completed = true;
		}
	}

	// What it walks on reaches its script like any body's contact.
	void OnCollisionEnter(const Collision& collision) override
	{
		if (collision.Other != Scene::FindEntityByName("Ground"))
			return;
		if (GroundContacts++ == 0)
			Expect(collision.Normal.y < -0.9f, "the ground contact's normal points from the walker down to the ground");
	}

	void OnCollisionExit(const Collision& collision) override
	{
		if (collision.Other == Scene::FindEntityByName("Ground"))
			GroundExits++;
	}
private:
	// Whether a phase's condition holds; fails the scenario after waiting too long for it.
	bool Waited(bool condition)
	{
		if (condition)
		{
			WaitSteps = 0;
			return true;
		}
		if (++WaitSteps == c_MaxWaitSteps)
			Expect(false, "the walker's phase " + std::to_string(Phase) + " ends in time");
		return false;
	}
};

ST_SCRIPT_CLASS(CharacterFeatures)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Completed);
	ST_SCRIPT_FIELD(Start);
	ST_SCRIPT_FIELD(Phase);
	ST_SCRIPT_FIELD(WaitSteps);
	ST_SCRIPT_FIELD(GroundContacts);
	ST_SCRIPT_FIELD(GroundExits);
}
