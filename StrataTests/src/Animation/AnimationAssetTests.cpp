#include <doctest/doctest.h>

#include "Animation/AnimationTestUtils.h"
#include "Strata/Asset/AssetDependencies.h"
#include "Strata/Asset/AssetManager.h"
#include "Strata/Core/JsonUtils.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Strata;

namespace
{

	// Every truncation and every single-byte corruption of a cooked form is rejected or loads something valid, never crashes.
	template<typename Load>
	void CheckCorruptionRejected(const std::vector<uint8_t>& cooked, const Load& load)
	{
		CHECK_FALSE(load(std::span<const uint8_t>()));
		for (size_t length = 0; length < cooked.size(); length += std::max<size_t>(1, cooked.size() / 211))
		{
			CAPTURE(length);
			CHECK_FALSE(load(std::span<const uint8_t>(cooked.data(), length)));
		}
		// The cooked bytes and one more, sized up front: GCC 14 falsely reports a push_back on a copy as out of bounds.
		std::vector<uint8_t> trailing(cooked.size() + 1, 0);
		std::copy(cooked.begin(), cooked.end(), trailing.begin());
		CHECK_FALSE(load(trailing));
		std::vector<uint8_t> wrongMagic = cooked;
		wrongMagic[0] ^= 0xFF;
		CHECK_FALSE(load(wrongMagic));
		for (size_t offset = 0; offset < cooked.size(); offset++)
		{
			for (uint8_t pattern : { uint8_t(0x5A), uint8_t(0xFF), uint8_t(0x80) })
			{
				std::vector<uint8_t> corrupted = cooked;
				corrupted[offset] ^= pattern;
				load(corrupted);
			}
		}
		// Garbage of every small size.
		std::vector<uint8_t> garbage;
		for (uint32_t index = 0; index < 512; index++)
		{
			garbage.push_back(static_cast<uint8_t>(index * 2654435761u >> 24));
			load(garbage);
		}
	}

	float QuatLength(const glm::quat& q)
	{
		return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	}

}

