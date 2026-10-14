#include "stpch.h"
#include "Strata/Animation/AnimatorInstance.h"

#include "Strata/Animation/RestPose.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace Strata
{

	namespace
	{

		// The heading of a rotation: the angle about +Y of where it turns +Z (0 for rotations that keep +Z pointing up or down).
		float GetYaw(const glm::quat& rotation)
		{
			const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, 1.0f);
			if (std::abs(forward.x) < 1.0e-6f && std::abs(forward.z) < 1.0e-6f)
				return 0.0f;
			return std::atan2(forward.x, forward.z);
		}

		// The shortest signed angle from a to b.
		float AngleDifference(float a, float b)
		{
			float difference = std::fmod(b - a, glm::two_pi<float>());
			if (difference > glm::pi<float>())
				difference -= glm::two_pi<float>();
			else if (difference < -glm::pi<float>())
				difference += glm::two_pi<float>();
			return difference;
		}

		glm::vec3 RotateY(const glm::vec3& vector, float angle)
		{
			const float cosine = std::cos(angle);
			const float sine = std::sin(angle);
			return glm::vec3(cosine * vector.x + sine * vector.z, vector.y, -sine * vector.x + cosine * vector.z);
		}

		// Where the root motion joint is in the animator's space at a time of a clip, and its heading relative to the clip's
		// start.
		struct RootSample
		{
			glm::vec3 Position = glm::vec3(0.0f);
			glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		};

		RootSample SampleRoot(const AnimatorClip& clip, const Skeleton& skeleton, const RootMotionSetup& setup, float time)
		{
			const JointIndex root = skeleton.GetRootMotionJoint();
			const JointPose pose = clip.Clip->SampleRootMotionJoint(*clip.Binding, time, skeleton.GetRestPose()[root]);
			RootSample sample;
			sample.Position = glm::vec3(setup.AncestorModel * glm::vec4(pose.Translation, 1.0f));
			sample.Rotation = setup.AncestorRotation * pose.Rotation;
			return sample;
		}

		// Root motion moves the entity, so the pose holds the root's subtree back: moved by -offset in the plane and turned by
		// -yaw about the root's position.
		void HoldRoot(std::vector<glm::mat4>& model, const RootMotionSetup& setup, JointIndex root, const glm::vec3& offset, float yaw)
		{
			const glm::vec3 pivot(model[root][3].x, 0.0f, model[root][3].z);
			const glm::mat4 correction = glm::translate(glm::mat4(1.0f), pivot - offset) * glm::rotate(glm::mat4(1.0f), -yaw, glm::vec3(0.0f, 1.0f, 0.0f))
				* glm::translate(glm::mat4(1.0f), -pivot);
			for (size_t joint = 0; joint < model.size(); joint++)
			{
				if (setup.InSubtree[joint])
					model[joint] = AnimationMath::MultiplyAffine(correction, model[joint]);
			}
		}

		// The largest scale a transform applies along any axis.
		float GetMaxScale(const glm::mat4& transform)
		{
			return std::max({ glm::length(glm::vec3(transform[0])), glm::length(glm::vec3(transform[1])), glm::length(glm::vec3(transform[2])) });
		}

		// The motion of the root from time a to time b of one pass through a clip, in the frame the root faced at a.
		struct RootTravel
		{
			glm::vec3 Translation = glm::vec3(0.0f);
			float Yaw = 0.0f;

			// Appends a stretch that starts where this one ends.
			void Append(const RootSample& from, const RootSample& to, const glm::quat& clipStart)
			{
				const float fromYaw = GetYaw(from.Rotation * glm::inverse(clipStart));
				const float toYaw = GetYaw(to.Rotation * glm::inverse(clipStart));
				const glm::vec3 local = RotateY(to.Position - from.Position, -fromYaw);
				Translation += RotateY(local, Yaw);
				Yaw += AngleDifference(fromYaw, toYaw);
			}
		};

	}

	RootMotionSetup RootMotionSetup::Create(const Skeleton& skeleton)
	{
		RootMotionSetup setup;
		const JointIndex root = skeleton.GetRootMotionJoint();
		// The ancestors' rest pose: they usually only orient the skeleton (an "Armature" node).
		std::vector<JointIndex> chain;
		for (JointIndex joint = skeleton.GetParent(root); joint != c_NoJoint; joint = skeleton.GetParent(joint))
			chain.push_back(joint);
		for (auto it = chain.rbegin(); it != chain.rend(); ++it)
		{
			const JointPose& pose = skeleton.GetRestPose()[*it];
			setup.AncestorModel = AnimationMath::MultiplyAffine(setup.AncestorModel, AnimationMath::ToMatrix(pose));
			setup.AncestorRotation = setup.AncestorRotation * pose.Rotation;
		}
		setup.InSubtree.assign(skeleton.GetJointCount(), 0);
		setup.InSubtree[root] = 1;
		for (uint32_t joint = root + 1u; joint < skeleton.GetJointCount(); joint++)
		{
			const JointIndex parent = skeleton.GetParent(static_cast<JointIndex>(joint));
			setup.InSubtree[joint] = parent != c_NoJoint && setup.InSubtree[parent] ? 1 : 0;
		}
		return setup;
	}

	AnimationReach AnimationReach::Compute(const AnimationClip& clip, const AnimationClipBinding& binding, const Skeleton& skeleton, const RootMotionSetup& setup)
	{
		AnimationReach reach;
		const uint32_t joints = skeleton.GetJointCount();
		if (joints == 0 || setup.InSubtree.size() != joints)
			return reach;
		const float duration = clip.GetDuration();
		const uint32_t samples = duration > 0.0f ? std::clamp(static_cast<uint32_t>(std::ceil(duration * c_SamplesPerSecond)) + 1u, 2u, c_MaxSamples) : 1u;
		const AnimatorClip bound { &clip, &binding };
		const RootSample start = SampleRoot(bound, skeleton, setup, 0.0f);
		const JointIndex root = skeleton.GetRootMotionJoint();
		std::vector<JointPose> local;
		std::vector<glm::mat4> model(joints);
		for (uint32_t sample = 0; sample < samples; sample++)
		{
			const float time = samples > 1 ? duration * static_cast<float>(sample) / static_cast<float>(samples - 1) : 0.0f;
			local = skeleton.GetRestPose();
			clip.Sample(binding, time, local);
			skeleton.ComputeModelTransforms(local, model);
			for (const glm::mat4& transform : model)
			{
				reach.Bounds.Expand(glm::vec3(transform[3]));
				reach.MaxScale = std::max(reach.MaxScale, GetMaxScale(transform));
			}
			// As the pose holds the root with root motion (Evaluate).
			const RootSample now = SampleRoot(bound, skeleton, setup, time);
			glm::vec3 offset = now.Position - start.Position;
			offset.y = 0.0f;
			HoldRoot(model, setup, root, offset, GetYaw(now.Rotation * glm::inverse(start.Rotation)));
			for (const glm::mat4& transform : model)
				reach.RootMotionBounds.Expand(glm::vec3(transform[3]));
		}
		// Transforms that overflowed would make the boxes useless for culling: no reach then.
		auto finite = [](const AABB& box)
		{
			return box.IsValid() && std::isfinite(box.Min.x) && std::isfinite(box.Min.y) && std::isfinite(box.Min.z) && std::isfinite(box.Max.x)
				&& std::isfinite(box.Max.y) && std::isfinite(box.Max.z);
		};
		if (!std::isfinite(reach.MaxScale) || !finite(reach.Bounds) || !finite(reach.RootMotionBounds))
			return AnimationReach();
		return reach;
	}

	void AnimatorInstance::Reset()
	{
		m_PendingState.reset();
		m_PendingFade = 0.0f;
		m_Count = 0;
		m_PlayRevision++;
		m_FadeElapsed = 0.0f;
		m_FadeDuration = 0.0f;
		m_Playing = false;
		m_StartPending = false;
		m_PlayedClip = UUID::Null();
		m_BoundControllerId = 0;
		m_ParameterNames.clear();
		m_ParameterTypes.clear();
		m_Parameters.clear();
		m_PendingParameters.clear();
		m_RootMotion = {};
	}

	AnimatorClip AnimatorInstance::GetClip(const AnimatorAssets& assets, uint32_t state) const
	{
		if (state == c_ClipState)
		{
			if (m_PlayedClip.IsValid())
				return assets.PlayedClip;
			return assets.Controller || assets.StateClips.empty() ? AnimatorClip() : assets.StateClips[0];
		}
		return state < assets.StateClips.size() ? assets.StateClips[state] : AnimatorClip();
	}

	float AnimatorInstance::GetStateSpeed(const AnimatorAssets& assets, uint32_t state) const
	{
		if (state == c_ClipState || !assets.Controller || state >= assets.Controller->GetStates().size())
			return 1.0f;
		return assets.Controller->GetStates()[state].Speed;
	}

	bool AnimatorInstance::IsStateLooping(const AnimatorAssets& assets, const AnimatorSettings& settings, uint32_t state) const
	{
		if (state == c_ClipState || !assets.Controller || state >= assets.Controller->GetStates().size())
			return settings.Loop;
		return assets.Controller->GetStates()[state].Loop;
	}

	void AnimatorInstance::BindParameters(const AnimationController& controller, uint64_t controllerId)
	{
		const std::vector<AnimationParameter>& parameters = controller.GetParameters();
		if (m_BoundControllerId == controllerId && controllerId != 0 && m_Parameters.size() == parameters.size())
			return;
		// A controller seen for the first time (or reloaded): its defaults, the values of the previous version's parameters
		// with the same name and type, then what was set by name meanwhile.
		std::vector<float> values(parameters.size());
		std::vector<std::string> names(parameters.size());
		std::vector<AnimationParameterType> types(parameters.size());
		for (size_t index = 0; index < parameters.size(); index++)
		{
			const AnimationParameter& parameter = parameters[index];
			names[index] = parameter.Name;
			types[index] = parameter.Type;
			values[index] = parameter.Type == AnimationParameterType::Float ? parameter.DefaultFloat : (parameter.Type == AnimationParameterType::Bool && parameter.DefaultBool ? 1.0f : 0.0f);
			for (size_t old = 0; old < m_ParameterNames.size(); old++)
			{
				if (m_ParameterNames[old] == parameter.Name && m_ParameterTypes[old] == parameter.Type)
					values[index] = m_Parameters[old];
			}
		}
		m_Parameters = std::move(values);
		m_ParameterNames = std::move(names);
		m_ParameterTypes = std::move(types);
		m_BoundControllerId = controllerId;
		for (const PendingParameter& pending : m_PendingParameters)
		{
			const uint32_t index = controller.FindParameter(pending.Name);
			if (index != ~0u && parameters[index].Type == pending.Type)
				m_Parameters[index] = pending.Value;
		}
		m_PendingParameters.clear();
	}

	void AnimatorInstance::StartState(uint32_t state, float fadeSeconds)
	{
		// Every change of what plays (and of the clip played directly, which callers set before starting it) comes through
		// here, Reset or the end of a cross-fade.
		m_PlayRevision++;
		Playing entry;
		entry.State = state;
		if (fadeSeconds <= 0.0f || m_Count == 0)
		{
			m_Entries[0] = entry;
			m_Count = 1;
			m_FadeElapsed = 0.0f;
			m_FadeDuration = 0.0f;
			return;
		}
		// The entries playing now keep their current shares of what fades out.
		for (uint32_t index = 0; index < m_Count; index++)
			m_Entries[index].StartWeight = m_Entries[index].Weight;
		if (m_Count == c_MaxPlaying)
		{
			// The faintest entry makes room; the others share its weight.
			uint32_t faintest = 0;
			for (uint32_t index = 1; index < m_Count; index++)
			{
				if (m_Entries[index].StartWeight < m_Entries[faintest].StartWeight)
					faintest = index;
			}
			const float dropped = m_Entries[faintest].StartWeight;
			std::move(m_Entries.begin() + faintest + 1, m_Entries.begin() + m_Count, m_Entries.begin() + faintest);
			m_Count--;
			if (dropped < 1.0f)
			{
				for (uint32_t index = 0; index < m_Count; index++)
					m_Entries[index].StartWeight /= 1.0f - dropped;
			}
		}
		entry.StartWeight = 0.0f;
		entry.Weight = 0.0f;
		m_Entries[m_Count++] = entry;
		m_FadeElapsed = 0.0f;
		m_FadeDuration = fadeSeconds;
	}

	bool AnimatorInstance::Play(const AnimatorAssets& assets, std::string_view state)
	{
		if (!assets.Controller && assets.ControllerLoading)
		{
			// Played once the controller is loaded (Advance).
			m_PendingState = std::string(state);
			m_PendingFade = 0.0f;
			m_Playing = true;
			m_StartPending = false;
			return true;
		}
		if (!assets.Controller)
			return false;
		const uint32_t index = assets.Controller->FindState(state);
		if (index == ~0u)
			return false;
		m_PlayedClip = UUID::Null();
		m_PendingState.reset();
		m_Playing = true;
		m_StartPending = false;
		BindParameters(*assets.Controller, assets.ControllerId);
		StartState(index, 0.0f);
		return true;
	}

	bool AnimatorInstance::CrossFade(const AnimatorAssets& assets, std::string_view state, float seconds)
	{
		if (!std::isfinite(seconds) || seconds < 0.0f)
			return false;
		if (!assets.Controller && assets.ControllerLoading)
		{
			m_PendingState = std::string(state);
			m_PendingFade = seconds;
			m_Playing = true;
			if (m_Count == 0)
				m_StartPending = false;
			return true;
		}
		if (!assets.Controller)
			return false;
		const uint32_t index = assets.Controller->FindState(state);
		if (index == ~0u)
			return false;
		m_PlayedClip = UUID::Null();
		m_PendingState.reset();
		m_Playing = true;
		BindParameters(*assets.Controller, assets.ControllerId);
		if (m_StartPending)
		{
			// Nothing played yet: there is nothing to fade from.
			m_StartPending = false;
			StartState(index, 0.0f);
			return true;
		}
		StartState(index, std::min(seconds, AnimationController::c_MaxDuration));
		return true;
	}

	void AnimatorInstance::PlayClip(AssetHandle clip)
	{
		m_PlayedClip = clip;
		m_PendingState.reset();
		m_Playing = true;
		m_StartPending = false;
		StartState(c_ClipState, 0.0f);
	}

	void AnimatorInstance::SetPending(std::string_view name, AnimationParameterType type, float value)
	{
		// The latest value per name waits (a script setting it every frame while the controller loads keeps one entry).
		for (PendingParameter& pending : m_PendingParameters)
		{
			if (pending.Name == name)
			{
				pending.Type = type;
				pending.Value = value;
				return;
			}
		}
		m_PendingParameters.push_back(PendingParameter { std::string(name), type, value });
	}

	bool AnimatorInstance::SetParameter(const AnimatorAssets& assets, std::string_view name, AnimationParameterType type, float value)
	{
		if (!assets.Controller)
		{
			if (!assets.ControllerLoading || name.empty() || name.size() > AnimationController::c_MaxNameLength)
				return false;
			SetPending(name, type, value);
			return true;
		}
		BindParameters(*assets.Controller, assets.ControllerId);
		const uint32_t index = assets.Controller->FindParameter(name);
		if (index == ~0u || assets.Controller->GetParameters()[index].Type != type)
			return false;
		m_Parameters[index] = value;
		return true;
	}

	bool AnimatorInstance::SetFloat(const AnimatorAssets& assets, std::string_view name, float value)
	{
		if (!std::isfinite(value))
			return false;
		return SetParameter(assets, name, AnimationParameterType::Float, value);
	}

	bool AnimatorInstance::SetBool(const AnimatorAssets& assets, std::string_view name, bool value)
	{
		return SetParameter(assets, name, AnimationParameterType::Bool, value ? 1.0f : 0.0f);
	}

	bool AnimatorInstance::SetTrigger(const AnimatorAssets& assets, std::string_view name, bool set)
	{
		return SetParameter(assets, name, AnimationParameterType::Trigger, set ? 1.0f : 0.0f);
	}

	float AnimatorInstance::GetFloat(std::string_view name) const
	{
		for (const PendingParameter& pending : m_PendingParameters)
		{
			if (pending.Name == name)
				return pending.Value;
		}
		for (size_t index = 0; index < m_ParameterNames.size(); index++)
		{
			if (m_ParameterNames[index] == name)
				return m_Parameters[index];
		}
		return 0.0f;
	}

	bool AnimatorInstance::GetBool(std::string_view name) const
	{
		return GetFloat(name) != 0.0f;
	}

	bool AnimatorInstance::ConditionsHold(const AnimationTransition& transition) const
	{
		for (const AnimationCondition& condition : transition.Conditions)
		{
			const float value = condition.Parameter < m_Parameters.size() ? m_Parameters[condition.Parameter] : 0.0f;
			bool holds = false;
			switch (condition.Mode)
			{
				case AnimationConditionMode::Greater: holds = value > condition.Threshold; break;
				case AnimationConditionMode::Less:    holds = value < condition.Threshold; break;
				case AnimationConditionMode::True:
				case AnimationConditionMode::Set:     holds = value != 0.0f; break;
				case AnimationConditionMode::False:   holds = value == 0.0f; break;
			}
			if (!holds)
				return false;
		}
		return true;
	}

	void AnimatorInstance::ConsumeTriggers(const AnimationTransition& transition)
	{
		for (const AnimationCondition& condition : transition.Conditions)
		{
			if (condition.Mode == AnimationConditionMode::Set && condition.Parameter < m_Parameters.size())
				m_Parameters[condition.Parameter] = 0.0f;
		}
	}

	int64_t AnimatorInstance::AdvanceEntry(Playing& entry, float seconds, const AnimatorAssets& assets, const AnimatorSettings& settings, bool fireEvents,
		std::vector<AnimatorFiredEvent>& outEvents) const
	{
		const AnimatorClip clip = GetClip(assets, entry.State);
		const float duration = clip.Clip ? clip.Clip->GetDuration() : 0.0f;
		if (!(duration > 0.0f))
		{
			// Nothing to play (no clip, or a single pose): it is done at once.
			entry.Time = 0.0f;
			entry.Loops = 0;
			entry.Normalized = 1.0;
			return 0;
		}
		const bool loop = IsStateLooping(assets, settings, entry.State);
		const float start = entry.Time;
		const bool fresh = entry.Normalized == 0.0 && entry.Time == 0.0f;

		// Events of the state the animator is in, in the order the playhead passes them (only controller states have events).
		const std::vector<AnimationEvent>* events = nullptr;
		if (fireEvents && entry.State != c_ClipState && assets.Controller && entry.State < assets.Controller->GetStates().size())
		{
			events = &assets.Controller->GetStates()[entry.State].Events;
			if (events->empty())
				events = nullptr;
		}
		auto fire = [&](float from, float to, bool includeFrom)
		{
			// Events in (from, to] forward or [to, from) backward; includeFrom also takes an event at `from` itself.
			if (!events)
				return;
			if (to >= from)
			{
				for (uint32_t index = 0; index < events->size(); index++)
				{
					const float time = (*events)[index].Time;
					if ((time > from || (includeFrom && time == from)) && time <= to)
						outEvents.push_back(AnimatorFiredEvent { entry.State, index, time });
				}
			}
			else
			{
				for (uint32_t index = static_cast<uint32_t>(events->size()); index > 0; index--)
				{
					const float time = (*events)[index - 1].Time;
					if ((time < from || (includeFrom && time == from)) && time >= to)
						outEvents.push_back(AnimatorFiredEvent { entry.State, index - 1, time });
				}
			}
		};

		if (!loop)
		{
			const float end = std::clamp(start + seconds, 0.0f, duration);
			fire(start, end, fresh);
			entry.Time = end;
			entry.Normalized = end / duration;
			return 0;
		}

		// The passes through the clip this update makes, the earliest skipped beyond c_MaxEventLoops loops. Every crossing of
		// the loop boundary is counted where the playhead makes it.
		int64_t crossings = 0;
		float remaining = seconds;
		float position = start;
		bool includeFrom = fresh;
		const float maxSpan = duration * static_cast<float>(c_MaxEventLoops);
		if (std::abs(remaining) > maxSpan)
		{
			const float skipped = std::abs(remaining) - maxSpan;
			const float skippedInClip = std::fmod(skipped, duration);
			// The whole loops in what is skipped (what fmod leaves out is a whole number of durations, up to rounding), then the
			// part of a loop.
			const int64_t wholeLoops = static_cast<int64_t>(std::round((skipped - skippedInClip) / duration));
			if (remaining > 0.0f)
			{
				crossings += wholeLoops + (position + skippedInClip >= duration ? 1 : 0);
				position = std::fmod(position + skippedInClip, duration);
			}
			else
			{
				crossings -= wholeLoops + (position - skippedInClip < 0.0f ? 1 : 0);
				position = std::fmod(position - skippedInClip + duration, duration);
			}
			remaining = remaining > 0.0f ? maxSpan : -maxSpan;
			includeFrom = false;
		}
		while (remaining != 0.0f)
		{
			if (remaining > 0.0f)
			{
				const float step = std::min(remaining, duration - position);
				fire(position, position + step, includeFrom);
				position += step;
				remaining -= step;
				if (remaining > 0.0f || position >= duration)
				{
					// Back to the start: an event at time 0 fires again.
					position = 0.0f;
					includeFrom = true;
					crossings++;
					if (remaining <= 0.0f)
						break;
					continue;
				}
			}
			else
			{
				const float step = std::max(remaining, -position);
				fire(position, position + step, includeFrom);
				position += step;
				remaining -= step;
				if (remaining < 0.0f || position <= 0.0f)
				{
					position = duration;
					includeFrom = true;
					// Ending exactly at the start is no crossing (the playhead stays there); going on past it is.
					if (remaining >= 0.0f)
						break;
					crossings--;
					continue;
				}
			}
			includeFrom = false;
		}
		// Ending exactly on a loop boundary leaves the playhead at the start.
		entry.Time = position >= duration ? 0.0f : position;
		entry.Loops += crossings;
		// Normalized time counts loops forward; playing backwards before the first loop keeps it at the start.
		entry.Normalized = std::max(0.0, static_cast<double>(entry.Loops) + static_cast<double>(entry.Time) / static_cast<double>(duration));
		return crossings;
	}

	void AnimatorInstance::UpdateWeights()
	{
		if (m_Count == 0)
			return;
		const float progress = m_FadeDuration > 0.0f ? std::clamp(m_FadeElapsed / m_FadeDuration, 0.0f, 1.0f) : 1.0f;
		if (progress >= 1.0f)
		{
			// The fade is over: only the target plays. (Without a fade this runs every update: what plays changes only when
			// entries go.)
			if (m_Count != 1)
				m_PlayRevision++;
			m_Entries[0] = m_Entries[m_Count - 1];
			m_Entries[0].Weight = 1.0f;
			m_Entries[0].StartWeight = 1.0f;
			m_Count = 1;
			m_FadeDuration = 0.0f;
			m_FadeElapsed = 0.0f;
			return;
		}
		for (uint32_t index = 0; index + 1 < m_Count; index++)
			m_Entries[index].Weight = (1.0f - progress) * m_Entries[index].StartWeight;
		m_Entries[m_Count - 1].Weight = progress;
	}

	bool AnimatorInstance::Advance(float deltaTime, const AnimatorAssets& assets, const AnimatorSettings& settings, std::vector<AnimatorFiredEvent>& outEvents)
	{
		m_RootMotion = {};
		if (!assets.SkeletonAsset || assets.ControllerLoading)
			return false;
		if (assets.Controller)
			BindParameters(*assets.Controller, assets.ControllerId);
		if (assets.Controller && m_PendingState)
		{
			// A command given while the controller loaded.
			const uint32_t state = assets.Controller->FindState(*m_PendingState);
			if (state != ~0u)
			{
				m_PlayedClip = UUID::Null();
				m_StartPending = false;
				StartState(state, m_Count == 0 ? 0.0f : m_PendingFade);
			}
			m_PendingState.reset();
		}
		if (m_StartPending)
		{
			m_StartPending = false;
			if (m_PlayedClip.IsValid() || !assets.Controller)
				StartState(c_ClipState, 0.0f);
			else
				StartState(assets.Controller->GetDefaultState(), 0.0f);
		}
		if (!m_Playing || m_Count == 0)
			return true;
		// A controller that changed under the animator (a reload with other states) starts over at its default state.
		for (uint32_t index = 0; index < m_Count; index++)
		{
			const uint32_t state = m_Entries[index].State;
			if (state != c_ClipState && (!assets.Controller || state >= assets.Controller->GetStates().size()))
			{
				m_Count = 0;
				StartState(assets.Controller ? assets.Controller->GetDefaultState() : c_ClipState, 0.0f);
				break;
			}
		}

		const float scaled = deltaTime * settings.Speed;
		const bool rootMotion = settings.ApplyRootMotion && assets.RootMotion && assets.SkeletonAsset->GetJointCount() == assets.RootMotion->InSubtree.size();
		std::array<RootTravel, c_MaxPlaying> travel = {};
		const uint32_t current = m_Count - 1;
		const double previousNormalized = m_Entries[current].Normalized;
		for (uint32_t index = 0; index < m_Count; index++)
		{
			Playing& entry = m_Entries[index];
			const float seconds = scaled * GetStateSpeed(assets, entry.State);
			const float before = entry.Time;
			const int64_t crossings = AdvanceEntry(entry, seconds, assets, settings, index == current, outEvents);
			if (!rootMotion)
				continue;
			const AnimatorClip clip = GetClip(assets, entry.State);
			if (!clip.Clip || !clip.Binding || !(clip.Clip->GetDuration() > 0.0f))
				continue;
			// The root's travel through each pass of the clip this update made, split where the playhead crossed the loop
			// boundary (whole loops included).
			const Skeleton& skeleton = *assets.SkeletonAsset;
			const RootMotionSetup& setup = *assets.RootMotion;
			const float duration = clip.Clip->GetDuration();
			const glm::quat clipStart = SampleRoot(clip, skeleton, setup, 0.0f).Rotation;
			const RootSample from = SampleRoot(clip, skeleton, setup, before);
			const RootSample to = SampleRoot(clip, skeleton, setup, entry.Time);
			if (crossings == 0)
			{
				travel[index].Append(from, to, clipStart);
				continue;
			}
			const RootSample first = SampleRoot(clip, skeleton, setup, 0.0f);
			const RootSample last = SampleRoot(clip, skeleton, setup, duration);
			const uint32_t whole = static_cast<uint32_t>(std::min<int64_t>((crossings > 0 ? crossings : -crossings) - 1, c_MaxRootMotionLoops));
			if (crossings > 0)
			{
				// On to the end, whole loops, then from the start.
				travel[index].Append(from, last, clipStart);
				for (uint32_t loopIndex = 0; loopIndex < whole; loopIndex++)
					travel[index].Append(first, last, clipStart);
				travel[index].Append(first, to, clipStart);
			}
			else
			{
				// Backwards: down to the start, whole loops back, then on from the end.
				travel[index].Append(from, first, clipStart);
				for (uint32_t loopIndex = 0; loopIndex < whole; loopIndex++)
					travel[index].Append(last, first, clipStart);
				travel[index].Append(last, to, clipStart);
			}
		}

		m_FadeElapsed += std::abs(scaled);
		UpdateWeights();
		if (rootMotion)
		{
			// Weighted by the shares after the update; the move stays in the plane (vertical motion stays in the pose).
			// The entries before UpdateWeights may have been dropped: travel was recorded by the old indices, and a finished
			// fade keeps only the last entry, so its travel weighs 1.
			if (m_Count == 1)
			{
				m_RootMotion.Translation = travel[current].Translation;
				m_RootMotion.Yaw = travel[current].Yaw;
			}
			else
			{
				for (uint32_t index = 0; index < m_Count; index++)
				{
					m_RootMotion.Translation += travel[index].Translation * m_Entries[index].Weight;
					m_RootMotion.Yaw += travel[index].Yaw * m_Entries[index].Weight;
				}
			}
			m_RootMotion.Translation.y = 0.0f;
		}

		// The state machine: transitions from any state first, then from the state the animator is in; one fires at most.
		const AnimationController* controller = assets.Controller;
		if (!controller || !settings.Transitions || m_PlayedClip.IsValid() || m_Count == 0)
			return true;
		const Playing& playing = m_Entries[m_Count - 1];
		if (playing.State == c_ClipState)
			return true;
		const bool looping = IsStateLooping(assets, settings, playing.State);
		const double normalized = playing.Normalized;
		auto exitReached = [&](const AnimationTransition& transition)
		{
			if (!transition.ExitTime)
				return true;
			const double exitTime = *transition.ExitTime;
			if (looping && exitTime < 1.0)
			{
				// Every loop at that point: the update passed exitTime + k for some whole k.
				return std::floor(normalized - exitTime) > std::floor(previousNormalized - exitTime) || (previousNormalized == 0.0 && normalized >= exitTime && exitTime == 0.0);
			}
			return normalized >= exitTime;
		};
		auto tryTransitions = [&](std::span<const uint32_t> transitions)
		{
			for (const uint32_t index : transitions)
			{
				const AnimationTransition& transition = controller->GetTransitions()[index];
				if (transition.From == AnimationTransition::c_AnyState && transition.To == playing.State && !transition.CanTransitionToSelf)
					continue;
				if (!exitReached(transition) || !ConditionsHold(transition))
					continue;
				ConsumeTriggers(transition);
				StartState(transition.To, transition.Duration);
				return true;
			}
			return false;
		};
		if (!tryTransitions(controller->GetAnyStateTransitions()))
			tryTransitions(controller->GetStateTransitions(playing.State));
		return true;
	}

	bool AnimatorInstance::Evaluate(const AnimatorAssets& assets, const AnimatorSettings& settings, std::vector<JointPose>& scratch, std::vector<glm::mat4>& outModel) const
	{
		if (!assets.SkeletonAsset)
			return false;
		const Skeleton& skeleton = *assets.SkeletonAsset;
		// Nothing plays: the rest pose, computed the way the editor computes it for scenes that do not play.
		if (m_Count == 0)
		{
			ComputeRestPose(skeleton, outModel);
			return true;
		}
		const uint32_t joints = skeleton.GetJointCount();
		const std::vector<JointPose>& rest = skeleton.GetRestPose();
		scratch.resize(static_cast<size_t>(joints) * 2);
		const std::span<JointPose> blended(scratch.data(), joints);
		const std::span<JointPose> sample(scratch.data() + joints, joints);
		outModel.resize(joints);

		if (m_Count == 1)
		{
			std::copy(rest.begin(), rest.end(), blended.begin());
			const AnimatorClip clip = GetClip(assets, m_Entries[0].State);
			if (clip.Clip && clip.Binding)
				clip.Clip->Sample(*clip.Binding, m_Entries[0].Time, blended);
		}
		else
		{
			// Weighted sums: translations and scales linearly, rotations by normalized weighted sum on the first entry's
			// hemisphere (each sample's rest pose fills the joints its clip does not animate).
			for (uint32_t joint = 0; joint < joints; joint++)
			{
				blended[joint].Translation = glm::vec3(0.0f);
				blended[joint].Scale = glm::vec3(0.0f);
				blended[joint].Rotation = glm::quat(0.0f, 0.0f, 0.0f, 0.0f);
			}
			for (uint32_t index = 0; index < m_Count; index++)
			{
				const Playing& entry = m_Entries[index];
				std::copy(rest.begin(), rest.end(), sample.begin());
				const AnimatorClip clip = GetClip(assets, entry.State);
				if (clip.Clip && clip.Binding)
					clip.Clip->Sample(*clip.Binding, entry.Time, sample);
				const float weight = entry.Weight;
				for (uint32_t joint = 0; joint < joints; joint++)
				{
					JointPose& target = blended[joint];
					const JointPose& source = sample[joint];
					target.Translation += source.Translation * weight;
					target.Scale += source.Scale * weight;
					const glm::quat& reference = index == 0 ? source.Rotation : target.Rotation;
					const float sign = glm::dot(reference, source.Rotation) < 0.0f ? -1.0f : 1.0f;
					target.Rotation = glm::quat(target.Rotation.w + source.Rotation.w * weight * sign, target.Rotation.x + source.Rotation.x * weight * sign,
						target.Rotation.y + source.Rotation.y * weight * sign, target.Rotation.z + source.Rotation.z * weight * sign);
				}
			}
			for (uint32_t joint = 0; joint < joints; joint++)
				blended[joint].Rotation = AnimationMath::Normalize(blended[joint].Rotation);
		}
		skeleton.ComputeModelTransforms(blended, outModel);

		const bool rootMotion = settings.ApplyRootMotion && assets.RootMotion && assets.RootMotion->InSubtree.size() == joints;
		if (!rootMotion)
			return true;
		// Root motion moves the entity; the pose keeps the root where (and facing how) each clip started: the root's subtree
		// is moved back by its weighted offset from the clips' starts.
		glm::vec3 offset(0.0f);
		float yaw = 0.0f;
		for (uint32_t index = 0; index < m_Count; index++)
		{
			const Playing& entry = m_Entries[index];
			const AnimatorClip clip = GetClip(assets, entry.State);
			if (!clip.Clip || !clip.Binding)
				continue;
			const RootSample start = SampleRoot(clip, skeleton, *assets.RootMotion, 0.0f);
			const RootSample now = SampleRoot(clip, skeleton, *assets.RootMotion, entry.Time);
			const float weight = m_Count == 1 ? 1.0f : entry.Weight;
			offset += (now.Position - start.Position) * weight;
			yaw += GetYaw(now.Rotation * glm::inverse(start.Rotation)) * weight;
		}
		offset.y = 0.0f;
		HoldRoot(outModel, *assets.RootMotion, skeleton.GetRootMotionJoint(), offset, yaw);
		return true;
	}

	AnimatorReach AnimatorInstance::GetReach(const AnimatorAssets& assets, const AnimatorSettings& settings) const
	{
		AnimatorReach reach;
		if (m_Count == 0)
			return reach;
		const bool rootMotion = settings.ApplyRootMotion && assets.RootMotion;
		for (uint32_t index = 0; index < m_Count; index++)
		{
			const AnimatorClip clip = GetClip(assets, m_Entries[index].State);
			if (!clip.Reach || !clip.Reach->Bounds.IsValid())
				return AnimatorReach();
			reach.Bounds.Expand(rootMotion ? clip.Reach->RootMotionBounds : clip.Reach->Bounds);
			reach.Scale = std::max(reach.Scale, clip.Reach->MaxScale);
		}
		return reach;
	}

}
