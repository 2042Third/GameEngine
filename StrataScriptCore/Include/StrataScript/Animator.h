#pragma once

#include "StrataScript/Entity.h"
#include "StrataScript/Host.h"
#include "StrataScript/Value.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	// Where a joint of an animator's pose is in the world (Animator::GetJointTransform).
	struct JointTransform
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 Scale = glm::vec3(1.0f);
	};

	// Control of an entity's Animator component while the scene plays: the states of its animation controller, its
	// parameters (which the controller's transitions test), its speed and its pose. States and parameters are named as in
	// the controller. Calls fail on entities without an Animator component, for states and parameters the controller does
	// not have (or of another type) and while the scene plays without animation (the engine logs why). Commands given
	// while the controller is still loading apply once it is loaded. The scripts on the animator's entity receive the
	// events of its states through Script::OnAnimationEvent.
	class Animator
	{
	public:
		explicit Animator(Entity entity)
			: m_Entity(entity)
		{
		}

		Entity GetEntity() const { return m_Entity; }

		// Jumps to a state of the controller, from its start.
		bool Play(std::string_view state)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AnimatorPlay);
			return host && m_Entity.GetID() != 0 && host->AnimatorPlay(Detail::GetContext(), m_Entity.GetID(), Detail::ToABIString(state));
		}

		// Plays an animation clip directly, without the controller's transitions until a state is played again (the
		// component's Loop says whether it repeats).
		bool Play(AssetHandle clip)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AnimatorPlayClip);
			return host && m_Entity.GetID() != 0 && host->AnimatorPlayClip(Detail::GetContext(), m_Entity.GetID(), clip.ID);
		}

		// Blends from what plays now to a state over `seconds`.
		bool CrossFade(std::string_view state, float seconds)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AnimatorCrossFade);
			return host && m_Entity.GetID() != 0 && host->AnimatorCrossFade(Detail::GetContext(), m_Entity.GetID(), Detail::ToABIString(state), seconds);
		}

		bool SetFloat(std::string_view parameter, float value)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AnimatorSetFloat);
			return host && m_Entity.GetID() != 0 && host->AnimatorSetFloat(Detail::GetContext(), m_Entity.GetID(), Detail::ToABIString(parameter), value);
		}

		bool SetBool(std::string_view parameter, bool value)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AnimatorSetBool);
			return host && m_Entity.GetID() != 0 && host->AnimatorSetBool(Detail::GetContext(), m_Entity.GetID(), Detail::ToABIString(parameter), value);
		}

		// Sets a trigger: it stays set until a transition that tests it fires (and consumes it) or ResetTrigger clears it.
		bool SetTrigger(std::string_view parameter)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AnimatorSetTrigger);
			return host && m_Entity.GetID() != 0 && host->AnimatorSetTrigger(Detail::GetContext(), m_Entity.GetID(), Detail::ToABIString(parameter));
		}

		bool ResetTrigger(std::string_view parameter)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AnimatorResetTrigger);
			return host && m_Entity.GetID() != 0 && host->AnimatorResetTrigger(Detail::GetContext(), m_Entity.GetID(), Detail::ToABIString(parameter));
		}

		// The playback rate of everything the animator plays (the component's Speed; negative plays backwards, at most 100).
		bool SetSpeed(float speed)
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AnimatorSetSpeed);
			return host && m_Entity.GetID() != 0 && host->AnimatorSetSpeed(Detail::GetContext(), m_Entity.GetID(), speed);
		}

		// The state the animator is in: the target of the latest transition or command (empty while it plays a clip directly,
		// before it plays, or when it cannot be reached).
		std::string GetCurrentState() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AnimatorGetCurrentState);
			if (!host || m_Entity.GetID() == 0)
				return {};
			const uint64_t entity = m_Entity.GetID();
			return Detail::ReadHostString([&](char* buffer, uint64_t capacity) { return host->AnimatorGetCurrentState(Detail::GetContext(), entity, buffer, capacity); });
		}

		// How far the current state is: its time over its clip's duration, counting loops (1.5: halfway through the second
		// loop).
		float GetNormalizedTime() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AnimatorGetNormalizedTime);
			return host && m_Entity.GetID() != 0 ? host->AnimatorGetNormalizedTime(Detail::GetContext(), m_Entity.GetID()) : 0.0f;
		}

		// Whether a cross-fade is under way.
		bool IsInTransition() const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AnimatorIsInTransition);
			return host && m_Entity.GetID() != 0 && host->AnimatorIsInTransition(Detail::GetContext(), m_Entity.GetID());
		}

		// A joint's world transform in the animator's current pose, e.g. to put something in a hand (an entity with an
		// AttachToJoint component follows a joint without a script). Nothing before the animator has a pose, or for a joint
		// its skeleton does not have.
		std::optional<JointTransform> GetJointTransform(std::string_view joint) const
		{
			const StrataScriptHostAPI* host = ST_SCRIPT_DETAIL_HOST_WITH(AnimatorGetJointTransform);
			StrataScriptTransform transform = {};
			if (!host || m_Entity.GetID() == 0 || !host->AnimatorGetJointTransform(Detail::GetContext(), m_Entity.GetID(), Detail::ToABIString(joint), &transform))
				return std::nullopt;
			JointTransform result;
			result.Position = Detail::FromABIVector3(transform.Translation);
			result.Rotation = Detail::FromABIQuat(transform.Rotation);
			result.Scale = Detail::FromABIVector3(transform.Scale);
			return result;
		}
	private:
		Entity m_Entity;
	};

	inline Animator Entity::GetAnimator() const
	{
		return Animator(*this);
	}

}
