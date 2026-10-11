#pragma once

#include "Strata/Animation/AnimationClip.h"
#include "Strata/Animation/AnimationController.h"
#include "Strata/Animation/Skeleton.h"

#include <doctest/doctest.h>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

namespace Strata::Tests
{

	// A three-joint arm along +Y: Shoulder (root, at the origin), Elbow (1 unit up), Wrist (1 unit above the elbow).
	inline Ref<Skeleton> CreateArmSkeleton()
	{
		std::vector<JointPose> rest(3);
		rest[1].Translation = glm::vec3(0.0f, 1.0f, 0.0f);
		rest[2].Translation = glm::vec3(0.0f, 1.0f, 0.0f);
		std::string error;
		Ref<Skeleton> skeleton = Skeleton::Create({ "Shoulder", "Elbow", "Wrist" }, { c_NoJoint, 0, 1 }, rest, c_NoJoint, &error);
		REQUIRE_MESSAGE(skeleton, error);
		return skeleton;
	}

	inline AnimationChannelData MakeChannel(std::string target, AnimationPath path, AnimationInterpolation interpolation, std::vector<float> times, std::vector<float> values)
	{
		AnimationChannelData channel;
		channel.Target = std::move(target);
		channel.Path = path;
		channel.Interpolation = interpolation;
		channel.Times = std::move(times);
		channel.Values = std::move(values);
		return channel;
	}

	// Rotation values (x, y, z, w) of a turn about +Z.
	inline std::vector<float> RotationZ(float degrees)
	{
		const glm::quat rotation = glm::angleAxis(glm::radians(degrees), glm::vec3(0.0f, 0.0f, 1.0f));
		return { rotation.x, rotation.y, rotation.z, rotation.w };
	}

	inline std::vector<float> Concat(std::initializer_list<std::vector<float>> parts)
	{
		std::vector<float> values;
		for (const std::vector<float>& part : parts)
			values.insert(values.end(), part.begin(), part.end());
		return values;
	}

	// A clip of `duration` seconds that bends the elbow from 0 to 90 degrees about +Z and back (linear), and moves the
	// shoulder (the root motion joint) by `travel` along +Z over the clip (linear).
	inline Ref<AnimationClip> CreateWaveClip(float duration = 1.0f, float travel = 0.0f, std::string name = "Wave")
	{
		std::vector<AnimationChannelData> channels;
		channels.push_back(MakeChannel("Elbow", AnimationPath::Rotation, AnimationInterpolation::Linear, { 0.0f, duration * 0.5f, duration },
			Concat({ RotationZ(0.0f), RotationZ(90.0f), RotationZ(0.0f) })));
		channels.push_back(MakeChannel("Shoulder", AnimationPath::Translation, AnimationInterpolation::Linear, { 0.0f, duration },
			{ 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, travel }));
		std::string error;
		Ref<AnimationClip> clip = AnimationClip::Create(std::move(name), std::move(channels), &error);
		REQUIRE_MESSAGE(clip, error);
		return clip;
	}

	// A controller: parameters Speed (float), Grounded (bool), Jump (trigger); states Idle (default), Walk and Jump with the
	// given clips; Idle -> Walk when Speed > 0.5 (0.2 s fade), Walk -> Idle when Speed < 0.5 (0.2 s fade), any state -> Jump
	// on the trigger (0.1 s fade), Jump -> Idle at its exit time 1 (no fade). Walk has two events.
	inline AnimationControllerData CreateLocomotionControllerData(AssetHandle idleClip, AssetHandle walkClip, AssetHandle jumpClip)
	{
		AnimationControllerData data;
		data.Parameters = {
			AnimationParameter { "Speed", AnimationParameterType::Float, 0.0f, false },
			AnimationParameter { "Grounded", AnimationParameterType::Bool, 0.0f, true },
			AnimationParameter { "Jump", AnimationParameterType::Trigger, 0.0f, false } };
		data.States = {
			AnimationState { "Idle", idleClip, 1.0f, true, {} },
			AnimationState { "Walk", walkClip, 1.0f, true, { AnimationEvent { "StepRight", 0.75f }, AnimationEvent { "StepLeft", 0.25f } } },
			AnimationState { "Jump", jumpClip, 1.0f, false, {} } };
		data.DefaultState = 0;

		AnimationTransition toWalk;
		toWalk.From = 0;
		toWalk.To = 1;
		toWalk.Conditions = { AnimationCondition { 0, AnimationConditionMode::Greater, 0.5f } };
		toWalk.Duration = 0.2f;
		AnimationTransition toIdle;
		toIdle.From = 1;
		toIdle.To = 0;
		toIdle.Conditions = { AnimationCondition { 0, AnimationConditionMode::Less, 0.5f } };
		toIdle.Duration = 0.2f;
		AnimationTransition jump;
		jump.From = AnimationTransition::c_AnyState;
		jump.To = 2;
		jump.Conditions = { AnimationCondition { 2, AnimationConditionMode::Set, 0.0f }, AnimationCondition { 1, AnimationConditionMode::True, 0.0f } };
		jump.Duration = 0.1f;
		AnimationTransition land;
		land.From = 2;
		land.To = 0;
		land.ExitTime = 1.0f;
		land.Duration = 0.0f;
		data.Transitions = { toWalk, toIdle, jump, land };
		return data;
	}

}
