#include "stpch.h"
#include "Strata/Animation/AnimationClip.h"

#include "Strata/Animation/Skeleton.h"
#include "Strata/Core/BinaryStream.h"
#include "Strata/Core/Hash.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		constexpr uint32_t c_ClipMagic = 0x43415453; // "STAC"

		struct CookedClipHeader
		{
			uint32_t Magic;
			uint32_t Version;
			uint32_t ChannelCount;
			uint32_t TargetCount;
			uint64_t TimeCount;
			uint64_t ValueCount;
			float Duration;
			uint32_t Reserved;
		};

		struct CookedChannel
		{
			uint32_t Target;
			uint8_t Path;
			uint8_t Interpolation;
			uint16_t Reserved;
			uint32_t KeyCount;
			uint32_t FirstTime;
			uint32_t FirstValue;
		};
		static_assert(sizeof(CookedChannel) == 20);

		// Keys whose value deviates from the interpolation of their kept neighbours by less than this are dropped (a hundred
		// thousandth of a unit for translations and scales at unit size; about 0.01 degrees for rotations, measured as the
		// distance between the unit quaternions, which unlike their dot product keeps its precision for small angles).
		constexpr float c_ReductionTolerance = 1.0e-5f;
		constexpr float c_RotationReductionTolerance = 1.0e-4f;
		// Keys tested against one interpolation at most: bounds the reduction's work on long straight runs.
		constexpr uint32_t c_ReductionWindow = 256;

		uint32_t GetComponentCount(AnimationPath path)
		{
			return path == AnimationPath::Rotation ? 4u : 3u;
		}

		uint32_t GetKeyStride(AnimationPath path, AnimationInterpolation interpolation)
		{
			return GetComponentCount(path) * (interpolation == AnimationInterpolation::CubicSpline ? 3u : 1u);
		}

		bool IsValidPath(uint32_t path)
		{
			return path <= static_cast<uint32_t>(AnimationPath::Scale);
		}

		bool IsValidInterpolation(uint32_t interpolation)
		{
			return interpolation <= static_cast<uint32_t>(AnimationInterpolation::CubicSpline);
		}

		glm::vec3 ReadVector(const float* values)
		{
			return glm::vec3(values[0], values[1], values[2]);
		}

		glm::quat ReadRotation(const float* values)
		{
			return glm::quat(values[3], values[0], values[1], values[2]);
		}

		// Validates a channel's times (finite, not negative, strictly increasing).
		bool ValidTimes(std::span<const float> times)
		{
			for (size_t index = 0; index < times.size(); index++)
			{
				if (!std::isfinite(times[index]) || times[index] < 0.0f || (index > 0 && !(times[index] > times[index - 1])))
					return false;
			}
			return true;
		}

		bool ValidValues(std::span<const float> values)
		{
			return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
		}

		// Normalizes the rotation values of a channel (cubic splines: the values, not the tangents).
		void NormalizeRotations(AnimationInterpolation interpolation, uint32_t keyCount, float* values)
		{
			const uint32_t stride = GetKeyStride(AnimationPath::Rotation, interpolation);
			const uint32_t offset = interpolation == AnimationInterpolation::CubicSpline ? 4u : 0u;
			for (uint32_t key = 0; key < keyCount; key++)
			{
				float* value = values + static_cast<size_t>(key) * stride + offset;
				const glm::quat rotation = AnimationMath::Normalize(ReadRotation(value));
				value[0] = rotation.x;
				value[1] = rotation.y;
				value[2] = rotation.z;
				value[3] = rotation.w;
			}
		}

		struct TimesKey
		{
			uint64_t Hash = 0;
			size_t Count = 0;
			bool operator==(const TimesKey&) const = default;
		};

		struct TimesKeyHasher
		{
			size_t operator()(const TimesKey& key) const { return static_cast<size_t>(key.Hash ^ (key.Count * 0x9E3779B97F4A7C15ull)); }
		};

	}

	const char* AnimationPathToString(AnimationPath path)
	{
		switch (path)
		{
			case AnimationPath::Translation: return "Translation";
			case AnimationPath::Rotation:    return "Rotation";
			case AnimationPath::Scale:       return "Scale";
		}
		return "Translation";
	}

	const char* AnimationInterpolationToString(AnimationInterpolation interpolation)
	{
		switch (interpolation)
		{
			case AnimationInterpolation::Step:        return "Step";
			case AnimationInterpolation::Linear:      return "Linear";
			case AnimationInterpolation::CubicSpline: return "CubicSpline";
		}
		return "Linear";
	}

	Ref<AnimationClip> AnimationClip::Create(std::string name, std::vector<AnimationChannelData> channels, std::string* outError)
	{
		auto fail = [outError](const std::string& message) -> Ref<AnimationClip>
		{
			if (outError)
				*outError = message;
			return nullptr;
		};

		if (name.size() > c_MaxNameLength)
			return fail("The clip's name is too long");
		if (channels.empty())
			return fail("The clip has no channels");
		if (channels.size() > c_MaxChannels)
			return fail(fmt::format("The clip has {} channels; at most {} are supported", channels.size(), c_MaxChannels));

		Ref<AnimationClip> clip(new AnimationClip());
		clip->m_Name = std::move(name);
		std::unordered_map<std::string, uint32_t> targets;
		std::unordered_set<uint64_t> animated; // (target << 2) | path
		std::unordered_map<TimesKey, std::vector<uint32_t>, TimesKeyHasher> sharedTimes; // Offsets of time arrays with that content
		uint64_t keyCount = 0;
		uint64_t valueCount = 0;
		for (AnimationChannelData& data : channels)
		{
			if (data.Target.empty() || data.Target.size() > c_MaxNameLength)
				return fail("A channel has no target joint name, or a too long one");
			if (!IsValidPath(static_cast<uint32_t>(data.Path)) || !IsValidInterpolation(static_cast<uint32_t>(data.Interpolation)))
				return fail(fmt::format("The channel of '{}' has an unknown path or interpolation", data.Target));
			const size_t keys = data.Times.size();
			const uint32_t stride = GetKeyStride(data.Path, data.Interpolation);
			if (keys == 0 || data.Values.size() != keys * stride)
				return fail(fmt::format("The {} channel of '{}' has {} keys and {} values; {} values per key are needed", AnimationPathToString(data.Path), data.Target, keys,
					data.Values.size(), stride));
			keyCount += keys;
			valueCount += data.Values.size();
			if (keyCount > c_MaxKeys || valueCount > c_MaxValues)
				return fail("The clip has too many keys");
			if (!ValidTimes(data.Times))
				return fail(fmt::format("The {} channel of '{}' has key times that are negative, not finite or not increasing", AnimationPathToString(data.Path), data.Target));
			if (!ValidValues(data.Values))
				return fail(fmt::format("The {} channel of '{}' has non-finite values", AnimationPathToString(data.Path), data.Target));
			if (data.Path == AnimationPath::Rotation)
				NormalizeRotations(data.Interpolation, static_cast<uint32_t>(keys), data.Values.data());

			auto [target, inserted] = targets.emplace(data.Target, static_cast<uint32_t>(clip->m_TargetNames.size()));
			if (inserted)
				clip->m_TargetNames.push_back(data.Target);
			if (!animated.insert((static_cast<uint64_t>(target->second) << 2) | static_cast<uint64_t>(data.Path)).second)
				return fail(fmt::format("Two channels animate the {} of '{}'", AnimationPathToString(data.Path), data.Target));

			AnimationChannel channel;
			channel.Target = target->second;
			channel.Path = data.Path;
			channel.Interpolation = data.Interpolation;
			channel.KeyCount = static_cast<uint32_t>(keys);
			// Channels of one glTF sampler input (or baked at one rate) share their times.
			const TimesKey timesKey { Hash::FNV1a(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.Times.data()), data.Times.size() * sizeof(float))), keys };
			std::vector<uint32_t>& candidates = sharedTimes[timesKey];
			auto shared = std::find_if(candidates.begin(), candidates.end(), [&](uint32_t offset)
			{
				return std::equal(data.Times.begin(), data.Times.end(), clip->m_Times.begin() + offset);
			});
			if (shared != candidates.end())
			{
				channel.FirstTime = *shared;
			}
			else
			{
				channel.FirstTime = static_cast<uint32_t>(clip->m_Times.size());
				clip->m_Times.insert(clip->m_Times.end(), data.Times.begin(), data.Times.end());
				candidates.push_back(channel.FirstTime);
			}
			channel.FirstValue = static_cast<uint32_t>(clip->m_Values.size());
			clip->m_Values.insert(clip->m_Values.end(), data.Values.begin(), data.Values.end());
			clip->m_Duration = std::max(clip->m_Duration, data.Times.back());
			clip->m_Channels.push_back(channel);
		}
		return clip;
	}

	void AnimationClip::ReduceKeys(AnimationChannelData& channel)
	{
		const size_t keys = channel.Times.size();
		const uint32_t components = GetComponentCount(channel.Path);
		if (keys <= 2 || channel.Interpolation == AnimationInterpolation::CubicSpline || channel.Values.size() != keys * components)
			return;

		const bool rotation = channel.Path == AnimationPath::Rotation;
		auto value = [&](size_t key) { return channel.Values.data() + key * components; };
		auto matches = [&](const float* expected, const float* actual)
		{
			if (rotation)
			{
				const glm::quat a = AnimationMath::Normalize(ReadRotation(expected));
				glm::quat b = AnimationMath::Normalize(ReadRotation(actual));
				if (glm::dot(a, b) < 0.0f)
					b = -b; // The same rotation
				const glm::vec4 difference(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
				return glm::length(difference) <= c_RotationReductionTolerance;
			}
			const glm::vec3 a = ReadVector(expected);
			const glm::vec3 b = ReadVector(actual);
			return glm::length(a - b) <= c_ReductionTolerance * std::max(1.0f, glm::length(b));
		};

		std::vector<uint8_t> keep(keys, 0);
		keep.front() = 1;
		keep.back() = 1;
		size_t lastKept = 0;
		for (size_t key = 1; key + 1 < keys; key++)
		{
			bool removable = key - lastKept <= c_ReductionWindow;
			if (removable && channel.Interpolation == AnimationInterpolation::Step)
			{
				// A step holds the previous kept value: a key repeating it changes nothing.
				removable = std::equal(value(key), value(key) + components, value(lastKept));
			}
			else if (removable)
			{
				// Every key since the last kept one must lie on the interpolation from it to the next key.
				const float startTime = channel.Times[lastKept];
				const float span = channel.Times[key + 1] - startTime;
				for (size_t tested = lastKept + 1; removable && tested <= key; tested++)
				{
					const float s = (channel.Times[tested] - startTime) / span;
					float interpolated[4];
					if (rotation)
					{
						const glm::quat result = AnimationMath::Slerp(AnimationMath::Normalize(ReadRotation(value(lastKept))), AnimationMath::Normalize(ReadRotation(value(key + 1))), s);
						interpolated[0] = result.x;
						interpolated[1] = result.y;
						interpolated[2] = result.z;
						interpolated[3] = result.w;
					}
					else
					{
						for (uint32_t component = 0; component < components; component++)
							interpolated[component] = value(lastKept)[component] + (value(key + 1)[component] - value(lastKept)[component]) * s;
					}
					removable = matches(interpolated, value(tested));
				}
			}
			if (!removable)
			{
				keep[key] = 1;
				lastKept = key;
			}
		}

		size_t written = 0;
		for (size_t key = 0; key < keys; key++)
		{
			if (!keep[key])
				continue;
			channel.Times[written] = channel.Times[key];
			std::copy(value(key), value(key) + components, channel.Values.data() + written * components);
			written++;
		}
		channel.Times.resize(written);
		channel.Values.resize(written * components);
	}

	std::vector<uint8_t> AnimationClip::Serialize() const
	{
		BinaryWriter writer;
		CookedClipHeader header = {};
		header.Magic = c_ClipMagic;
		header.Version = c_CookedVersion;
		header.ChannelCount = static_cast<uint32_t>(m_Channels.size());
		header.TargetCount = static_cast<uint32_t>(m_TargetNames.size());
		header.TimeCount = m_Times.size();
		header.ValueCount = m_Values.size();
		header.Duration = m_Duration;
		writer.Write(header);
		writer.WriteString(m_Name);
		for (const std::string& target : m_TargetNames)
			writer.WriteString(target);
		for (const AnimationChannel& channel : m_Channels)
		{
			writer.Write(CookedChannel { channel.Target, static_cast<uint8_t>(channel.Path), static_cast<uint8_t>(channel.Interpolation), 0, channel.KeyCount,
				channel.FirstTime, channel.FirstValue });
		}
		writer.WriteBytes(m_Times.data(), m_Times.size() * sizeof(float));
		writer.WriteBytes(m_Values.data(), m_Values.size() * sizeof(float));
		return writer.TakeData();
	}

	Ref<AnimationClip> AnimationClip::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		auto fail = [outError](const std::string& message) -> Ref<AnimationClip>
		{
			if (outError)
				*outError = message;
			return nullptr;
		};

		BinaryReader reader(data);
		const CookedClipHeader header = reader.Read<CookedClipHeader>();
		if (!reader.IsValid() || header.Magic != c_ClipMagic)
			return fail("Not a cooked Strata animation clip");
		if (header.Version != c_CookedVersion)
			return fail(fmt::format("Unsupported cooked animation clip version {}", header.Version));
		// Every count is bounded by the format's limits and by the bytes that are left, before anything is allocated.
		if (header.ChannelCount == 0 || header.ChannelCount > c_MaxChannels || header.TargetCount == 0 || header.TargetCount > header.ChannelCount
			|| header.TimeCount == 0 || header.TimeCount > c_MaxKeys || header.ValueCount == 0 || header.ValueCount > c_MaxValues
			|| (static_cast<uint64_t>(header.ChannelCount) * sizeof(CookedChannel) + (header.TimeCount + header.ValueCount) * sizeof(float)) > reader.GetRemaining())
			return fail("Cooked animation clip header is corrupt");

		Ref<AnimationClip> clip(new AnimationClip());
		clip->m_Name = reader.ReadString(c_MaxNameLength);
		std::unordered_set<std::string> targetNames;
		clip->m_TargetNames.resize(header.TargetCount);
		for (std::string& target : clip->m_TargetNames)
		{
			target = reader.ReadString(c_MaxNameLength);
			if (!reader.IsValid() || target.empty() || !targetNames.insert(target).second)
				return fail("Cooked animation clip target names are corrupt");
		}

		clip->m_Channels.resize(header.ChannelCount);
		for (AnimationChannel& channel : clip->m_Channels)
		{
			const CookedChannel cooked = reader.Read<CookedChannel>();
			if (!IsValidPath(cooked.Path) || !IsValidInterpolation(cooked.Interpolation) || cooked.Target >= header.TargetCount)
				return fail("Cooked animation clip channels are corrupt");
			channel.Target = cooked.Target;
			channel.Path = static_cast<AnimationPath>(cooked.Path);
			channel.Interpolation = static_cast<AnimationInterpolation>(cooked.Interpolation);
			channel.KeyCount = cooked.KeyCount;
			channel.FirstTime = cooked.FirstTime;
			channel.FirstValue = cooked.FirstValue;
		}
		clip->m_Times.resize(header.TimeCount);
		clip->m_Values.resize(header.ValueCount);
		reader.ReadBytes(clip->m_Times.data(), clip->m_Times.size() * sizeof(float));
		reader.ReadBytes(clip->m_Values.data(), clip->m_Values.size() * sizeof(float));
		if (!reader.IsValid())
			return fail("Cooked animation clip is truncated");
		if (reader.GetRemaining() != 0)
			return fail("Cooked animation clip has trailing data");
		if (!ValidValues(clip->m_Values))
			return fail("Cooked animation clip values are not finite");

		std::unordered_set<uint64_t> animated;
		float duration = 0.0f;
		for (const AnimationChannel& channel : clip->m_Channels)
		{
			const uint64_t stride = GetKeyStride(channel.Path, channel.Interpolation);
			if (channel.KeyCount == 0 || static_cast<uint64_t>(channel.FirstTime) + channel.KeyCount > clip->m_Times.size()
				|| static_cast<uint64_t>(channel.FirstValue) + channel.KeyCount * stride > clip->m_Values.size())
				return fail("A cooked animation clip channel lies outside the clip's data");
			const std::span<const float> times(clip->m_Times.data() + channel.FirstTime, channel.KeyCount);
			if (!ValidTimes(times))
				return fail("A cooked animation clip channel has invalid key times");
			if (!animated.insert((static_cast<uint64_t>(channel.Target) << 2) | static_cast<uint64_t>(channel.Path)).second)
				return fail("Two cooked animation clip channels animate the same part of a joint");
			if (channel.Path == AnimationPath::Rotation)
				NormalizeRotations(channel.Interpolation, channel.KeyCount, clip->m_Values.data() + channel.FirstValue);
			duration = std::max(duration, times.back());
		}
		if (duration != header.Duration)
			return fail("Cooked animation clip duration does not match its keys");
		clip->m_Duration = duration;
		return clip;
	}

	AssetMemoryUsage AnimationClip::GetMemoryUsage() const
	{
		AssetMemoryUsage usage;
		usage.Cpu = sizeof(AnimationClip) + m_Name.capacity() + m_Channels.size() * sizeof(AnimationChannel) + (m_Times.size() + m_Values.size()) * sizeof(float);
		for (const std::string& target : m_TargetNames)
			usage.Cpu += sizeof(std::string) + target.capacity();
		return usage;
	}

	uint64_t AnimationClip::GetKeyCount() const
	{
		uint64_t keys = 0;
		for (const AnimationChannel& channel : m_Channels)
			keys += channel.KeyCount;
		return keys;
	}

	AnimationClipBinding AnimationClip::Bind(const Skeleton& skeleton) const
	{
		AnimationClipBinding binding;
		std::vector<JointIndex> targetJoints(m_TargetNames.size());
		for (size_t target = 0; target < m_TargetNames.size(); target++)
			targetJoints[target] = skeleton.FindJoint(m_TargetNames[target]);
		binding.ChannelJoints.resize(m_Channels.size());
		for (size_t index = 0; index < m_Channels.size(); index++)
		{
			const AnimationChannel& channel = m_Channels[index];
			const JointIndex joint = targetJoints[channel.Target];
			binding.ChannelJoints[index] = joint;
			if (joint == c_NoJoint)
				continue;
			binding.BoundChannels++;
			if (joint == skeleton.GetRootMotionJoint())
				binding.RootMotionChannels[static_cast<size_t>(channel.Path)] = static_cast<int32_t>(index);
		}
		return binding;
	}

	uint32_t AnimationClip::FindKey(const AnimationChannel& channel, float time) const
	{
		// The last key whose time is not after `time` (the caller handles times outside the keys).
		const float* begin = m_Times.data() + channel.FirstTime;
		const float* end = begin + channel.KeyCount;
		const float* next = std::upper_bound(begin, end, time);
		return static_cast<uint32_t>(std::max<std::ptrdiff_t>(next - begin - 1, 0));
	}

	void AnimationClip::SampleChannel(const AnimationChannel& channel, float time, JointPose& pose) const
	{
		const uint32_t components = GetComponentCount(channel.Path);
		const uint32_t stride = GetKeyStride(channel.Path, channel.Interpolation);
		const bool cubic = channel.Interpolation == AnimationInterpolation::CubicSpline;
		const float* values = m_Values.data() + channel.FirstValue;
		const float* times = m_Times.data() + channel.FirstTime;
		auto valueOf = [&](uint32_t key) { return values + static_cast<size_t>(key) * stride + (cubic ? components : 0u); };

		float result[4];
		const uint32_t last = channel.KeyCount - 1;
		if (channel.KeyCount == 1 || !(time > times[0]) || time >= times[last])
		{
			const float* value = valueOf(channel.KeyCount == 1 || !(time > times[0]) ? 0u : last);
			std::copy(value, value + components, result);
		}
		else
		{
			const uint32_t key = std::min(FindKey(channel, time), last - 1);
			const float keyTime = times[key];
			const float span = times[key + 1] - keyTime;
			const float s = std::clamp((time - keyTime) / span, 0.0f, 1.0f);
			const float* from = valueOf(key);
			const float* to = valueOf(key + 1);
			switch (channel.Interpolation)
			{
				case AnimationInterpolation::Step:
					std::copy(from, from + components, result);
					break;
				case AnimationInterpolation::Linear:
					if (channel.Path == AnimationPath::Rotation)
					{
						const glm::quat rotation = AnimationMath::Slerp(ReadRotation(from), ReadRotation(to), s);
						result[0] = rotation.x;
						result[1] = rotation.y;
						result[2] = rotation.z;
						result[3] = rotation.w;
					}
					else
					{
						for (uint32_t component = 0; component < components; component++)
							result[component] = from[component] + (to[component] - from[component]) * s;
					}
					break;
				case AnimationInterpolation::CubicSpline:
				{
					// glTF's cubic Hermite spline: the key's out-tangent and the next key's in-tangent, scaled by the span.
					const float* outTangent = values + static_cast<size_t>(key) * stride + 2u * components;
					const float* inTangent = values + static_cast<size_t>(key + 1) * stride;
					const float s2 = s * s;
					const float s3 = s2 * s;
					const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
					const float h10 = (s3 - 2.0f * s2 + s) * span;
					const float h01 = -2.0f * s3 + 3.0f * s2;
					const float h11 = (s3 - s2) * span;
					for (uint32_t component = 0; component < components; component++)
						result[component] = h00 * from[component] + h10 * outTangent[component] + h01 * to[component] + h11 * inTangent[component];
					break;
				}
			}
		}

		switch (channel.Path)
		{
			case AnimationPath::Translation: pose.Translation = ReadVector(result); break;
			case AnimationPath::Scale:       pose.Scale = ReadVector(result); break;
			case AnimationPath::Rotation:    pose.Rotation = AnimationMath::Normalize(ReadRotation(result)); break;
		}
	}

	void AnimationClip::Sample(const AnimationClipBinding& binding, float time, std::span<JointPose> pose) const
	{
		const size_t count = std::min(binding.ChannelJoints.size(), m_Channels.size());
		for (size_t index = 0; index < count; index++)
		{
			const JointIndex joint = binding.ChannelJoints[index];
			if (joint < pose.size())
				SampleChannel(m_Channels[index], time, pose[joint]);
		}
	}

	JointPose AnimationClip::SampleRootMotionJoint(const AnimationClipBinding& binding, float time, const JointPose& restPose) const
	{
		JointPose pose = restPose;
		for (const int32_t channel : binding.RootMotionChannels)
		{
			if (channel >= 0 && static_cast<size_t>(channel) < m_Channels.size())
				SampleChannel(m_Channels[static_cast<size_t>(channel)], time, pose);
		}
		return pose;
	}

}
