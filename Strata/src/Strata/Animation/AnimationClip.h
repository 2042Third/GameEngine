#pragma once

#include "Strata/Animation/AnimationTypes.h"
#include "Strata/Asset/Asset.h"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	class Skeleton;

	enum class AnimationPath : uint8_t
	{
		Translation = 0,
		Rotation,
		Scale
	};

	// How a channel's value runs between two keys (glTF's samplers).
	enum class AnimationInterpolation : uint8_t
	{
		Step = 0,   // The earlier key's value until the next key
		Linear,     // Linear (rotations: spherical linear) between the keys
		CubicSpline // Cubic Hermite spline with the keys' in- and out-tangents
	};

	const char* AnimationPathToString(AnimationPath path);
	const char* AnimationInterpolationToString(AnimationInterpolation interpolation);

	// A channel as authored (importers, tests): the values of one part of one joint's transform over time.
	struct AnimationChannelData
	{
		std::string Target; // Joint name (clips bind to skeletons by name)
		AnimationPath Path = AnimationPath::Translation;
		AnimationInterpolation Interpolation = AnimationInterpolation::Linear;
		std::vector<float> Times; // Seconds, strictly increasing, not negative
		// Per key the path's components (3, rotations 4 as x, y, z, w); cubic splines store in-tangent, value and out-tangent
		// per key.
		std::vector<float> Values;
	};

	// One channel of a clip (indices into the clip's arrays).
	struct AnimationChannel
	{
		uint32_t Target = 0; // Into GetTargetNames()
		AnimationPath Path = AnimationPath::Translation;
		AnimationInterpolation Interpolation = AnimationInterpolation::Linear;
		uint32_t KeyCount = 0;
		uint32_t FirstTime = 0;  // Into the clip's key times (channels with the same times share them)
		uint32_t FirstValue = 0; // Into the clip's values
	};

	// Where a clip's channels write in a skeleton's pose: the joint of each channel (c_NoJoint for targets the skeleton does
	// not have, which are skipped) and, for root motion, the channels of the skeleton's root motion joint. Made once per
	// clip and skeleton (AnimationClip::Bind).
	struct AnimationClipBinding
	{
		std::vector<JointIndex> ChannelJoints;
		std::array<int32_t, 3> RootMotionChannels = { -1, -1, -1 }; // Per AnimationPath: the channel, or -1
		uint32_t BoundChannels = 0;
	};

	// Keyframed joint transforms over time: an animation imported from a model ("Animation/<name>" sub-assets of glTF
	// files). Channels name their joint, so a clip plays on any skeleton with joints of those names; joints without a
	// channel keep their rest pose. The clip lasts until its last key (Duration); before a channel's first key and after
	// its last one the channel holds the key's value. Events (named moments) are authored per state in the animation
	// controller, since imported clips carry none.
	//
	// Cooked format ("STAC"): a header, the target names, the channels, the shared key times and the values. Deserialize
	// validates everything (counts and ranges of every channel, times strictly increasing and finite, values finite).
	class AnimationClip : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::AnimationClip; }
		AssetType GetType() const override { return GetStaticType(); }

		static constexpr uint32_t c_CookedVersion = 1;
		static constexpr uint32_t c_MaxChannels = 3u * 65535u;
		static constexpr uint64_t c_MaxKeys = 1ull << 24;      // Over all channels
		static constexpr uint64_t c_MaxValues = 1ull << 26;    // Floats over all channels
		static constexpr uint32_t c_MaxNameLength = 1024;

		// Validates (null with the reason), normalizes rotation values and shares identical key times between channels.
		static Ref<AnimationClip> Create(std::string name, std::vector<AnimationChannelData> channels, std::string* outError = nullptr);

		// Removes keys that interpolating their neighbours reproduces (Step: repeated values; Linear: values on the line or
		// arc between the neighbours kept), within a tolerance far below what shows: the importer's keyframe reduction of
		// baked animations. The first and last keys always stay. Cubic splines are left as they are.
		static void ReduceKeys(AnimationChannelData& channel);

		std::vector<uint8_t> Serialize() const;
		static Ref<AnimationClip> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);

		AssetMemoryUsage GetMemoryUsage() const override;

		const std::string& GetName() const { return m_Name; }
		float GetDuration() const { return m_Duration; }
		const std::vector<std::string>& GetTargetNames() const { return m_TargetNames; }
		const std::vector<AnimationChannel>& GetChannels() const { return m_Channels; }
		uint64_t GetKeyCount() const;

		// The channels' joints in `skeleton` (see AnimationClipBinding).
		AnimationClipBinding Bind(const Skeleton& skeleton) const;

		// Writes every bound channel's value at `time` (seconds; clamped to each channel's keys) into `pose`, one entry per
		// skeleton joint; joints without a channel keep what `pose` holds (the caller starts from the rest pose).
		void Sample(const AnimationClipBinding& binding, float time, std::span<JointPose> pose) const;
		// One channel's value at `time`, written into the part of `pose` the channel animates.
		void SampleChannel(const AnimationChannel& channel, float time, JointPose& pose) const;
		// The root motion joint's pose at `time`: `restPose` with the root motion channels applied.
		JointPose SampleRootMotionJoint(const AnimationClipBinding& binding, float time, const JointPose& restPose) const;
	private:
		AnimationClip() = default;

		uint32_t FindKey(const AnimationChannel& channel, float time) const;
	private:
		std::string m_Name;
		float m_Duration = 0.0f;
		std::vector<std::string> m_TargetNames;
		std::vector<AnimationChannel> m_Channels;
		std::vector<float> m_Times;
		std::vector<float> m_Values;
	};

}
