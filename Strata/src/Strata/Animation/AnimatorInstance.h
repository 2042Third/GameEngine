#pragma once

#include "Strata/Animation/AnimationClip.h"
#include "Strata/Animation/AnimationController.h"
#include "Strata/Animation/AnimationTypes.h"
#include "Strata/Animation/Skeleton.h"

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	// A clip as an animator plays it: the loaded clip and its binding to the animator's skeleton (null while not loaded).
	struct AnimatorClip
	{
		const AnimationClip* Clip = nullptr;
		const AnimationClipBinding* Binding = nullptr;
	};

	// What the root motion joint's motion is measured in: the model-space transform of its ancestors (from the rest pose)
	// and which joints lie in its subtree (the joints root motion moves). One per skeleton.
	struct RootMotionSetup
	{
		glm::mat4 AncestorModel = glm::mat4(1.0f);
		glm::quat AncestorRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		std::vector<uint8_t> InSubtree; // Per joint

		static RootMotionSetup Create(const Skeleton& skeleton);
	};

	// What the assets of an animator are now: the skeleton, the controller (null: the simple mode, which plays the
	// AnimatorComponent's clip) with a clip per controller state (simple mode: one), and the clip a script played directly
	// (PlayClip). The pointers are valid during the call they are passed to; the instance keeps none of them.
	struct AnimatorAssets
	{
		const Skeleton* SkeletonAsset = nullptr;
		const RootMotionSetup* RootMotion = nullptr;
		const AnimationController* Controller = nullptr;
		// Identifies the controller object: a new number whenever another object is passed (a reload replaces it), so the
		// instance knows when to bind its parameters again without keeping a pointer.
		uint64_t ControllerId = 0;
		bool ControllerLoading = false; // The animator has a controller that is not loaded yet: commands wait for it
		std::span<const AnimatorClip> StateClips;
		AnimatorClip PlayedClip;
	};

	// The settings of an AnimatorComponent an update reads.
	struct AnimatorSettings
	{
		float Speed = 1.0f;
		bool Loop = true; // The simple mode's clip and clips played directly
		bool ApplyRootMotion = false;
		bool Transitions = true; // false: the state machine does not run (a preview holds the state it shows)
	};

	// An event the playhead passed during an update, in the order they happened.
	struct AnimatorFiredEvent
	{
		uint32_t State = 0;      // Controller state (c_ClipState for clips played without one: no events)
		uint32_t Event = 0;      // Index into the state's events
		float Time = 0.0f;       // The event's time in its clip
	};

	// The motion of the root motion joint during an update, taken out of the pose: a move in the animator's space and a turn
	// about its +Y axis, which the animation system applies to the animator's entity (AnimationSystem::ApplyRootMotion).
	struct RootMotionDelta
	{
		glm::vec3 Translation = glm::vec3(0.0f); // In the animator's space at the start of the update (X and Z)
		float Yaw = 0.0f;                        // Radians about +Y
		bool IsZero() const { return Translation == glm::vec3(0.0f) && Yaw == 0.0f; }
	};

	// One animator's playback: what it plays (a controller's states, or a clip), its parameters, the cross-fades in progress
	// and the pose that follows. The animation system keeps one per AnimatorComponent; it knows nothing about scenes.
	//
	// A controller's state machine: the animator is in one state (the target of the latest transition), whose transitions
	// (and those from any state, checked first) are tested after every update; the first whose conditions hold, and whose
	// exit time the update reached, fires: triggers it tests are consumed (reset), and a cross-fade to its target starts.
	// Cross-fades blend up to c_MaxPlaying clips: the target's weight grows linearly over the duration while the others share
	// the rest in the proportions they had when the fade started, so the weights always add up to 1. Events fire from the
	// state the animator is in (not from states fading out), each time the playhead passes them, in time order, also across
	// a loop boundary.
	class AnimatorInstance
	{
	public:
		static constexpr uint32_t c_ClipState = ~0u; // The simple mode's clip, or a clip played directly
		static constexpr uint32_t c_MaxPlaying = 4;
		// Loops of one clip an update fires events for at most (a very short clip at a very large time step).
		static constexpr uint32_t c_MaxEventLoops = 4;

		struct Playing
		{
			uint32_t State = c_ClipState;
			float Time = 0.0f;      // Seconds into the clip, within [0, duration]
			// Looping states: the loop boundaries the playhead crossed, forward minus backward. Counted where Time wraps, so
			// the normalized time, exit times and root motion follow the playhead itself.
			int64_t Loops = 0;
			double Normalized = 0.0; // Loops plus Time over duration, at least 0 (non-looping states: Time over duration)
			float StartWeight = 1.0f; // The share it had when the latest cross-fade started (entries fading out)
			float Weight = 1.0f;
		};

		// Back to the start: no state, parameters at their defaults (on the next update), not playing.
		void Reset();

		// Starts the controller's default state (or the clip) at time 0 when the assets are known; until then it waits.
		void Start() { m_StartPending = true; m_Playing = true; }
		bool IsPlaying() const { return m_Playing; }

		// Script and editor commands (main thread, between updates). States are named; false for an unknown state or without
		// a controller (while the controller loads, the latest command waits for it). Play jumps, CrossFade blends over
		// `seconds`. PlayClip plays a clip directly (without a controller's transitions until a state is played again).
		bool Play(const AnimatorAssets& assets, std::string_view state);
		bool CrossFade(const AnimatorAssets& assets, std::string_view state, float seconds);
		void PlayClip(AssetHandle clip);
		AssetHandle GetPlayedClip() const { return m_PlayedClip; }

		// Parameters by name. While the controller loads the values wait and apply once it is known; false without a
		// controller, for a parameter the controller does not have (or of another type) and for non-finite floats.
		bool SetFloat(const AnimatorAssets& assets, std::string_view name, float value);
		bool SetBool(const AnimatorAssets& assets, std::string_view name, bool value);
		bool SetTrigger(const AnimatorAssets& assets, std::string_view name, bool set);
		// The value of a parameter (bools and triggers: 0 or 1), also one still waiting for the controller; 0 for unknown names.
		float GetFloat(std::string_view name) const;
		bool GetBool(std::string_view name) const;

		// Advances time by `deltaTime` (scaled by the settings' and the states' speeds), runs the state machine and appends the
		// events the playhead passed. With root motion it measures the root joint's motion (GetRootMotion). False while the
		// skeleton or the controller is not loaded (nothing changes then); callers wait for the clips too, which would play
		// as the rest pose (a state without a clip is over at once).
		bool Advance(float deltaTime, const AnimatorAssets& assets, const AnimatorSettings& settings, std::vector<AnimatorFiredEvent>& outEvents);
		// The pose of what plays now: per joint its model-space transform. `scratch` holds per joint the blended local pose and
		// a sample (resized as needed). With root motion the root joint's subtree is held where the clip started.
		bool Evaluate(const AnimatorAssets& assets, const AnimatorSettings& settings, std::vector<JointPose>& scratch, std::vector<glm::mat4>& outModel) const;
		const RootMotionDelta& GetRootMotion() const { return m_RootMotion; }

		// State queries: the state the animator is in (its name; empty for a clip played without one), how far it is (time
		// over duration, counting loops) and whether a cross-fade is under way.
		uint32_t GetCurrentState() const { return m_Count > 0 ? m_Entries[m_Count - 1].State : c_ClipState; }
		double GetNormalizedTime() const { return m_Count > 0 ? m_Entries[m_Count - 1].Normalized : 0.0; }
		bool IsInTransition() const { return m_Count > 1; }
		std::span<const Playing> GetPlaying() const { return std::span<const Playing>(m_Entries.data(), m_Count); }
		// A state Play or CrossFade named while the controller was loading, which plays once it is loaded.
		const std::optional<std::string>& GetPendingState() const { return m_PendingState; }
		// The parameters' values by controller parameter index (floats; bools and triggers are 0 or 1), once a controller was
		// bound.
		std::span<const float> GetParameterValues() const { return m_Parameters; }
		// Loops of a clip an update measures root motion for at most (a very large time step).
		static constexpr uint32_t c_MaxRootMotionLoops = 64;
	private:
		struct PendingParameter
		{
			std::string Name;
			AnimationParameterType Type = AnimationParameterType::Float;
			float Value = 0.0f;
		};

		AnimatorClip GetClip(const AnimatorAssets& assets, uint32_t state) const;
		float GetStateSpeed(const AnimatorAssets& assets, uint32_t state) const;
		bool IsStateLooping(const AnimatorAssets& assets, const AnimatorSettings& settings, uint32_t state) const;
		// Parameters sized to the controller (defaults; values of parameters the previous controller had with the same name and
		// type are kept) and pending values applied: when a controller object is first passed.
		void BindParameters(const AnimationController& controller, uint64_t controllerId);
		void SetPending(std::string_view name, AnimationParameterType type, float value);
		bool SetParameter(const AnimatorAssets& assets, std::string_view name, AnimationParameterType type, float value);
		void StartState(uint32_t state, float fadeSeconds);
		bool ConditionsHold(const AnimationTransition& transition) const;
		void ConsumeTriggers(const AnimationTransition& transition);
		// Advances one entry; appends the events of the state the animator is in. Returns the loop boundaries the playhead
		// crossed (forward positive, backward negative; 0 for states that do not loop).
		int64_t AdvanceEntry(Playing& entry, float seconds, const AnimatorAssets& assets, const AnimatorSettings& settings, bool fireEvents,
			std::vector<AnimatorFiredEvent>& outEvents) const;
		void UpdateWeights();
	private:
		std::array<Playing, c_MaxPlaying> m_Entries = {};
		uint32_t m_Count = 0;
		float m_FadeElapsed = 0.0f;
		float m_FadeDuration = 0.0f;
		bool m_Playing = false;
		bool m_StartPending = false;
		AssetHandle m_PlayedClip = UUID::Null();
		std::optional<std::string> m_PendingState; // Play or CrossFade given while the controller loads
		float m_PendingFade = 0.0f;
		uint64_t m_BoundControllerId = 0; // The controller the parameters belong to (AnimatorAssets::ControllerId; 0: none)
		std::vector<std::string> m_ParameterNames;
		std::vector<AnimationParameterType> m_ParameterTypes;
		std::vector<float> m_Parameters;
		std::vector<PendingParameter> m_PendingParameters;
		RootMotionDelta m_RootMotion;
	};

}
