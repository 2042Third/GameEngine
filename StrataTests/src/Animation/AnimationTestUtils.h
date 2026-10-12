#pragma once

#include "Asset/AssetTestUtils.h"
#include "Strata/Animation/AnimationClip.h"
#include "Strata/Animation/AnimationController.h"
#include "Strata/Animation/AnimatorInstance.h"
#include "Strata/Animation/Skeleton.h"
#include "Strata/Core/JsonUtils.h"

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

	inline Ref<AnimationController> CreateController(AnimationControllerData data)
	{
		std::string error;
		Ref<AnimationController> controller = AnimationController::Create(std::move(data), &error);
		REQUIRE_MESSAGE(controller, error);
		return controller;
	}

	// The stored form of a controller (its document), for asset managers in tests.
	inline std::vector<uint8_t> StoreController(const AnimationController& controller)
	{
		const std::string text = JsonUtils::Dump(controller.Serialize());
		return std::vector<uint8_t>(text.begin(), text.end());
	}

	// The assets of one animator, made in memory, in the form AnimatorInstance takes them: the skeleton, a controller (or
	// none) and a clip per controller state (or the one clip), bound to the skeleton.
	class TestAnimatorAssets
	{
	public:
		TestAnimatorAssets(Ref<Skeleton> skeleton, Ref<AnimationController> controller, std::vector<Ref<AnimationClip>> clips)
			: m_Skeleton(std::move(skeleton)), m_Controller(std::move(controller)), m_Clips(std::move(clips))
		{
			m_RootMotion = RootMotionSetup::Create(*m_Skeleton);
			m_Bindings.reserve(m_Clips.size());
			for (const Ref<AnimationClip>& clip : m_Clips)
				m_Bindings.push_back(clip ? clip->Bind(*m_Skeleton) : AnimationClipBinding());
			for (size_t index = 0; index < m_Clips.size(); index++)
				m_StateClips.push_back(m_Clips[index] ? AnimatorClip { m_Clips[index].get(), &m_Bindings[index] } : AnimatorClip());
		}

		TestAnimatorAssets(const TestAnimatorAssets&) = delete;
		TestAnimatorAssets& operator=(const TestAnimatorAssets&) = delete;

		AnimatorAssets Get() const
		{
			AnimatorAssets assets;
			assets.SkeletonAsset = m_Skeleton.get();
			assets.RootMotion = &m_RootMotion;
			assets.Controller = m_Controller.get();
			assets.ControllerId = m_Controller ? 1 : 0;
			assets.StateClips = m_StateClips;
			return assets;
		}

		const Skeleton& GetSkeleton() const { return *m_Skeleton; }
		const AnimationController& GetController() const { return *m_Controller; }
	private:
		Ref<Skeleton> m_Skeleton;
		Ref<AnimationController> m_Controller;
		std::vector<Ref<AnimationClip>> m_Clips;
		RootMotionSetup m_RootMotion;
		std::vector<AnimationClipBinding> m_Bindings;
		std::vector<AnimatorClip> m_StateClips;
	};

	// A clip that turns the shoulder (the root motion joint) a quarter about +Y in a second.
	inline Ref<AnimationClip> CreateTurnClip()
	{
		std::vector<AnimationChannelData> channels;
		std::vector<float> rotations;
		for (int key = 0; key <= 4; key++)
		{
			const glm::quat rotation = glm::angleAxis(glm::radians(22.5f * static_cast<float>(key)), glm::vec3(0.0f, 1.0f, 0.0f));
			rotations.insert(rotations.end(), { rotation.x, rotation.y, rotation.z, rotation.w });
		}
		channels.push_back(MakeChannel("Shoulder", AnimationPath::Rotation, AnimationInterpolation::Linear, { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }, rotations));
		std::string error;
		Ref<AnimationClip> clip = AnimationClip::Create("Turn", std::move(channels), &error);
		REQUIRE_MESSAGE(clip, error);
		return clip;
	}

	// The handles of the assets AddAnimationTestAssets stores.
	namespace AnimationTestAssets
	{
		constexpr uint64_t Skeleton = 0xA001;   // CreateArmSkeleton
		constexpr uint64_t Wave = 0xA101;       // Elbow 0 -> 90 -> 0 degrees in a second
		constexpr uint64_t Walk = 0xA102;       // The wave, and the shoulder travels 2 along +Z per loop
		constexpr uint64_t Turn = 0xA103;       // The shoulder turns 90 degrees about +Y per loop
		constexpr uint64_t Idle = 0xA104;       // The wave over two seconds
		constexpr uint64_t Jump = 0xA105;       // The wave over half a second
		constexpr uint64_t Controller = 0xA201; // CreateLocomotionControllerData over Idle, Walk and Jump
		constexpr uint64_t Missing = 0xA999;    // Known to no asset manager
	}

	// Stores the arm's skeleton, its clips and the locomotion controller in an asset manager (loaded right away with
	// `preload`).
	inline void AddAnimationTestAssets(FakeAssetManager& manager, bool preload)
	{
		using namespace AnimationTestAssets;
		manager.AddStored(Skeleton, AssetType::Skeleton, CreateArmSkeleton()->Serialize(), "Arm/Skeleton");
		manager.AddStored(Wave, AssetType::AnimationClip, CreateWaveClip(1.0f)->Serialize(), "Arm/Wave");
		manager.AddStored(Walk, AssetType::AnimationClip, CreateWaveClip(1.0f, 2.0f, "Walk")->Serialize(), "Arm/Walk");
		manager.AddStored(Turn, AssetType::AnimationClip, CreateTurnClip()->Serialize(), "Arm/Turn");
		manager.AddStored(Idle, AssetType::AnimationClip, CreateWaveClip(2.0f, 0.0f, "Idle")->Serialize(), "Arm/Idle");
		manager.AddStored(Jump, AssetType::AnimationClip, CreateWaveClip(0.5f, 0.0f, "Jump")->Serialize(), "Arm/Jump");
		const Ref<AnimationController> controller = CreateController(CreateLocomotionControllerData(UUID(Idle), UUID(Walk), UUID(Jump)));
		manager.AddStored(Controller, AssetType::AnimationController, StoreController(*controller), "Arm/Locomotion.stanimator");
		if (preload)
		{
			for (const uint64_t handle : { Skeleton, Wave, Walk, Turn, Idle, Jump, Controller })
				REQUIRE(manager.LoadAssetSync(UUID(handle)));
		}
	}

	// The sum of the weights of what an animator plays.
	inline float SumWeights(const AnimatorInstance& instance)
	{
		float sum = 0.0f;
		for (const AnimatorInstance::Playing& playing : instance.GetPlaying())
			sum += playing.Weight;
		return sum;
	}

}