TEST_SUITE("Animation.Assets")
{
	TEST_CASE("Skeletons validate, round trip and find joints by name")
	{
		Ref<Skeleton> skeleton = Tests::CreateArmSkeleton();
		CHECK(skeleton->GetJointCount() == 3);
		CHECK(skeleton->FindJoint("Elbow") == 1);
		CHECK(skeleton->FindJoint("elbow") == c_NoJoint);
		CHECK(skeleton->GetRootMotionJoint() == 0);

		std::string error;
		const std::vector<uint8_t> cooked = skeleton->Serialize();
		Ref<Skeleton> loaded = Skeleton::Deserialize(cooked, &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetJointNames() == skeleton->GetJointNames());
		CHECK(loaded->GetParents() == skeleton->GetParents());
		CHECK(loaded->GetRestPose() == skeleton->GetRestPose());
		CHECK(loaded->Serialize() == cooked);

		// Model transforms chain the parents.
		std::vector<glm::mat4> model(3);
		skeleton->ComputeModelTransforms(skeleton->GetRestPose(), model);
		CHECK(glm::vec3(model[2][3]) == glm::vec3(0.0f, 2.0f, 0.0f));

		std::vector<JointPose> rest(2);
		CHECK_FALSE(Skeleton::Create({}, {}, {}, c_NoJoint, &error));
		CHECK_FALSE(Skeleton::Create({ "A", "B" }, { 1, c_NoJoint }, rest, c_NoJoint, &error)); // Parent after child
		CHECK_FALSE(Skeleton::Create({ "A", "A" }, { c_NoJoint, 0 }, rest, c_NoJoint, &error)); // Duplicate names
		CHECK_FALSE(Skeleton::Create({ "A", "" }, { c_NoJoint, 0 }, rest, c_NoJoint, &error));
		CHECK_FALSE(Skeleton::Create({ "A", "B" }, { c_NoJoint, 0 }, rest, 5, &error));
		std::vector<JointPose> bad(2);
		bad[1].Translation.x = std::numeric_limits<float>::quiet_NaN();
		CHECK_FALSE(Skeleton::Create({ "A", "B" }, { c_NoJoint, 0 }, bad, c_NoJoint, &error));
		bad[1] = JointPose();
		bad[1].Rotation = glm::quat(0.0f, 0.0f, 0.0f, 0.0f);
		CHECK_FALSE(Skeleton::Create({ "A", "B" }, { c_NoJoint, 0 }, bad, c_NoJoint, &error));
	}

	TEST_CASE("Skeleton loading rejects truncated and garbage bytes")
	{
		const std::vector<uint8_t> cooked = Tests::CreateArmSkeleton()->Serialize();
		CheckCorruptionRejected(cooked, [](std::span<const uint8_t> data)
		{
			Ref<Skeleton> skeleton = Skeleton::Deserialize(data);
			if (skeleton)
			{
				// Whatever loads is valid: parents come first, names are unique.
				for (uint32_t joint = 0; joint < skeleton->GetJointCount(); joint++)
				{
					const JointIndex parent = skeleton->GetParent(static_cast<JointIndex>(joint));
					CHECK((parent == c_NoJoint || parent < joint));
				}
			}
			return skeleton != nullptr;
		});
	}

	TEST_CASE("Clips validate, share key times and round trip")
	{
		std::vector<AnimationChannelData> channels;
		channels.push_back(Tests::MakeChannel("Elbow", AnimationPath::Rotation, AnimationInterpolation::Linear, { 0.0f, 1.0f }, Tests::Concat({ Tests::RotationZ(0.0f), Tests::RotationZ(90.0f) })));
		channels.push_back(Tests::MakeChannel("Elbow", AnimationPath::Translation, AnimationInterpolation::Step, { 0.0f, 1.0f }, { 0, 1, 0, 0, 2, 0 }));
		channels.push_back(Tests::MakeChannel("Wrist", AnimationPath::Scale, AnimationInterpolation::CubicSpline, { 0.0f, 2.0f },
			{ 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 2, 2, 2, 0, 0, 0 }));
		std::string error;
		Ref<AnimationClip> clip = AnimationClip::Create("Test", channels, &error);
		REQUIRE_MESSAGE(clip, error);
		CHECK(clip->GetDuration() == 2.0f);
		CHECK(clip->GetTargetNames() == std::vector<std::string> { "Elbow", "Wrist" });
		// The two Elbow channels share their times.
		CHECK(clip->GetChannels()[0].FirstTime == clip->GetChannels()[1].FirstTime);
		CHECK(clip->GetChannels()[2].FirstTime != clip->GetChannels()[0].FirstTime);

		const std::vector<uint8_t> cooked = clip->Serialize();
		Ref<AnimationClip> loaded = AnimationClip::Deserialize(cooked, &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetName() == "Test");
		CHECK(loaded->GetDuration() == clip->GetDuration());
		CHECK(loaded->GetKeyCount() == clip->GetKeyCount());
		CHECK(loaded->Serialize() == cooked);

		auto attempt = [&](AnimationChannelData channel)
		{
			error.clear();
			Ref<AnimationClip> result = AnimationClip::Create("Bad", { std::move(channel) }, &error);
			CHECK_FALSE(error.empty());
			return result != nullptr;
		};
		CHECK_FALSE(AnimationClip::Create("Empty", {}, &error));
		CHECK_FALSE(attempt(Tests::MakeChannel("", AnimationPath::Translation, AnimationInterpolation::Linear, { 0.0f }, { 0, 0, 0 })));
		CHECK_FALSE(attempt(Tests::MakeChannel("A", AnimationPath::Translation, AnimationInterpolation::Linear, { 0.0f, 1.0f }, { 0, 0, 0 })));
		CHECK_FALSE(attempt(Tests::MakeChannel("A", AnimationPath::Translation, AnimationInterpolation::Linear, { 1.0f, 1.0f }, { 0, 0, 0, 1, 1, 1 })));
		CHECK_FALSE(attempt(Tests::MakeChannel("A", AnimationPath::Translation, AnimationInterpolation::Linear, { -1.0f, 1.0f }, { 0, 0, 0, 1, 1, 1 })));
		CHECK_FALSE(attempt(Tests::MakeChannel("A", AnimationPath::Translation, AnimationInterpolation::Linear, { 0.0f, std::numeric_limits<float>::infinity() }, { 0, 0, 0, 1, 1, 1 })));
		CHECK_FALSE(attempt(Tests::MakeChannel("A", AnimationPath::Rotation, AnimationInterpolation::CubicSpline, { 0.0f }, { 0, 0, 0, 1 })));
		CHECK_FALSE(attempt(Tests::MakeChannel("A", AnimationPath::Scale, AnimationInterpolation::Step, { 0.0f }, { 0, std::numeric_limits<float>::quiet_NaN(), 0 })));
		std::vector<AnimationChannelData> twice = { channels[0], channels[0] };
		CHECK_FALSE(AnimationClip::Create("Twice", twice, &error));
	}

	TEST_CASE("Clip loading rejects truncated and garbage bytes")
	{
		std::vector<AnimationChannelData> channels;
		channels.push_back(Tests::MakeChannel("Elbow", AnimationPath::Rotation, AnimationInterpolation::CubicSpline, { 0.0f, 0.5f },
			Tests::Concat({ { 0, 0, 0, 0 }, Tests::RotationZ(0.0f), { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, Tests::RotationZ(45.0f), { 0, 0, 0, 0 } })));
		channels.push_back(Tests::MakeChannel("Shoulder", AnimationPath::Translation, AnimationInterpolation::Linear, { 0.0f, 0.25f, 1.0f }, { 0, 0, 0, 1, 0, 0, 2, 0, 0 }));
		Ref<AnimationClip> clip = AnimationClip::Create("Corrupt", channels);
		REQUIRE(clip);
		const std::vector<uint8_t> cooked = clip->Serialize();
		Ref<Skeleton> skeleton = Tests::CreateArmSkeleton();
		CheckCorruptionRejected(cooked, [&](std::span<const uint8_t> data)
		{
			Ref<AnimationClip> loaded = AnimationClip::Deserialize(data);
			if (loaded)
			{
				// Whatever loads samples without reading outside its data.
				const AnimationClipBinding binding = loaded->Bind(*skeleton);
				std::vector<JointPose> pose = skeleton->GetRestPose();
				for (float time : { -1.0f, 0.0f, 0.3f, 1.0f, 100.0f })
					loaded->Sample(binding, time, pose);
				for (const JointPose& joint : pose)
					CHECK(QuatLength(joint.Rotation) == doctest::Approx(1.0f).epsilon(1e-5));
			}
			return loaded != nullptr;
		});
	}

	TEST_CASE("Controllers validate, round trip as documents and list their clips")
	{
		const AnimationControllerData data = Tests::CreateLocomotionControllerData(UUID(0x1001), UUID(0x1002), UUID(0x1003));
		std::string error;
		Ref<AnimationController> controller = AnimationController::Create(data, &error);
		REQUIRE_MESSAGE(controller, error);
		CHECK(controller->FindState("Walk") == 1);
		CHECK(controller->FindParameter("Jump") == 2);
		CHECK(controller->GetStateTransitions(0).size() == 1);
		CHECK(controller->GetAnyStateTransitions().size() == 1);
		// Events are kept in time order.
		REQUIRE(controller->GetStates()[1].Events.size() == 2);
		CHECK(controller->GetStates()[1].Events[0].Name == "StepLeft");
		CHECK(controller->GetClips() == std::vector<AssetHandle> { UUID(0x1001), UUID(0x1002), UUID(0x1003) });

		const nlohmann::json document = controller->Serialize();
		Ref<AnimationController> loaded = AnimationController::FromJson(document, &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->Serialize() == document);

		// The asset pipeline finds the clips a controller plays.
		const std::string text = JsonUtils::Dump(document);
		std::vector<AssetHandle> references;
		REQUIRE(AssetDependencies::CanReference(AssetType::AnimationController));
		REQUIRE(AssetDependencies::Scan(AssetType::AnimationController, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()), references, &error));
		CHECK(references == std::vector<AssetHandle> { UUID(0x1001), UUID(0x1002), UUID(0x1003) });

		// Validation names the problem.
		auto expectFailure = [&](AnimationControllerData broken, std::string_view message)
		{
			error.clear();
			CHECK_FALSE(AnimationController::Create(std::move(broken), &error));
			CAPTURE(error);
			CHECK(error.find(message) != std::string::npos);
		};
		AnimationControllerData broken = data;
		broken.States.clear();
		expectFailure(broken, "at least one state");
		broken = data;
		broken.States[1].Name = "Idle";
		expectFailure(broken, "used twice");
		broken = data;
		broken.Transitions[0].Conditions[0].Mode = AnimationConditionMode::True;
		expectFailure(broken, "tests the Float parameter 'Speed' with mode True");
		broken = data;
		broken.Transitions[0].To = 7;
		expectFailure(broken, "does not exist");
		broken = data;
		broken.Transitions[0].Duration = -1.0f;
		expectFailure(broken, "cross-fade");
		broken = data;
		broken.States[0].Speed = std::numeric_limits<float>::infinity();
		expectFailure(broken, "speed");
		broken = data;
		broken.States[1].Events[0].Time = -1.0f;
		expectFailure(broken, "time of 0 seconds or more");
		broken = data;
		broken.DefaultState = 3;
		expectFailure(broken, "default state");
	}

	TEST_CASE("Controller documents with wrong JSON are rejected with a reason")
	{
		const nlohmann::json valid = AnimationController::Create(Tests::CreateLocomotionControllerData(UUID(1001), UUID(1002), UUID(1003)))->Serialize();
		std::string error;
		auto expectFailure = [&](const nlohmann::json& document, std::string_view message)
		{
			error.clear();
			CHECK_FALSE(AnimationController::FromJson(document, &error));
			CAPTURE(error);
			CHECK(error.find(message) != std::string::npos);
		};
		nlohmann::json document = valid;
		document["Strata"]["Format"] = "Material";
		expectFailure(document, "Not a Strata animation controller");
		document = valid;
		document["Strata"]["Version"] = 99;
		expectFailure(document, "Unsupported");
		document = valid;
		document["AnimationController"]["States"][0]["Clip"] = "not a handle";
		expectFailure(document, "asset handle");
		document = valid;
		document["AnimationController"]["States"][0]["Speed"] = "fast";
		expectFailure(document, "must be a number");
		document = valid;
		document["AnimationController"]["Transitions"][0]["To"] = "Swim";
		expectFailure(document, "'Swim', which does not exist");
		document = valid;
		document["AnimationController"]["Transitions"][0]["Conditions"][0]["Mode"] = "Above";
		expectFailure(document, "unknown condition mode 'Above'");
		document = valid;
		document["AnimationController"]["Parameters"][0]["Type"] = "Int";
		expectFailure(document, "unknown type 'Int'");
		document = valid;
		document["AnimationController"]["Transitions"] = 3;
		expectFailure(document, "must be an array");
		document = valid;
		document["AnimationController"]["DefaultState"] = "Nowhere";
		expectFailure(document, "default state 'Nowhere'");

		// Garbage bytes and truncations of the document fail without crashing.
		const std::string text = JsonUtils::Dump(valid);
		for (size_t length = 0; length < text.size(); length += 13)
			CHECK_FALSE(AnimationController::Deserialize(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), length)));
	}

	TEST_CASE("The animation module registers loaders for its asset types")
	{
		for (AssetType type : { AssetType::Skeleton, AssetType::AnimationClip, AssetType::AnimationController })
		{
			CAPTURE(AssetTypeToString(type));
			CHECK(AssetLoaderRegistry::Find(type) != nullptr);
			CHECK(AssetTypeFromString(AssetTypeToString(type)) == type);
		}
		CHECK(GetNativeAssetExtension(AssetType::AnimationController) == ".stanimator");

		const std::vector<uint8_t> skeleton = Tests::CreateArmSkeleton()->Serialize();
		AssetMetadata metadata;
		metadata.Type = AssetType::Skeleton;
		AssetLoadData data { std::span<const uint8_t>(skeleton) };
		std::string error;
		Ref<Asset> loaded = (*AssetLoaderRegistry::Find(AssetType::Skeleton))(metadata, data, &error);
		REQUIRE_MESSAGE(loaded, error);
		CHECK(loaded->GetType() == AssetType::Skeleton);
	}
}

