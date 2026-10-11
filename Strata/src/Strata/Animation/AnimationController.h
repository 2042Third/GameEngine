#pragma once

#include "Strata/Asset/Asset.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Strata
{

	enum class AnimationParameterType : uint8_t
	{
		Float = 0,
		Bool,
		Trigger // A bool that a transition using it resets (consumes) when it fires
	};

	enum class AnimationConditionMode : uint8_t
	{
		Greater = 0, // Float parameter above Threshold
		Less,        // Float parameter below Threshold
		True,        // Bool parameter set
		False,       // Bool parameter cleared
		Set          // Trigger set (consumed when the transition fires)
	};

	const char* AnimationParameterTypeToString(AnimationParameterType type);
	std::optional<AnimationParameterType> AnimationParameterTypeFromString(std::string_view text);
	const char* AnimationConditionModeToString(AnimationConditionMode mode);
	std::optional<AnimationConditionMode> AnimationConditionModeFromString(std::string_view text);
	// The parameter type a condition mode tests.
	AnimationParameterType GetConditionParameterType(AnimationConditionMode mode);

	struct AnimationParameter
	{
		std::string Name;
		AnimationParameterType Type = AnimationParameterType::Float;
		float DefaultFloat = 0.0f;
		bool DefaultBool = false; // Bool parameters; triggers start cleared
	};

	// A named moment of a state's clip, dispatched to the animator entity's scripts (OnAnimationEvent) when the playhead
	// passes it.
	struct AnimationEvent
	{
		std::string Name;
		float Time = 0.0f; // Seconds from the start of the state's clip
	};

	struct AnimationState
	{
		std::string Name;
		AssetHandle Clip = UUID::Null(); // None: the rest pose
		float Speed = 1.0f;              // Playback rate (negative plays backwards)
		bool Loop = true;
		std::vector<AnimationEvent> Events; // Sorted by time
	};

	struct AnimationCondition
	{
		uint32_t Parameter = 0; // Index into the controller's parameters
		AnimationConditionMode Mode = AnimationConditionMode::Greater;
		float Threshold = 0.0f; // Greater and Less
	};

	struct AnimationTransition
	{
		static constexpr uint32_t c_AnyState = ~0u;

		uint32_t From = c_AnyState; // State index, or c_AnyState: from whatever state the animator is in
		uint32_t To = 0;
		std::vector<AnimationCondition> Conditions; // All must hold
		// The source state's normalized time (its time over its clip's duration) the transition waits for: it fires in the
		// update the time reaches it (looping states with an exit time below 1: at that point of every loop) while the
		// conditions hold. None: it fires as soon as they hold.
		std::optional<float> ExitTime;
		float Duration = 0.25f; // Cross-fade in seconds (0: switch at once)
		bool CanTransitionToSelf = false; // Any-state transitions only: whether it fires while the animator is in its target
	};

	// Everything a controller holds, as plain data (commands and tests build it; Create validates it).
	struct AnimationControllerData
	{
		std::vector<AnimationParameter> Parameters;
		std::vector<AnimationState> States;
		uint32_t DefaultState = 0;
		std::vector<AnimationTransition> Transitions;
	};

	// An animator's state machine: parameters (float, bool, trigger), states (a clip, a speed, looping, events) and
	// transitions between them (from a state or from any state; conditions on parameters; an exit time; a cross-fade). An
	// engine-native document (".stanimator"):
	//   { "Strata": { "Format": "AnimationController", "Version": 1 },
	//     "AnimationController": { "Parameters": [ { "Name", "Type": "Float" | "Bool" | "Trigger", "Default" } ],
	//       "States": [ { "Name", "Clip": "<asset handle>" | null, "Speed", "Loop", "Events": [ { "Name", "Time" } ] } ],
	//       "DefaultState": "<state name>",
	//       "Transitions": [ { "From": "<state name>" (or "AnyState": true), "To", "Conditions": [ { "Parameter",
	//         "Mode": "Greater" | "Less" | "True" | "False" | "Set", "Threshold" } ], "ExitTime": number | null, "Duration",
	//         "CanTransitionToSelf" } ] } }
	// FromJson validates everything and refers to states and parameters by name; Serialize writes the same form.
	class AnimationController : public Asset
	{
	public:
		static AssetType GetStaticType() { return AssetType::AnimationController; }
		AssetType GetType() const override { return GetStaticType(); }

		static constexpr uint32_t c_FormatVersion = 1;
		static constexpr uint32_t c_MaxParameters = 256;
		static constexpr uint32_t c_MaxStates = 1024;
		static constexpr uint32_t c_MaxTransitions = 4096;
		static constexpr uint32_t c_MaxConditions = 16;
		static constexpr uint32_t c_MaxEventsPerState = 256;
		static constexpr uint32_t c_MaxNameLength = 256;
		static constexpr float c_MaxSpeed = 100.0f;
		static constexpr float c_MaxDuration = 60.0f;

		// Validates; null with the reason (the first problem found).
		static Ref<AnimationController> Create(AnimationControllerData data, std::string* outError = nullptr);
		static Ref<AnimationController> FromJson(const nlohmann::json& json, std::string* outError = nullptr);
		static Ref<AnimationController> Deserialize(std::span<const uint8_t> data, std::string* outError = nullptr);
		nlohmann::json Serialize() const;
		// Reads a controller description in the document's content form (the "AnimationController" object), as commands take it.
		static std::optional<AnimationControllerData> DataFromJson(const nlohmann::json& content, std::string* outError = nullptr);
		static nlohmann::json DataToJson(const AnimationControllerData& data);

		AssetMemoryUsage GetMemoryUsage() const override;

		const AnimationControllerData& GetData() const { return m_Data; }
		const std::vector<AnimationParameter>& GetParameters() const { return m_Data.Parameters; }
		const std::vector<AnimationState>& GetStates() const { return m_Data.States; }
		const std::vector<AnimationTransition>& GetTransitions() const { return m_Data.Transitions; }
		uint32_t GetDefaultState() const { return m_Data.DefaultState; }
		// Indices by name (exact match), or ~0u.
		uint32_t FindState(std::string_view name) const;
		uint32_t FindParameter(std::string_view name) const;
		// The transitions that leave a state (in document order) and those from any state.
		std::span<const uint32_t> GetStateTransitions(uint32_t state) const;
		std::span<const uint32_t> GetAnyStateTransitions() const { return m_AnyStateTransitions; }
		// The clips its states play, each once.
		std::vector<AssetHandle> GetClips() const;
	private:
		AnimationController() = default;
	private:
		AnimationControllerData m_Data;
		std::vector<uint32_t> m_TransitionOrder;   // Transition indices grouped by source state
		std::vector<uint32_t> m_StateTransitionStart; // Per state (and one past the end): its range in m_TransitionOrder
		std::vector<uint32_t> m_AnyStateTransitions;
	};

}
