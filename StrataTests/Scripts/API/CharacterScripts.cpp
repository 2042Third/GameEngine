// Scripts of the Physics.Character tests (StrataTests/src/Physics/PhysicsCharacterTests.cpp). They run on "Tester" in the
// character test scene: a static "Ground" box (top face at y = 0), "Hero", a character controller standing at the origin
// (1.8 m tall, 0.3 m in radius), and "Plain", an entity without physics.

#include "TestScripts.h"

#include <cstddef>
#include <limits>

using namespace Strata;
using namespace ScriptTests;

namespace
{

	constexpr float c_NaN = std::numeric_limits<float>::quiet_NaN();

	// Points the SDK at another host API table while it lives (restoring the engine's on every path).
	class ScopedHost
	{
	public:
		explicit ScopedHost(const StrataScriptHostAPI* host)
			: m_Previous(Detail::s_Host)
		{
			Detail::s_Host = host;
		}

		~ScopedHost()
		{
			Detail::s_Host = m_Previous;
		}

		ScopedHost(const ScopedHost&) = delete;
		ScopedHost& operator=(const ScopedHost&) = delete;
	private:
		const StrataScriptHostAPI* m_Previous;
	};

}

// Walks, jumps and teleports "Hero" through the SDK over the fixed steps of two seconds (at 60 steps per second).
class CharacterAPI : public CheckingScript
{
public:
	int32_t Steps = 0;
	bool Done = false;

	void OnFixedUpdate(float) override
	{
		const Entity hero = Scene::FindEntityByName("Hero");
		CharacterController controller = hero.GetCharacterController();
		switch (Steps++)
		{
			case 0:
				// It stands on the ground from the start.
				Expect(controller.GetEntity() == hero, "CharacterController::GetEntity");
				Expect(controller.IsGrounded() && controller.GetGroundState() == GroundState::OnGround, "it starts on the ground");
				Expect(controller.GetGroundEntity() == Scene::FindEntityByName("Ground"), "it stands on the ground entity");
				Expect(Near(controller.GetGroundNormal(), glm::vec3(0.0f, 1.0f, 0.0f), 1e-3f), "the ground's normal points up");
				Expect(Near(controller.GetGroundVelocity(), glm::vec3(0.0f)), "the ground does not move");
				Expect(controller.Move({ 2.0f, 0.0f, 0.0f }), "Move");
				break;
			case 30:
				Expect(Near(controller.GetVelocity(), glm::vec3(2.0f, 0.0f, 0.0f), 1e-2f), "it walks at the velocity it was given");
				Expect(Near(hero.GetTransform().GetWorldPosition().x, 1.0f, 0.05f), "half a second at 2 m/s is a meter");
				Expect(controller.Jump(5.0f), "Jump");
				break;
			case 31:
				Expect(controller.GetGroundState() == GroundState::InAir && !controller.IsGrounded(), "a jump leaves the ground");
				Expect(controller.GetVelocity().y > 4.0f, "it rises");
				Expect(!controller.GetGroundEntity().IsValid() && controller.GetGroundNormal() == glm::vec3(0.0f), "nothing below it in the air");
				break;
			case 40:
				Expect(controller.Teleport({ 0.0f, 0.0f, 5.0f }), "Teleport");
				Expect(Near(hero.GetTransform().GetWorldPosition(), glm::vec3(0.0f, 0.0f, 5.0f)), "a teleport moves the entity at once");
				Expect(controller.Move(glm::vec3(0.0f)), "stop walking");
				break;
			case 100:
				Expect(controller.IsGrounded(), "it lands again");
				Expect(Near(hero.GetTransform().GetWorldPosition(), glm::vec3(0.0f, 0.0f, 5.0f), 0.02f), "it stands where it was teleported");
				Done = true;
				break;
			default:
				break;
		}
	}
};

ST_SCRIPT_CLASS(CharacterAPI)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Steps);
	ST_SCRIPT_FIELD(Done);
}