TEST_SUITE("Animation.Sampling")
{
	TEST_CASE("Step, linear and cubic spline channels interpolate as glTF defines")
	{
		std::vector<AnimationChannelData> channels;
		channels.push_back(Tests::MakeChannel("Shoulder", AnimationPath::Translation, AnimationInterpolation::Step, { 0.0f, 1.0f, 2.0f }, { 0, 0, 0, 1, 0, 0, 2, 0, 0 }));
		channels.push_back(Tests::MakeChannel("Elbow", AnimationPath::Translation, AnimationInterpolation::Linear, { 0.5f, 1.5f }, { 0, 1, 0, 0, 3, 0 }));
		// Cubic: value 0 -> 1 over 2 seconds with tangents 0 (smoothstep) on X, and with tangents 1 on Y (a straight line,
		// since the slope matches the chord).
		channels.push_back(Tests::MakeChannel("Wrist", AnimationPath::Translation, AnimationInterpolation::CubicSpline, { 0.0f, 2.0f },
			{ 0, 0.5f, 0, 0, 0, 0, 0, 0.5f, 0, 0, 0.5f, 0, 1, 1, 0, 0, 0.5f, 0 }));
		std::string error;
		Ref<AnimationClip> clip = AnimationClip::Create("Interpolation", channels, &error);
		REQUIRE_MESSAGE(clip, error);
		Ref<Skeleton> skeleton = Tests::CreateArmSkeleton();
		const AnimationClipBinding binding = clip->Bind(*skeleton);
		CHECK(binding.BoundChannels == 3);

		auto sample = [&](float time)
		{
			std::vector<JointPose> pose = skeleton->GetRestPose();
			clip->Sample(binding, time, pose);
			return pose;
		};
		// Step holds the earlier key's value.
		CHECK(sample(0.0f)[0].Translation.x == 0.0f);
		CHECK(sample(0.99f)[0].Translation.x == 0.0f);
		CHECK(sample(1.0f)[0].Translation.x == 1.0f);
		CHECK(sample(1.5f)[0].Translation.x == 1.0f);
		CHECK(sample(5.0f)[0].Translation.x == 2.0f);
		// Linear, clamped before the first and after the last key.
		CHECK(sample(0.0f)[1].Translation.y == 1.0f);
		CHECK(sample(1.0f)[1].Translation.y == doctest::Approx(2.0f));
		CHECK(sample(1.25f)[1].Translation.y == doctest::Approx(2.5f));
		CHECK(sample(3.0f)[1].Translation.y == 3.0f);
		// Cubic Hermite: 3s^2 - 2s^3 for flat tangents; the line for tangents equal to the chord's slope.
		for (float time : { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f })
		{
			const float s = time / 2.0f;
			CAPTURE(time);
			CHECK(sample(time)[2].Translation.x == doctest::Approx(3.0f * s * s - 2.0f * s * s * s).epsilon(1e-5));
			CHECK(sample(time)[2].Translation.y == doctest::Approx(s).epsilon(1e-5));
		}
	}

	TEST_CASE("Rotations interpolate along the shorter arc and stay normalized")
	{
		// Keys of different signs (q and -q are the same rotation) and a cubic spline with tangents that would denormalize.
		const std::vector<float> start = Tests::RotationZ(0.0f);
		std::vector<float> end = Tests::RotationZ(170.0f);
		for (float& component : end)
			component = -component;
		std::vector<AnimationChannelData> channels;
		channels.push_back(Tests::MakeChannel("Elbow", AnimationPath::Rotation, AnimationInterpolation::Linear, { 0.0f, 1.0f }, Tests::Concat({ start, end })));
		channels.push_back(Tests::MakeChannel("Wrist", AnimationPath::Rotation, AnimationInterpolation::CubicSpline, { 0.0f, 1.0f },
			Tests::Concat({ { 0, 0, 3, 0 }, start, { 0, 0, 3, 0 }, { 0, 0, 3, 0 }, Tests::RotationZ(120.0f), { 0, 0, 3, 0 } })));
		Ref<AnimationClip> clip = AnimationClip::Create("Rotations", channels);
		REQUIRE(clip);
		Ref<Skeleton> skeleton = Tests::CreateArmSkeleton();
		const AnimationClipBinding binding = clip->Bind(*skeleton);
		for (int step = 0; step <= 64; step++)
		{
			const float time = step / 64.0f;
			std::vector<JointPose> pose = skeleton->GetRestPose();
			clip->Sample(binding, time, pose);
			CAPTURE(time);
			CHECK(QuatLength(pose[1].Rotation) == doctest::Approx(1.0f).epsilon(1e-6));
			CHECK(QuatLength(pose[2].Rotation) == doctest::Approx(1.0f).epsilon(1e-6));
			// The shorter arc: the angle grows linearly from 0 to 170 degrees (q and -q are the same rotation; the last key is
			// stored negated).
			const glm::quat rotation = pose[1].Rotation.w < 0.0f ? -pose[1].Rotation : pose[1].Rotation;
			const float angle = glm::degrees(glm::angle(rotation));
			CHECK(angle == doctest::Approx(170.0f * time).epsilon(1e-3).scale(1.0));
		}
	}

	TEST_CASE("Keyframe reduction drops only keys the interpolation reproduces")
	{
		// 31 keys on a line, with one key off it: the line keeps its ends and the corner around the outlier.
		std::vector<float> times;
		std::vector<float> values;
		for (int key = 0; key <= 30; key++)
		{
			times.push_back(key / 30.0f);
			const float x = key == 15 ? 10.0f : key * 0.1f;
			values.insert(values.end(), { x, 0.0f, 1.0f });
		}
		AnimationChannelData channel = Tests::MakeChannel("Shoulder", AnimationPath::Translation, AnimationInterpolation::Linear, times, values);
		AnimationChannelData reduced = channel;
		AnimationClip::ReduceKeys(reduced);
		CHECK(reduced.Times.size() < channel.Times.size());
		CHECK(reduced.Times.size() == 5); // 0, 14, 15, 16, 30
		Ref<AnimationClip> original = AnimationClip::Create("Original", { channel });
		Ref<AnimationClip> smaller = AnimationClip::Create("Reduced", { reduced });
		REQUIRE(original);
		REQUIRE(smaller);
		Ref<Skeleton> skeleton = Tests::CreateArmSkeleton();
		for (int step = 0; step <= 120; step++)
		{
			const float time = step / 120.0f;
			std::vector<JointPose> a = skeleton->GetRestPose();
			std::vector<JointPose> b = skeleton->GetRestPose();
			original->Sample(original->Bind(*skeleton), time, a);
			smaller->Sample(smaller->Bind(*skeleton), time, b);
			CAPTURE(time);
			CHECK(glm::length(a[0].Translation - b[0].Translation) <= 1e-4f);
		}

		// Rotations on one arc reduce to their ends; repeated step values to their changes.
		AnimationChannelData arc = Tests::MakeChannel("Elbow", AnimationPath::Rotation, AnimationInterpolation::Linear, { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f },
			Tests::Concat({ Tests::RotationZ(0.0f), Tests::RotationZ(10.0f), Tests::RotationZ(20.0f), Tests::RotationZ(30.0f), Tests::RotationZ(40.0f) }));
		AnimationClip::ReduceKeys(arc);
		CHECK(arc.Times.size() == 2);
		AnimationChannelData steps = Tests::MakeChannel("Wrist", AnimationPath::Scale, AnimationInterpolation::Step, { 0.0f, 1.0f, 2.0f, 3.0f },
			{ 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2 });
		AnimationClip::ReduceKeys(steps);
		CHECK(steps.Times == std::vector<float> { 0.0f, 2.0f, 3.0f });
	}

	TEST_CASE("Keyframe reduction of a long channel stops when it is cancelled, leaving the channel as it was")
	{
		// A baked minute at 120 keys per second, all on one line: every key tests a window of earlier ones.
		std::vector<float> times;
		std::vector<float> values;
		for (int key = 0; key < 7200; key++)
		{
			times.push_back(static_cast<float>(key) / 120.0f);
			values.insert(values.end(), { static_cast<float>(key) * 0.01f, 0.0f, 0.0f });
		}
		const AnimationChannelData channel = Tests::MakeChannel("Shoulder", AnimationPath::Translation, AnimationInterpolation::Linear, times, values);

		// Asked while it works, it stops at the first answer that it is cancelled.
		AnimationChannelData cancelled = channel;
		uint32_t asked = 0;
		CHECK_FALSE(AnimationClip::ReduceKeys(cancelled, [&]() { return ++asked == 3; }));
		CHECK(asked == 3);
		CHECK(cancelled.Times == channel.Times);
		CHECK(cancelled.Values == channel.Values);

		// Never cancelled, it asks every c_ReductionCancelInterval keys and reduces the line to its ends.
		AnimationChannelData reduced = channel;
		asked = 0;
		CHECK(AnimationClip::ReduceKeys(reduced, [&]() { asked++; return false; }));
		CHECK(asked == (channel.Times.size() - 2) / AnimationClip::c_ReductionCancelInterval);
		CHECK(reduced.Times.size() < 100);
	}
}
