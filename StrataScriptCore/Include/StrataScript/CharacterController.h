#pragma once

#include "StrataScript/Entity.h"
#include "StrataScript/Host.h"
#include "StrataScript/Value.h"

#include <glm/glm.hpp>

#include <cstdint>

namespace Strata
{

	// Where a character controller stands (CharacterController::GetGroundState).
	enum class GroundState : uint32_t
	{
		OnGround = StrataScriptGroundState_OnGround,           // On walkable ground (not steeper than its MaxSlopeDegrees)
		OnSteepGround = StrataScriptGroundState_OnSteepGround, // On ground too steep to walk on: it slides down
		NotSupported = StrataScriptGroundState_NotSupported,   // Touching something that does not hold it up
		InAir = StrataScriptGroundState_InAir                  // Touching nothing
	};

	// The character controller of an entity while the scene plays: an active entity with an enabled "CharacterController"
	// component (an upright capsule whose bottom is the entity's origin) and no RigidBody. Every fixed step it moves with the
	// velocity Move gave it, falls with gravity (times its GravityScale), slides along walls, walks up steps and walkable
	// slopes, follows the ground down and pushes dynamic bodies; it writes its entity's position (not its rotation: turn
	// the entity yourself, e.g. to face where it walks). Its size and limits are properties of the component
	// (Entity::GetProperty/SetProperty). Calls fail on other entities and while the scene plays without physics (the engine
	// logs why). Its contacts reach the scripts on its entity as OnCollisionEnter/Exit (what it walks on and into) and
	// OnTriggerEnter/Exit (triggers it walks through).
	class CharacterController
	{
	public:
		explicit CharacterController(Entity entity)
			: m_Entity(entity)
		{
		}

		Entity GetEntity() const { return m_Entity; }

		// The velocity (meters per second, world space) it walks with from now on, until the next call: call it whenever the
		// player's input changes (every frame is fine), and Move(glm::vec3(0.0f)) to stop. The vertical part counts only while
		// its GravityScale is 0 (flying, swimming); otherwise gravity, jumps and the ground move it up and down.
		bool Move(const glm::vec3& velocity)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(CharacterMove);
			if (!host || m_Entity.GetID() == 0)
				return false;
			float value[3];
			Detail::ToABIVector3(velocity, value);
			return host->CharacterMove(Detail::GetContext(), m_Entity.GetID(), value);
		}

		// Makes it move up at `speed` meters per second at its next fixed step, on the ground or not (check IsGrounded for a
		// jump only from the ground).
		bool Jump(float speed)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(CharacterJump);
			return host && m_Entity.GetID() != 0 && host->CharacterJump(Detail::GetContext(), m_Entity.GetID(), speed);
		}

		// Moves it (its feet: the entity's origin) and its entity to a world position at once, keeping how it walks.
		bool Teleport(const glm::vec3& position)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(CharacterTeleport);
			if (!host || m_Entity.GetID() == 0)
				return false;
			float value[3];
			Detail::ToABIVector3(position, value);
			return host->CharacterTeleport(Detail::GetContext(), m_Entity.GetID(), value);
		}

		// On walkable ground after its last fixed step.
		bool IsGrounded() const { return GetGroundState() == GroundState::OnGround; }

		// InAir also for entities without a controller.
		GroundState GetGroundState() const
		{
			StrataScriptCharacterState state;
			return ReadState(state) ? static_cast<GroundState>(state.GroundState) : GroundState::InAir;
		}

		// The surface normal of what it stands on (zero in the air).
		glm::vec3 GetGroundNormal() const
		{
			StrataScriptCharacterState state;
			return ReadState(state) ? Detail::FromABIVector3(state.GroundNormal) : glm::vec3(0.0f);
		}

		// The velocity of what it stands on (a moving platform; zero on still ground and in the air).
		glm::vec3 GetGroundVelocity() const
		{
			StrataScriptCharacterState state;
			return ReadState(state) ? Detail::FromABIVector3(state.GroundVelocity) : glm::vec3(0.0f);
		}

		// The entity owning what it stands on (none in the air).
		Entity GetGroundEntity() const
		{
			StrataScriptCharacterState state;
			return ReadState(state) ? Entity(state.GroundEntity) : Entity();
		}

		// How fast it moved in its last fixed step, after collisions (meters per second): zero while it walks into a wall.
		glm::vec3 GetVelocity() const
		{
			StrataScriptCharacterState state;
			return ReadState(state) ? Detail::FromABIVector3(state.Velocity) : glm::vec3(0.0f);
		}
	private:
		bool ReadState(StrataScriptCharacterState& outState) const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(CharacterGetState);
			outState = {};
			return host && m_Entity.GetID() != 0 && host->CharacterGetState(Detail::GetContext(), m_Entity.GetID(), &outState);
		}
	private:
		Entity m_Entity;
	};

	inline CharacterController Entity::GetCharacterController() const
	{
		return CharacterController(*this);
	}

}