// Calls the character host functions with what the SDK never passes, and the SDK on entities without a controller and on
// engines without the functions: every call fails (or degrades) harmlessly.
class CharacterMisuse : public CheckingScript
{
public:
	bool Done = false;

	void OnFixedUpdate(float) override
	{
		if (Done)
			return;
		Done = true;

		const StrataScriptHostAPI* host = Detail::GetHost();
		StrataScriptContext* context = Detail::GetContext();
		const Entity hero = Scene::FindEntityByName("Hero");
		const Entity plain = Scene::FindEntityByName("Plain");
		const float notFinite[3] = { c_NaN, 0.0f, 0.0f };
		const float up[3] = { 0.0f, 1.0f, 0.0f };

		Expect(!host->CharacterMove(context, hero.GetID(), nullptr), "CharacterMove without a velocity");
		Expect(!host->CharacterMove(context, hero.GetID(), notFinite), "CharacterMove with a non-finite velocity");
		Expect(!host->CharacterJump(context, hero.GetID(), c_NaN), "CharacterJump with a non-finite speed");
		Expect(!host->CharacterTeleport(context, hero.GetID(), nullptr), "CharacterTeleport without a position");
		Expect(!host->CharacterTeleport(context, hero.GetID(), notFinite), "CharacterTeleport to a non-finite position");
		Expect(!host->CharacterGetState(context, hero.GetID(), nullptr), "CharacterGetState without an output");
		Expect(!host->CharacterMove(context, plain.GetID(), up), "an entity without a controller does not walk");
		Expect(!host->CharacterJump(context, 0, 1.0f), "the null entity");
		Expect(!host->CharacterJump(context, 0x51DE5u, 1.0f), "an entity that does not exist");
		StrataScriptCharacterState state = {};
		state.GroundState = 77;
		Expect(!host->CharacterGetState(context, plain.GetID(), &state) && state.GroundState == 77, "no state for an entity without a controller");

		CharacterController none = plain.GetCharacterController();
		Expect(!none.Move({ 1.0f, 0.0f, 0.0f }) && !none.Jump(1.0f) && !none.Teleport(glm::vec3(1.0f)), "the SDK fails without a controller");
		Expect(none.GetGroundState() == GroundState::InAir && !none.IsGrounded() && none.GetVelocity() == glm::vec3(0.0f), "and reads nothing");
		Expect(!Entity().GetCharacterController().Move({ 1.0f, 0.0f, 0.0f }), "the null entity has no controller");

		// An engine that predates the character functions, and a table whose character entries are null.
		StrataScriptHostAPI older = *host;
		older.StructSize = static_cast<uint32_t>(offsetof(StrataScriptHostAPI, CharacterMove));
		CheckUnavailable(hero, older, "an older engine");
		StrataScriptHostAPI missing = *host;
		missing.CharacterMove = nullptr;
		missing.CharacterJump = nullptr;
		missing.CharacterTeleport = nullptr;
		missing.CharacterGetState = nullptr;
		CheckUnavailable(hero, missing, "null entries");
		Expect(hero.GetCharacterController().IsGrounded(), "the engine's table is back");
	}
private:
	void CheckUnavailable(Entity hero, const StrataScriptHostAPI& host, const char* description)
	{
		const ScopedHost scope(&host);
		CharacterController controller = hero.GetCharacterController();
		const bool changed = controller.Move({ 1.0f, 0.0f, 0.0f }) || controller.Jump(1.0f) || controller.Teleport({ 0.0f, 5.0f, 0.0f });
		const bool read = controller.IsGrounded() || controller.GetGroundEntity().IsValid() || controller.GetVelocity() != glm::vec3(0.0f);
		Expect(!changed && !read, description);
	}
};

ST_SCRIPT_CLASS(CharacterMisuse)
{
	ST_SCRIPT_FIELD(Checks);
	ST_SCRIPT_FIELD(Failure);
	ST_SCRIPT_FIELD(Done);
}
