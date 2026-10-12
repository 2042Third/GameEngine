#include "stpch.h"
#include "Strata/Animation/AnimationController.h"

#include "Strata/Core/JsonUtils.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace Strata
{

	namespace
	{

		constexpr std::string_view c_DocumentKey = "AnimationController";

		bool IsValidName(const std::string& name)
		{
			if (name.empty() || name.size() > AnimationController::c_MaxNameLength)
				return false;
			return std::none_of(name.begin(), name.end(), [](char character) { return static_cast<unsigned char>(character) < 0x20; });
		}

		// Reads a JSON string member; false (with the error) when it is missing or not a string.
		bool ReadString(const nlohmann::json& object, std::string_view key, std::string& out, std::string& outError, std::string_view what)
		{
			const nlohmann::json* value = JsonUtils::Find(object, key);
			if (!value || !value->is_string())
			{
				outError = fmt::format("{} needs a \"{}\" string", what, key);
				return false;
			}
			out = value->get<std::string>();
			return true;
		}

		// An optional finite number; false (with the error) when present but not one.
		bool ReadNumber(const nlohmann::json& object, std::string_view key, float& inOut, std::string& outError, std::string_view what)
		{
			const nlohmann::json* value = JsonUtils::Find(object, key);
			if (!value)
				return true;
			if (!value->is_number())
			{
				outError = fmt::format("{}: \"{}\" must be a number", what, key);
				return false;
			}
			const double number = value->get<double>();
			if (!std::isfinite(number) || std::abs(number) > 1.0e30)
			{
				outError = fmt::format("{}: \"{}\" must be a finite number", what, key);
				return false;
			}
			inOut = static_cast<float>(number);
			return true;
		}

		bool ReadBool(const nlohmann::json& object, std::string_view key, bool& inOut, std::string& outError, std::string_view what)
		{
			const nlohmann::json* value = JsonUtils::Find(object, key);
			if (!value)
				return true;
			if (!value->is_boolean())
			{
				outError = fmt::format("{}: \"{}\" must be true or false", what, key);
				return false;
			}
			inOut = value->get<bool>();
			return true;
		}

		const nlohmann::json* FindArray(const nlohmann::json& object, std::string_view key, std::string& outError)
		{
			const nlohmann::json* value = JsonUtils::Find(object, key);
			if (value && !value->is_array())
			{
				outError = fmt::format("\"{}\" must be an array", key);
				return nullptr;
			}
			return value;
		}

	}

	const char* AnimationParameterTypeToString(AnimationParameterType type)
	{
		switch (type)
		{
			case AnimationParameterType::Float:   return "Float";
			case AnimationParameterType::Bool:    return "Bool";
			case AnimationParameterType::Trigger: return "Trigger";
		}
		return "Float";
	}

	std::optional<AnimationParameterType> AnimationParameterTypeFromString(std::string_view text)
	{
		for (AnimationParameterType type : { AnimationParameterType::Float, AnimationParameterType::Bool, AnimationParameterType::Trigger })
		{
			if (text == AnimationParameterTypeToString(type))
				return type;
		}
		return std::nullopt;
	}

	const char* AnimationConditionModeToString(AnimationConditionMode mode)
	{
		switch (mode)
		{
			case AnimationConditionMode::Greater: return "Greater";
			case AnimationConditionMode::Less:    return "Less";
			case AnimationConditionMode::True:    return "True";
			case AnimationConditionMode::False:   return "False";
			case AnimationConditionMode::Set:     return "Set";
		}
		return "Greater";
	}

	std::optional<AnimationConditionMode> AnimationConditionModeFromString(std::string_view text)
	{
		for (AnimationConditionMode mode : { AnimationConditionMode::Greater, AnimationConditionMode::Less, AnimationConditionMode::True, AnimationConditionMode::False,
			AnimationConditionMode::Set })
		{
			if (text == AnimationConditionModeToString(mode))
				return mode;
		}
		return std::nullopt;
	}

	AnimationParameterType GetConditionParameterType(AnimationConditionMode mode)
	{
		switch (mode)
		{
			case AnimationConditionMode::Greater:
			case AnimationConditionMode::Less:    return AnimationParameterType::Float;
			case AnimationConditionMode::True:
			case AnimationConditionMode::False:   return AnimationParameterType::Bool;
			case AnimationConditionMode::Set:     return AnimationParameterType::Trigger;
		}
		return AnimationParameterType::Float;
	}

	Ref<AnimationController> AnimationController::Create(AnimationControllerData data, std::string* outError)
	{
		auto fail = [outError](const std::string& message) -> Ref<AnimationController>
		{
			if (outError)
				*outError = message;
			return nullptr;
		};

		if (data.Parameters.size() > c_MaxParameters)
			return fail(fmt::format("A controller has at most {} parameters", c_MaxParameters));
		if (data.States.empty())
			return fail("A controller needs at least one state");
		if (data.States.size() > c_MaxStates)
			return fail(fmt::format("A controller has at most {} states", c_MaxStates));
		if (data.Transitions.size() > c_MaxTransitions)
			return fail(fmt::format("A controller has at most {} transitions", c_MaxTransitions));

		std::unordered_set<std::string> names;
		for (const AnimationParameter& parameter : data.Parameters)
		{
			if (!IsValidName(parameter.Name))
				return fail("Every parameter needs a name of 1 to 256 printable characters");
			if (!names.insert(parameter.Name).second)
				return fail(fmt::format("The parameter name '{}' is used twice", parameter.Name));
			if (!std::isfinite(parameter.DefaultFloat))
				return fail(fmt::format("Parameter '{}' has a non-finite default", parameter.Name));
		}
		names.clear();
		for (AnimationState& state : data.States)
		{
			if (!IsValidName(state.Name))
				return fail("Every state needs a name of 1 to 256 printable characters");
			if (!names.insert(state.Name).second)
				return fail(fmt::format("The state name '{}' is used twice", state.Name));
			if (!std::isfinite(state.Speed) || std::abs(state.Speed) > c_MaxSpeed)
				return fail(fmt::format("State '{}' has a speed outside -{} to {}", state.Name, c_MaxSpeed, c_MaxSpeed));
			if (state.Events.size() > c_MaxEventsPerState)
				return fail(fmt::format("State '{}' has more than {} events", state.Name, c_MaxEventsPerState));
			for (const AnimationEvent& event : state.Events)
			{
				if (!IsValidName(event.Name))
					return fail(fmt::format("An event of state '{}' has no valid name", state.Name));
				if (!std::isfinite(event.Time) || event.Time < 0.0f)
					return fail(fmt::format("Event '{}' of state '{}' needs a time of 0 seconds or more", event.Name, state.Name));
			}
			// Dispatched in time order; events at the same time keep their order.
			std::stable_sort(state.Events.begin(), state.Events.end(), [](const AnimationEvent& a, const AnimationEvent& b) { return a.Time < b.Time; });
		}
		if (data.DefaultState >= data.States.size())
			return fail("The default state does not exist");

		for (const AnimationTransition& transition : data.Transitions)
		{
			if (transition.From != AnimationTransition::c_AnyState && transition.From >= data.States.size())
				return fail("A transition leaves a state that does not exist");
			if (transition.To >= data.States.size())
				return fail("A transition leads to a state that does not exist");
			const std::string& from = transition.From == AnimationTransition::c_AnyState ? std::string("Any State") : data.States[transition.From].Name;
			const std::string what = fmt::format("The transition from '{}' to '{}'", from, data.States[transition.To].Name);
			if (transition.Conditions.size() > c_MaxConditions)
				return fail(fmt::format("{} has more than {} conditions", what, c_MaxConditions));
			if (!std::isfinite(transition.Duration) || transition.Duration < 0.0f || transition.Duration > c_MaxDuration)
				return fail(fmt::format("{} needs a cross-fade of 0 to {} seconds", what, c_MaxDuration));
			if (transition.ExitTime && (!std::isfinite(*transition.ExitTime) || *transition.ExitTime < 0.0f))
				return fail(fmt::format("{} needs an exit time of 0 or more", what));
			for (const AnimationCondition& condition : transition.Conditions)
			{
				if (condition.Parameter >= data.Parameters.size())
					return fail(fmt::format("{} tests a parameter that does not exist", what));
				const AnimationParameter& parameter = data.Parameters[condition.Parameter];
				if (GetConditionParameterType(condition.Mode) != parameter.Type)
					return fail(fmt::format("{} tests the {} parameter '{}' with mode {}, which tests {} parameters", what, AnimationParameterTypeToString(parameter.Type),
						parameter.Name, AnimationConditionModeToString(condition.Mode), AnimationParameterTypeToString(GetConditionParameterType(condition.Mode))));
				if (!std::isfinite(condition.Threshold))
					return fail(fmt::format("{} has a non-finite threshold", what));
			}
		}

		Ref<AnimationController> controller(new AnimationController());
		controller->m_Data = std::move(data);
		const AnimationControllerData& stored = controller->m_Data;
		const uint32_t stateCount = static_cast<uint32_t>(stored.States.size());
		controller->m_StateTransitionStart.assign(stateCount + 1, 0);
		for (const AnimationTransition& transition : stored.Transitions)
		{
			if (transition.From != AnimationTransition::c_AnyState)
				controller->m_StateTransitionStart[transition.From + 1]++;
		}
		for (uint32_t state = 0; state < stateCount; state++)
			controller->m_StateTransitionStart[state + 1] += controller->m_StateTransitionStart[state];
		controller->m_TransitionOrder.resize(controller->m_StateTransitionStart[stateCount]);
		std::vector<uint32_t> fill(controller->m_StateTransitionStart.begin(), controller->m_StateTransitionStart.end() - 1);
		for (uint32_t index = 0; index < stored.Transitions.size(); index++)
		{
			const uint32_t from = stored.Transitions[index].From;
			if (from == AnimationTransition::c_AnyState)
				controller->m_AnyStateTransitions.push_back(index);
			else
				controller->m_TransitionOrder[fill[from]++] = index;
		}
		return controller;
	}

	std::optional<AnimationControllerData> AnimationController::DataFromJson(const nlohmann::json& content, std::string* outError)
	{
		std::string error;
		auto fail = [&](const std::string& message) -> std::optional<AnimationControllerData>
		{
			if (outError)
				*outError = message;
			return std::nullopt;
		};
		if (!content.is_object())
			return fail("The controller must be a JSON object");

		AnimationControllerData data;
		if (const nlohmann::json* parameters = FindArray(content, "Parameters", error); !error.empty())
			return fail(error);
		else if (parameters)
		{
			if (parameters->size() > c_MaxParameters)
				return fail(fmt::format("A controller has at most {} parameters", c_MaxParameters));
			for (const nlohmann::json& item : *parameters)
			{
				if (!item.is_object())
					return fail("Every parameter must be an object");
				AnimationParameter parameter;
				std::string type;
				if (!ReadString(item, "Name", parameter.Name, error, "A parameter") || !ReadString(item, "Type", type, error, fmt::format("Parameter '{}'", parameter.Name)))
					return fail(error);
				const std::optional<AnimationParameterType> parsed = AnimationParameterTypeFromString(type);
				if (!parsed)
					return fail(fmt::format("Parameter '{}' has the unknown type '{}' (Float, Bool or Trigger)", parameter.Name, type));
				parameter.Type = *parsed;
				if (const nlohmann::json* value = JsonUtils::Find(item, "Default"); value && !value->is_null())
				{
					const std::string what = fmt::format("Parameter '{}'", parameter.Name);
					if (parameter.Type == AnimationParameterType::Float && !ReadNumber(item, "Default", parameter.DefaultFloat, error, what))
						return fail(error);
					if (parameter.Type == AnimationParameterType::Bool && !ReadBool(item, "Default", parameter.DefaultBool, error, what))
						return fail(error);
					if (parameter.Type == AnimationParameterType::Trigger)
						return fail(fmt::format("Trigger '{}' has no default: triggers start cleared", parameter.Name));
				}
				data.Parameters.push_back(std::move(parameter));
			}
		}

		const nlohmann::json* states = FindArray(content, "States", error);
		if (!error.empty())
			return fail(error);
		if (!states || states->empty())
			return fail("A controller needs \"States\": at least one state");
		if (states->size() > c_MaxStates)
			return fail(fmt::format("A controller has at most {} states", c_MaxStates));
		for (const nlohmann::json& item : *states)
		{
			if (!item.is_object())
				return fail("Every state must be an object");
			AnimationState state;
			if (!ReadString(item, "Name", state.Name, error, "A state"))
				return fail(error);
			const std::string what = fmt::format("State '{}'", state.Name);
			if (const nlohmann::json* clip = JsonUtils::Find(item, "Clip"); clip && !clip->is_null())
			{
				const std::optional<UUID> handle = clip->is_string() ? UUID::FromString(clip->get<std::string>()) : std::nullopt;
				if (!handle)
					return fail(fmt::format("{}: \"Clip\" must be an asset handle (16 hex digits) or null", what));
				state.Clip = *handle;
			}
			if (!ReadNumber(item, "Speed", state.Speed, error, what) || !ReadBool(item, "Loop", state.Loop, error, what))
				return fail(error);
			const nlohmann::json* events = FindArray(item, "Events", error);
			if (!error.empty())
				return fail(fmt::format("{}: {}", what, error));
			if (events)
			{
				if (events->size() > c_MaxEventsPerState)
					return fail(fmt::format("{} has more than {} events", what, c_MaxEventsPerState));
				for (const nlohmann::json& eventItem : *events)
				{
					if (!eventItem.is_object())
						return fail(fmt::format("{}: every event must be an object", what));
					AnimationEvent event;
					if (!ReadString(eventItem, "Name", event.Name, error, fmt::format("An event of {}", what))
						|| !ReadNumber(eventItem, "Time", event.Time, error, fmt::format("Event '{}' of {}", event.Name, what)))
						return fail(error);
					state.Events.push_back(std::move(event));
				}
			}
			data.States.push_back(std::move(state));
		}

		auto findState = [&](const std::string& name) -> uint32_t
		{
			for (uint32_t index = 0; index < data.States.size(); index++)
			{
				if (data.States[index].Name == name)
					return index;
			}
			return ~0u;
		};
		auto findParameter = [&](const std::string& name) -> uint32_t
		{
			for (uint32_t index = 0; index < data.Parameters.size(); index++)
			{
				if (data.Parameters[index].Name == name)
					return index;
			}
			return ~0u;
		};

		if (const nlohmann::json* defaultState = JsonUtils::Find(content, "DefaultState"); defaultState && !defaultState->is_null())
		{
			if (!defaultState->is_string())
				return fail("\"DefaultState\" must be a state name");
			data.DefaultState = findState(defaultState->get<std::string>());
			if (data.DefaultState == ~0u)
				return fail(fmt::format("The default state '{}' does not exist", defaultState->get<std::string>()));
		}

		const nlohmann::json* transitions = FindArray(content, "Transitions", error);
		if (!error.empty())
			return fail(error);
		if (transitions)
		{
			if (transitions->size() > c_MaxTransitions)
				return fail(fmt::format("A controller has at most {} transitions", c_MaxTransitions));
			for (const nlohmann::json& item : *transitions)
			{
				if (!item.is_object())
					return fail("Every transition must be an object");
				AnimationTransition transition;
				bool anyState = false;
				if (!ReadBool(item, "AnyState", anyState, error, "A transition"))
					return fail(error);
				std::string from = "Any State";
				if (!anyState)
				{
					if (!ReadString(item, "From", from, error, "A transition (or \"AnyState\": true)"))
						return fail(error);
					transition.From = findState(from);
					if (transition.From == ~0u)
						return fail(fmt::format("A transition leaves the state '{}', which does not exist", from));
				}
				else if (JsonUtils::Find(item, "From"))
				{
					return fail("A transition has both \"From\" and \"AnyState\"");
				}
				std::string to;
				if (!ReadString(item, "To", to, error, fmt::format("The transition from '{}'", from)))
					return fail(error);
				transition.To = findState(to);
				if (transition.To == ~0u)
					return fail(fmt::format("A transition leads to the state '{}', which does not exist", to));
				const std::string what = fmt::format("The transition from '{}' to '{}'", from, to);
				if (!ReadNumber(item, "Duration", transition.Duration, error, what) || !ReadBool(item, "CanTransitionToSelf", transition.CanTransitionToSelf, error, what))
					return fail(error);
				if (const nlohmann::json* exitTime = JsonUtils::Find(item, "ExitTime"); exitTime && !exitTime->is_null())
				{
					float value = 0.0f;
					if (!ReadNumber(item, "ExitTime", value, error, what))
						return fail(error);
					transition.ExitTime = value;
				}
				const nlohmann::json* conditions = FindArray(item, "Conditions", error);
				if (!error.empty())
					return fail(fmt::format("{}: {}", what, error));
				if (conditions)
				{
					if (conditions->size() > c_MaxConditions)
						return fail(fmt::format("{} has more than {} conditions", what, c_MaxConditions));
					for (const nlohmann::json& conditionItem : *conditions)
					{
						if (!conditionItem.is_object())
							return fail(fmt::format("{}: every condition must be an object", what));
						AnimationCondition condition;
						std::string parameter;
						std::string mode;
						if (!ReadString(conditionItem, "Parameter", parameter, error, fmt::format("A condition of {}", what))
							|| !ReadString(conditionItem, "Mode", mode, error, fmt::format("A condition of {}", what))
							|| !ReadNumber(conditionItem, "Threshold", condition.Threshold, error, fmt::format("A condition of {}", what)))
							return fail(error);
						condition.Parameter = findParameter(parameter);
						if (condition.Parameter == ~0u)
							return fail(fmt::format("{} tests the parameter '{}', which does not exist", what, parameter));
						const std::optional<AnimationConditionMode> parsedMode = AnimationConditionModeFromString(mode);
						if (!parsedMode)
							return fail(fmt::format("{} has the unknown condition mode '{}' (Greater, Less, True, False or Set)", what, mode));
						condition.Mode = *parsedMode;
						transition.Conditions.push_back(condition);
					}
				}
				data.Transitions.push_back(std::move(transition));
			}
		}
		return data;
	}

	nlohmann::json AnimationController::DataToJson(const AnimationControllerData& data)
	{
		nlohmann::json parameters = nlohmann::json::array();
		for (const AnimationParameter& parameter : data.Parameters)
		{
			nlohmann::json item = { { "Name", parameter.Name }, { "Type", AnimationParameterTypeToString(parameter.Type) } };
			if (parameter.Type == AnimationParameterType::Float)
				item["Default"] = parameter.DefaultFloat;
			else if (parameter.Type == AnimationParameterType::Bool)
				item["Default"] = parameter.DefaultBool;
			parameters.push_back(std::move(item));
		}

		nlohmann::json states = nlohmann::json::array();
		for (const AnimationState& state : data.States)
		{
			nlohmann::json events = nlohmann::json::array();
			for (const AnimationEvent& event : state.Events)
				events.push_back({ { "Name", event.Name }, { "Time", event.Time } });
			states.push_back({
				{ "Name", state.Name },
				{ "Clip", state.Clip.IsValid() ? nlohmann::json(state.Clip.ToString()) : nlohmann::json(nullptr) },
				{ "Speed", state.Speed },
				{ "Loop", state.Loop },
				{ "Events", std::move(events) } });
		}

		nlohmann::json transitions = nlohmann::json::array();
		for (const AnimationTransition& transition : data.Transitions)
		{
			nlohmann::json conditions = nlohmann::json::array();
			for (const AnimationCondition& condition : transition.Conditions)
			{
				nlohmann::json item = {
					{ "Parameter", condition.Parameter < data.Parameters.size() ? data.Parameters[condition.Parameter].Name : std::string() },
					{ "Mode", AnimationConditionModeToString(condition.Mode) } };
				if (GetConditionParameterType(condition.Mode) == AnimationParameterType::Float)
					item["Threshold"] = condition.Threshold;
				conditions.push_back(std::move(item));
			}
			nlohmann::json item = nlohmann::json::object();
			if (transition.From == AnimationTransition::c_AnyState)
				item["AnyState"] = true;
			else if (transition.From < data.States.size())
				item["From"] = data.States[transition.From].Name;
			item["To"] = transition.To < data.States.size() ? data.States[transition.To].Name : std::string();
			item["Conditions"] = std::move(conditions);
			item["ExitTime"] = transition.ExitTime ? nlohmann::json(*transition.ExitTime) : nlohmann::json(nullptr);
			item["Duration"] = transition.Duration;
			if (transition.From == AnimationTransition::c_AnyState)
				item["CanTransitionToSelf"] = transition.CanTransitionToSelf;
			transitions.push_back(std::move(item));
		}

		return {
			{ "Parameters", std::move(parameters) },
			{ "States", std::move(states) },
			{ "DefaultState", data.DefaultState < data.States.size() ? nlohmann::json(data.States[data.DefaultState].Name) : nlohmann::json(nullptr) },
			{ "Transitions", std::move(transitions) } };
	}

	Ref<AnimationController> AnimationController::FromJson(const nlohmann::json& json, std::string* outError)
	{
		auto fail = [outError](const std::string& message) -> Ref<AnimationController>
		{
			if (outError)
				*outError = message;
			return nullptr;
		};
		const nlohmann::json* header = JsonUtils::Find(json, "Strata");
		if (!header || !header->is_object() || JsonUtils::GetString(*header, "Format") != c_DocumentKey)
			return fail("Not a Strata animation controller document");
		const uint64_t version = JsonUtils::GetUInt(*header, "Version", 0);
		if (version == 0 || version > c_FormatVersion)
			return fail(fmt::format("Unsupported animation controller version {}", version));
		const nlohmann::json* content = JsonUtils::Find(json, c_DocumentKey);
		if (!content || !content->is_object())
			return fail("The document has no \"AnimationController\" object");
		std::optional<AnimationControllerData> data = DataFromJson(*content, outError);
		if (!data)
			return nullptr;
		return Create(std::move(*data), outError);
	}

	Ref<AnimationController> AnimationController::Deserialize(std::span<const uint8_t> data, std::string* outError)
	{
		std::optional<nlohmann::json> json = JsonUtils::Parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()), outError);
		if (!json)
			return nullptr;
		return FromJson(*json, outError);
	}

	nlohmann::json AnimationController::Serialize() const
	{
		nlohmann::json json;
		json["Strata"] = { { "Format", c_DocumentKey }, { "Version", c_FormatVersion } };
		json[std::string(c_DocumentKey)] = DataToJson(m_Data);
		return json;
	}

	AssetMemoryUsage AnimationController::GetMemoryUsage() const
	{
		uint64_t bytes = sizeof(AnimationController) + m_Data.Parameters.size() * sizeof(AnimationParameter) + m_Data.States.size() * sizeof(AnimationState)
			+ m_Data.Transitions.size() * sizeof(AnimationTransition) + (m_TransitionOrder.size() + m_StateTransitionStart.size() + m_AnyStateTransitions.size()) * sizeof(uint32_t);
		for (const AnimationState& state : m_Data.States)
			bytes += state.Name.capacity() + state.Events.size() * sizeof(AnimationEvent);
		for (const AnimationTransition& transition : m_Data.Transitions)
			bytes += transition.Conditions.size() * sizeof(AnimationCondition);
		return AssetMemoryUsage { bytes };
	}

	uint32_t AnimationController::FindState(std::string_view name) const
	{
		for (uint32_t index = 0; index < m_Data.States.size(); index++)
		{
			if (m_Data.States[index].Name == name)
				return index;
		}
		return ~0u;
	}

	uint32_t AnimationController::FindParameter(std::string_view name) const
	{
		for (uint32_t index = 0; index < m_Data.Parameters.size(); index++)
		{
			if (m_Data.Parameters[index].Name == name)
				return index;
		}
		return ~0u;
	}

	std::span<const uint32_t> AnimationController::GetStateTransitions(uint32_t state) const
	{
		if (state + 1 >= m_StateTransitionStart.size())
			return {};
		return std::span<const uint32_t>(m_TransitionOrder).subspan(m_StateTransitionStart[state], m_StateTransitionStart[state + 1] - m_StateTransitionStart[state]);
	}

	std::vector<AssetHandle> AnimationController::GetClips() const
	{
		std::vector<AssetHandle> clips;
		for (const AnimationState& state : m_Data.States)
		{
			if (state.Clip.IsValid() && std::find(clips.begin(), clips.end(), state.Clip) == clips.end())
				clips.push_back(state.Clip);
		}
		return clips;
	}

}
