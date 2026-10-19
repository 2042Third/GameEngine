#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/InputSequence.h"
#include "Editor/SimulatedInput.h"

#include <Strata/Core/JsonUtils.h>
#include <Strata/Core/Log.h>
#include <Strata/GameUI/GameUIComponents.h>
#include <Strata/GameUI/GameUIHitTest.h>
#include <Strata/GameUI/GameUIScene.h>
#include <Strata/Input/Input.h>
#include <Strata/Input/InputNames.h>
#include <Strata/Reflection/ComponentRegistry.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scene/Scene.h>
#include <Strata/Scene/SceneServices.h>
#include <Strata/Scripting/ScriptEngine.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// About half an hour of a game's fixed steps at 60 per second.
		constexpr int64_t c_MaxHoldSteps = 100'000;
		// Pointer positions and scroll offsets beyond this are mistakes, not input.
		constexpr double c_MaxInputCoordinate = 1'000'000.0;

		enum class ButtonAction : uint8_t
		{
			Tap = 0,
			Press,
			Release
		};

		// Names match ignoring case, and mouse buttons have numbered aliases: the schemas list the names in their description, as an
		// enum would make clients that validate against the schema refuse spellings the command accepts.
		std::string JoinNames(std::span<const char* const> names)
		{
			std::string joined;
			for (const char* name : names)
				joined += (joined.empty() ? "" : ", ") + std::string(name);
			return joined;
		}

		nlohmann::json ActionSchema(const char* what)
		{
			return { { "type", "string" }, { "enum", { "tap", "press", "release" } },
				{ "description", fmt::format("tap (default): hold the {0} for 'steps' fixed steps of the game (or 'seconds' of game time), then let "
					"go; press: hold it until a release, input.releaseAll or play.stop; release: end every hold of the {0} (taps too)", what) } };
		}

		// A schema with the duration parameters of a tap or a timed hold: fixed steps of the game, its physics and FixedUpdate
		// time base.
		nlohmann::json WithDurations(nlohmann::json schema, const char* what, bool hasDefault)
		{
			nlohmann::json& properties = schema["properties"];
			properties["steps"] = IntegerSchema(fmt::format("Fixed steps of the game {} lasts ({}). Exact when frames run one step each (play.step, "
				"input.sequence); in a running game the release lands after the frame whose update ran the last step, which may run up to "
				"maxFixedStepsPerFrame - 1 more", what, hasDefault ? "default 1" : "default: until replaced or released"), 1, c_MaxHoldSteps);
			properties["seconds"] = NumberSchema(fmt::format("Game time {} lasts, rounded up to whole fixed steps of the scene's fixedTimestep "
				"(instead of steps)", what));
			properties["frames"] = IntegerSchema("Same as steps (an older name: they used to count frames)", 1, c_MaxHoldSteps);
			return schema;
		}

		// Reads steps, seconds or frames (an alias of steps): nullopt when none is given. Seconds become whole fixed steps of the
		// running scene's fixed timestep, rounded up.
		std::optional<int64_t> ReadDuration(const nlohmann::json& parameters, CommandArguments& arguments, const EditorContext& context)
		{
			const int given = (arguments.Has("steps") ? 1 : 0) + (arguments.Has("frames") ? 1 : 0) + (arguments.Has("seconds") ? 1 : 0);
			if (given == 0)
				return std::nullopt;
			if (given > 1)
			{
				arguments.SetError("Pass one of 'steps', 'seconds' or 'frames' (an older name of steps)");
				return std::nullopt;
			}
			if (!arguments.Has("seconds"))
				return arguments.Has("steps") ? arguments.GetInt("steps", 1, 1, c_MaxHoldSteps) : arguments.GetInt("frames", 1, 1, c_MaxHoldSteps);
			const nlohmann::json& seconds = parameters["seconds"];
			const float fixedStep = context.IsPlaying() ? context.GetActiveScene()->GetSettings().FixedTimestep : SceneSettings().FixedTimestep;
			if (!seconds.is_number() || !std::isfinite(seconds.get<double>()) || seconds.get<double>() <= 0.0
				|| seconds.get<double>() / fixedStep > static_cast<double>(c_MaxHoldSteps))
			{
				arguments.SetError(fmt::format("Parameter 'seconds' must be a number above 0 and at most {} fixed steps of {} s", c_MaxHoldSteps, fixedStep));
				return std::nullopt;
			}
			// Rounded up, so that a duration a whole number of steps long (1/60 s) is not taken for one step more by float error.
			return std::max<int64_t>(1, static_cast<int64_t>(std::ceil(seconds.get<double>() / fixedStep - 1.0e-6)));
		}

		nlohmann::json WaitSchema()
		{
			return BoolSchema("Answer once the game has seen the input (default: true while the game runs; false while it is paused without a "
				"pending step, because the input then waits for play.step or play.pause false and the command would wait with it)");
		}

		nlohmann::json Vec2Schema(std::string description)
		{
			return { { "type", "array" }, { "items", { { "type", "number" } } }, { "minItems", 2 }, { "maxItems", 2 }, { "description", std::move(description) } };
		}

		std::optional<ButtonAction> ReadAction(CommandArguments& arguments)
		{
			const std::string action = arguments.GetString("action", "tap");
			if (action == "tap")
				return ButtonAction::Tap;
			if (action == "press")
				return ButtonAction::Press;
			if (action == "release")
				return ButtonAction::Release;
			arguments.SetError(fmt::format("Parameter 'action' must be tap, press or release, not '{}'", action));
			return std::nullopt;
		}

		// Reads action and its duration in fixed steps (steps, seconds or frames); a duration only means something for taps.
		std::optional<ButtonAction> ReadButtonAction(const nlohmann::json& parameters, CommandArguments& arguments, const EditorContext& context,
			int64_t& outSteps)
		{
			const std::optional<ButtonAction> action = ReadAction(arguments);
			const std::optional<int64_t> steps = ReadDuration(parameters, arguments, context);
			outSteps = steps.value_or(1);
			if (action && *action != ButtonAction::Tap && steps)
				arguments.SetError("Parameters 'steps', 'seconds' and 'frames' only apply to action tap");
			return action;
		}

		// While the game is paused without a pending step its input frames stand still: a command waiting for the game to see
		// its input would wait until a client steps or resumes the game, which a client waiting for the answer cannot do.
		bool ReadWait(CommandArguments& arguments)
		{
			return arguments.GetBool("wait", !Input::IsSuspended());
		}

		std::optional<glm::vec2> ReadVec2(const nlohmann::json& parameters, CommandArguments& arguments, const char* name)
		{
			const auto it = parameters.find(name);
			if (it != parameters.end() && it->is_array() && it->size() == 2 && (*it)[0].is_number() && (*it)[1].is_number())
			{
				const double x = (*it)[0].get<double>();
				const double y = (*it)[1].get<double>();
				if (std::isfinite(x) && std::isfinite(y) && std::abs(x) <= c_MaxInputCoordinate && std::abs(y) <= c_MaxInputCoordinate)
					return glm::vec2(static_cast<float>(x), static_cast<float>(y));
			}
			arguments.SetError(fmt::format("Parameter '{}' must be an array of two finite numbers within +-{}", name, c_MaxInputCoordinate));
			return std::nullopt;
		}

		const char* ActionToString(ButtonAction action)
		{
			switch (action)
			{
				case ButtonAction::Tap:     return "tap";
				case ButtonAction::Press:   return "press";
				case ButtonAction::Release: return "release";
			}
			return "tap";
		}

		// Simulated input is meant for the game's scripts, which only run in play mode.
		bool IsGameRunning(const EditorContext& context)
		{
			return context.GetSceneState() == SceneState::Play;
		}

		EditorCommandResult NoGameError()
		{
			return EditorCommandResult::Fail("Simulated input goes to a running game: start it with play.start first");
		}

		nlohmann::json DescribeButtons(const SimulatedInput& input, SimulatedControlType type)
		{
			nlohmann::json names = nlohmann::json::array();
			for (uint16_t code : input.GetHeld(type))
			{
				const char* name = type == SimulatedControlType::Key ? InputNames::GetKeyName(code) : InputNames::GetMouseButtonName(code);
				// Commands only hold named buttons; a code without a name is reported by number.
				names.push_back(name ? nlohmann::json(name) : nlohmann::json(code));
			}
			return names;
		}

		// [{"gamepad": 0, "button": "A"}, ...]
		nlohmann::json DescribeGamepadButtons(const SimulatedInput& input)
		{
			nlohmann::json buttons = nlohmann::json::array();
			for (uint16_t code : input.GetHeld(SimulatedControlType::GamepadButton))
			{
				const char* name = InputNames::GetGamepadButtonName(static_cast<GamepadButton>(SimulatedInput::GetGamepadControl(code)));
				buttons.push_back({ { "gamepad", SimulatedInput::GetGamepad(code) },
					{ "button", name ? nlohmann::json(name) : nlohmann::json(SimulatedInput::GetGamepadControl(code)) } });
			}
			return buttons;
		}

		// [{"gamepad": 0, "axis": "LeftX", "value": 0.5}, ...]
		nlohmann::json DescribeGamepadAxes(const SimulatedInput& input)
		{
			nlohmann::json axes = nlohmann::json::array();
			for (uint16_t code : input.GetHeld(SimulatedControlType::GamepadAxis))
			{
				const char* name = InputNames::GetGamepadAxisName(static_cast<GamepadAxis>(SimulatedInput::GetGamepadControl(code)));
				axes.push_back({ { "gamepad", SimulatedInput::GetGamepad(code) },
					{ "axis", name ? nlohmann::json(name) : nlohmann::json(SimulatedInput::GetGamepadControl(code)) },
					{ "value", input.GetAxisValue(code) } });
			}
			return axes;
		}

		// What the commands hold, and where the simulated pointer is.
		nlohmann::json DescribeSimulatedInput(const EditorContext& context)
		{
			const SimulatedInput& input = context.GetSimulatedInput();
			const std::optional<glm::vec2> position = Input::GetSimulatedMousePosition();
			return {
				{ "keys", DescribeButtons(input, SimulatedControlType::Key) },
				{ "mouseButtons", DescribeButtons(input, SimulatedControlType::MouseButton) },
				{ "gamepadButtons", DescribeGamepadButtons(input) },
				{ "gamepadAxes", DescribeGamepadAxes(input) },
				{ "mousePosition", position ? nlohmann::json { position->x, position->y } : nlohmann::json(nullptr) } };
		}

		// Reads the gamepad's index (0 to c_MaxGamepads - 1, default 0).
		uint32_t ReadGamepad(CommandArguments& arguments)
		{
			return static_cast<uint32_t>(arguments.GetInt("gamepad", 0, 0, c_MaxGamepads - 1));
		}

		nlohmann::json GamepadSchema()
		{
			return IntegerSchema(fmt::format("The gamepad, 0 to {} (default 0): scripts read it as gamepad index; a gamepad simulated input used counts "
				"as connected until play stops", c_MaxGamepads - 1), 0, c_MaxGamepads - 1);
		}

		EditorCommandResult AnswerNow(const EditorContext& context, nlohmann::json result)
		{
			result["seen"] = false;
			result["held"] = DescribeSimulatedInput(context);
			return EditorCommandResult::Ok(std::move(result));
		}

		// Answers once the game has had input frame `frame` (Input::GetFrameIndex), or at once without `wait`.
		EditorCommandResult AnswerWhenSeen(EditorContext& context, bool wait, uint64_t frame, nlohmann::json result, std::string what)
		{
			if (!wait)
				return AnswerNow(context, std::move(result));
			return EditorCommandResult::Defer([frame, result = std::move(result), what = std::move(what)](EditorContext& pollContext) -> std::optional<EditorCommandResult>
			{
				if (!IsGameRunning(pollContext))
					return EditorCommandResult::Fail(fmt::format("Play mode stopped before the game saw the {}", what));
				if (Input::GetFrameIndex() < frame)
					return std::nullopt;
				nlohmann::json answer = result;
				answer["seen"] = true;
				answer["held"] = DescribeSimulatedInput(pollContext);
				return EditorCommandResult::Ok(std::move(answer));
			});
		}

		// Shared by input.key, input.mouseButton and input.gamepadButton.
		EditorCommandResult RunButtonAction(EditorContext& context, SimulatedControlType type, uint16_t code, ButtonAction action, int64_t steps,
			bool wait, nlohmann::json result, std::string what)
		{
			SimulatedInput& input = context.GetSimulatedInput();
			if (action == ButtonAction::Release)
			{
				result["released"] = input.Release(type, code);
				return AnswerWhenSeen(context, wait, Input::GetFrameIndex() + 1, std::move(result), std::move(what));
			}
			if (action == ButtonAction::Press)
			{
				Ref<const SimulatedHold> hold = input.Press(type, code, 0, context.GetFixedStepCount());
				return AnswerWhenSeen(context, wait, hold->FirstFrame, std::move(result), std::move(what));
			}

			// A tap answers once the game has seen its end: the input frame after the frame that ran its last step (unless
			// another hold keeps the button down).
			Ref<const SimulatedHold> hold = input.Press(type, code, static_cast<uint32_t>(steps), context.GetFixedStepCount());
			if (!wait)
				return AnswerNow(context, std::move(result));
			return EditorCommandResult::Defer([hold, result = std::move(result), what = std::move(what)](EditorContext& pollContext) -> std::optional<EditorCommandResult>
			{
				if (!IsGameRunning(pollContext))
					return EditorCommandResult::Fail(fmt::format("Play mode stopped before the {} was released", what));
				if (!hold->Ended || Input::GetFrameIndex() < hold->EndFrame)
					return std::nullopt;
				nlohmann::json answer = result;
				answer["seen"] = true;
				// Ended early by a release of the button, input.releaseAll or a restart of play mode.
				answer["interrupted"] = hold->Interrupted;
				answer["held"] = DescribeSimulatedInput(pollContext);
				return EditorCommandResult::Ok(std::move(answer));
			});
		}

	}

	namespace
	{

		// The longest input sequence: about half an hour of a 60 Hz game.
		constexpr int64_t c_MaxSequenceSteps = 100'000;

		std::optional<int64_t> ReadIndex(const nlohmann::json& object, const char* name, int64_t minimum, int64_t maximum)
		{
			const auto value = object.find(name);
			if (value == object.end() || !value->is_number_integer())
				return std::nullopt;
			const int64_t number = value->get<int64_t>();
			return number >= minimum && number <= maximum ? std::optional<int64_t>(number) : std::nullopt;
		}

		std::optional<glm::vec2> ReadPair(const nlohmann::json& value)
		{
			if (!value.is_array() || value.size() != 2 || !value[0].is_number() || !value[1].is_number())
				return std::nullopt;
			const double x = value[0].get<double>();
			const double y = value[1].get<double>();
			if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > c_MaxInputCoordinate || std::abs(y) > c_MaxInputCoordinate)
				return std::nullopt;
			return glm::vec2(static_cast<float>(x), static_cast<float>(y));
		}

		// One event of input.sequence's events; the error says what is wrong with it.
		std::optional<InputSequence::Event> ReadSequenceEvent(const nlohmann::json& json, size_t index, int64_t steps, const EditorCommandRegistry& registry,
			std::string& outError)
		{
			auto fail = [&outError, index](std::string message) -> std::optional<InputSequence::Event>
			{
				outError = fmt::format("events[{}]: {}", index, message);
				return std::nullopt;
			};
			if (!json.is_object())
				return fail("must be an object");
			InputSequence::Event event;
			event.Index = index;
			const std::optional<int64_t> step = ReadIndex(json, "step", 0, steps - 1);
			if (!step)
				return fail(fmt::format("'step' must be an integer from 0 to {} (one below steps)", steps - 1));
			event.Step = static_cast<uint32_t>(*step);
			const char* kinds[] = { "key", "mouseButton", "gamepadButton", "gamepadAxis", "mouseMove", "scroll", "command" };
			const char* kind = nullptr;
			for (const char* candidate : kinds)
			{
				if (json.contains(candidate))
				{
					if (kind)
						return fail(fmt::format("give one of key, mouseButton, gamepadButton, gamepadAxis, mouseMove, scroll or command, not both {} and {}",
							kind, candidate));
					kind = candidate;
				}
			}
			if (!kind)
				return fail("give one of key, mouseButton, gamepadButton, gamepadAxis, mouseMove, scroll or command");
			const std::string what = kind;
			std::vector<std::string> allowed = { "step", what };
			if (what == "key" || what == "mouseButton" || what == "gamepadButton")
			{
				allowed.insert(allowed.end(), { "action", "steps", "gamepad" });
				const nlohmann::json& name = json[what];
				if (!name.is_string())
					return fail(fmt::format("'{}' must be a name", what));
				event.Type = InputSequence::EventType::Button;
				if (what == "key")
				{
					const std::optional<KeyCode> key = InputNames::FindKey(name.get<std::string>());
					if (!key)
						return fail(fmt::format("unknown key '{}'", name.get<std::string>()));
					event.Control = SimulatedControlType::Key;
					event.Code = *key;
				}
				else if (what == "mouseButton")
				{
					const std::optional<MouseCode> button = InputNames::FindMouseButton(name.get<std::string>());
					if (!button)
						return fail(fmt::format("unknown mouse button '{}'", name.get<std::string>()));
					event.Control = SimulatedControlType::MouseButton;
					event.Code = *button;
				}
				else
				{
					const std::optional<GamepadButton> button = InputNames::FindGamepadButton(name.get<std::string>());
					if (!button)
						return fail(fmt::format("unknown gamepad button '{}'", name.get<std::string>()));
					const int64_t gamepad = json.contains("gamepad") ? ReadIndex(json, "gamepad", 0, c_MaxGamepads - 1).value_or(-1) : 0;
					if (gamepad < 0)
						return fail(fmt::format("'gamepad' must be 0 to {}", c_MaxGamepads - 1));
					event.Control = SimulatedControlType::GamepadButton;
					event.Code = SimulatedInput::MakeGamepadCode(static_cast<uint32_t>(gamepad), static_cast<uint32_t>(*button));
				}
				const std::string action = json.contains("action") && json["action"].is_string() ? json["action"].get<std::string>() : std::string("tap");
				if (json.contains("action") && !json["action"].is_string())
					return fail("'action' must be press, release or tap");
				if (action == "tap")
					event.Action = InputSequence::ButtonAction::Tap;
				else if (action == "press")
					event.Action = InputSequence::ButtonAction::Press;
				else if (action == "release")
					event.Action = InputSequence::ButtonAction::Release;
				else
					return fail(fmt::format("'action' must be press, release or tap, not '{}'", action));
				if (json.contains("steps"))
				{
					const std::optional<int64_t> tapSteps = ReadIndex(json, "steps", 1, c_MaxHoldSteps);
					if (!tapSteps || event.Action != InputSequence::ButtonAction::Tap)
						return fail(fmt::format("'steps' is a tap's length, 1 to {}", c_MaxHoldSteps));
					event.TapSteps = static_cast<uint32_t>(*tapSteps);
				}
			}
			else if (what == "gamepadAxis")
			{
				allowed.insert(allowed.end(), { "value", "gamepad" });
				const nlohmann::json& name = json[what];
				const std::optional<GamepadAxis> axis = name.is_string() ? InputNames::FindGamepadAxis(name.get<std::string>()) : std::nullopt;
				if (!axis)
					return fail("'gamepadAxis' must name an axis (LeftX, LeftY, RightX, RightY, LeftTrigger, RightTrigger)");
				const int64_t gamepad = json.contains("gamepad") ? ReadIndex(json, "gamepad", 0, c_MaxGamepads - 1).value_or(-1) : 0;
				if (gamepad < 0)
					return fail(fmt::format("'gamepad' must be 0 to {}", c_MaxGamepads - 1));
				const auto value = json.find("value");
				if (value == json.end() || !(value->is_null() || (value->is_number() && std::isfinite(value->get<double>()))))
					return fail("'value' must be a finite number, or null to give the axis back to the device");
				event.Type = InputSequence::EventType::Axis;
				event.Axis = *axis;
				event.Gamepad = static_cast<uint32_t>(gamepad);
				if (!value->is_null())
					event.AxisValue = static_cast<float>(value->get<double>());
			}
			else if (what == "mouseMove" || what == "scroll")
			{
				const std::optional<glm::vec2> vector = ReadPair(json[what]);
				if (!vector)
					return fail(fmt::format("'{}' must be [x, y], two finite numbers within +-{}", what, c_MaxInputCoordinate));
				event.Type = what == "mouseMove" ? InputSequence::EventType::MouseMove : InputSequence::EventType::Scroll;
				event.Vector = *vector;
			}
			else
			{
				allowed.push_back("parameters");
				const nlohmann::json& command = json["command"];
				const EditorCommand* target = command.is_string() && command.get<std::string>().starts_with("input.")
					&& command.get<std::string>() != "input.sequence" ? registry.Find(command.get<std::string>()) : nullptr;
				if (!target)
					return fail("'command' must name an input.* command (not input.sequence)");
				const auto parameters = json.find("parameters");
				if (parameters != json.end() && !parameters->is_object())
					return fail("'parameters' must be an object");
				event.Type = InputSequence::EventType::Command;
				event.Command = command.get<std::string>();
				event.Parameters = parameters != json.end() ? *parameters : nlohmann::json::object();
				// It answers at once (commands that wait for the game take wait: false), and its parameters are checked now, as
				// running it would check them: a misspelled one fails the sequence before anything runs.
				const nlohmann::json* declared = JsonUtils::Find(target->Parameters, "properties");
				if (declared && declared->contains("wait"))
					event.Parameters["wait"] = false;
				if (std::optional<std::string> problem = EditorCommandRegistry::CheckParameters(*target, event.Parameters))
					return fail(std::move(*problem));
			}
			for (const auto& [key, unused] : json.items())
			{
				if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
					return fail(fmt::format("unknown key '{}' for a {} event", key, what));
			}
			return event;
		}

		// One entry of input.sequence's observe; the error says what is wrong with it.
		std::optional<InputSequence::Observation> ReadObservation(const nlohmann::json& json, size_t index, EditorContext& context, std::string& outError)
		{
			auto fail = [&outError, index](std::string message) -> std::optional<InputSequence::Observation>
			{
				outError = fmt::format("observe[{}]: {}", index, message);
				return std::nullopt;
			};
			if (!json.is_object())
				return fail("must be an object");
			InputSequence::Observation observation;
			const auto entity = json.find("entity");
			const std::optional<UUID> id = entity != json.end() ? UUIDFromJson(*entity) : std::nullopt;
			if (!id || !context.GetActiveScene()->GetEntityByUUID(*id))
				return fail("'entity' must name an entity of the running scene");
			observation.Entity = *id;
			if (json.contains("every"))
			{
				const std::optional<int64_t> every = ReadIndex(json, "every", 1, c_MaxSequenceSteps);
				if (!every)
					return fail(fmt::format("'every' must be 1 to {} steps", c_MaxSequenceSteps));
				observation.Every = static_cast<uint32_t>(*every);
			}
			std::vector<std::string> allowed = { "entity", "every" };
			if (json.contains("component"))
			{
				allowed.insert(allowed.end(), { "component", "property" });
				const ComponentInfo* info = json["component"].is_string() ? ComponentRegistry::Find(json["component"].get<std::string>()) : nullptr;
				if (!info || HasFlag(info->Flags, ComponentFlags::NoSerialize))
					return fail("'component' must name a component (see component.list)");
				observation.Type = InputSequence::ObservationType::Component;
				observation.Info = info;
				if (json.contains("property"))
				{
					const PropertyInfo* property = json["property"].is_string() ? info->FindProperty(json["property"].get<std::string>()) : nullptr;
					if (!property)
						return fail(fmt::format("'property' must name a property of {}", info->Name));
					observation.Property = property->Name;
				}
			}
			else if (json.contains("script"))
			{
				allowed.insert(allowed.end(), { "script", "field" });
				const Ref<ScriptEngine>& engine = context.GetScriptEngine();
				const std::string className = json["script"].is_string() ? json["script"].get<std::string>() : std::string();
				const ScriptClassInfo* info = engine && engine->IsModuleLoaded() ? engine->FindClass(className) : nullptr;
				if (!info)
					return fail(fmt::format("'script' must name a class of the loaded module, not '{}'", className));
				const std::string field = json.contains("field") && json["field"].is_string() ? json["field"].get<std::string>() : std::string();
				if (!info->FindField(field))
					return fail(fmt::format("'field' must name a field of {}", className));
				observation.Type = InputSequence::ObservationType::ScriptField;
				observation.ClassName = className;
				observation.Field = field;
			}
			else if (json.contains("world"))
			{
				allowed.push_back("world");
				if (json["world"] != true)
					return fail("'world' must be true");
				observation.Type = InputSequence::ObservationType::World;
			}
			else
			{
				return fail("give component (and property), script and field, or world: true");
			}
			for (const auto& [key, unused] : json.items())
			{
				if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
					return fail(fmt::format("unknown key '{}'", key));
			}
			return observation;
		}

		nlohmann::json DescribeSequenceResult(const InputSequence::Result& result)
		{
			return { { "stepsRun", result.StepsRun }, { "frameIndex", result.FrameIndex }, { "time", result.Time }, { "seen", result.Seen },
				{ "samples", result.Samples }, { "interrupted", result.Interrupted } };
		}

	}

	void RegisterInputCommands(EditorCommandRegistry& registry)
	{
		registry.Register(MayDefer({ "input.sequence",
			"Plays a script of input in the running game, timed in its fixed steps (play mode): pauses the game (and resumes it at the end if it "
			"ran), then every frame runs exactly one step with the events of that step, so the result is the same whatever the frame rate. "
			"events: [{step (0-based, below steps), then one of: key | mouseButton | gamepadButton (+ gamepad) with action press | release | tap "
			"(+ steps, a tap's length); gamepadAxis (+ gamepad) with value (null: back to the device); mouseMove [x, y]; scroll [x, y]; command "
			"(an input.* command, run with wait: false; its parameters checked against its schema before anything runs) with parameters}]; "
			"events of one step apply in order, and one that fails ends the sequence with its error (holds released, the game resumed if it "
			"ran). steps: fixed steps to run (at "
			"least the last event's step + 1). observe: [{entity, component, property?}, {entity, script (class), field}, {entity, world: "
			"true}], each with every (sample after every n steps; default only at the end). The sequence's holds are released at its end unless "
			"keepHeld. Answers once it ran: {stepsRun, frameIndex, time, seen: [{step, event (index)}], samples: [{step, values: [{observe "
			"(index), value}]}], interrupted (a tap of it was ended by something else)}; play stopping or resuming fails it with stepsRun. "
			"play.step and another input.sequence fail while it runs. wait: false answers at once with {sequence, steps}.",
			ObjectSchema({
				{ "events", { { "type", "array" }, { "items", { { "type", "object" } } }, { "description", "Input events, each with its step" } } },
				{ "steps", IntegerSchema("Fixed steps to run", 1, c_MaxSequenceSteps) },
				{ "observe", { { "type", "array" }, { "items", { { "type", "object" } } },
					{ "description", "What to sample: {entity, component, property?, every?}, {entity, script, field, every?} or {entity, world: true, every?}" } } },
				{ "keepHeld", BoolSchema("Keep the keys and buttons the sequence still holds at its end (default false: released)") },
				{ "wait", BoolSchema("Answer once the sequence ran (default true)") } }, { "events", "steps" }),
			[&registry](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const int64_t steps = arguments.GetInt("steps", 1, 1, c_MaxSequenceSteps);
				if (!arguments.Has("steps"))
					arguments.SetError("Missing parameter 'steps'");
				const bool keepHeld = arguments.GetBool("keepHeld", false);
				const bool wait = arguments.GetBool("wait", true);
				const auto eventsJson = parameters.find("events");
				if (arguments.IsValid() && (eventsJson == parameters.end() || !eventsJson->is_array()))
					arguments.SetError("Parameter 'events' must be an array");
				const auto observeJson = parameters.find("observe");
				if (arguments.IsValid() && observeJson != parameters.end() && !observeJson->is_array())
					arguments.SetError("Parameter 'observe' must be an array");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!IsGameRunning(context))
					return NoGameError();

				std::string error;
				std::vector<InputSequence::Event> events;
				for (size_t index = 0; index < eventsJson->size(); index++)
				{
					std::optional<InputSequence::Event> event = ReadSequenceEvent((*eventsJson)[index], index, steps, registry, error);
					if (!event)
						return EditorCommandResult::InvalidParameters(error);
					events.push_back(std::move(*event));
				}
				std::stable_sort(events.begin(), events.end(), [](const InputSequence::Event& a, const InputSequence::Event& b) { return a.Step < b.Step; });
				std::vector<InputSequence::Observation> observations;
				if (observeJson != parameters.end())
				{
					for (size_t index = 0; index < observeJson->size(); index++)
					{
						std::optional<InputSequence::Observation> observation = ReadObservation((*observeJson)[index], index, context, error);
						if (!observation)
							return EditorCommandResult::InvalidParameters(error);
						observations.push_back(std::move(*observation));
					}
				}

				const uint64_t id = context.StartInputSequence(CreateScope<InputSequence>(std::move(events), static_cast<uint32_t>(steps), std::move(observations),
					keepHeld, registry), &error);
				if (id == 0)
					return EditorCommandResult::Fail(error);
				if (!wait)
					return EditorCommandResult::Ok({ { "sequence", id }, { "steps", steps } });
				return EditorCommandResult::Defer([id](EditorContext& pollContext) -> std::optional<EditorCommandResult>
				{
					const InputSequence* sequence = pollContext.FindInputSequence(id);
					if (!sequence)
						return EditorCommandResult::Fail("The input sequence was replaced before it finished", EditorCommandError::Cancelled);
					if (!sequence->IsFinished())
						return std::nullopt;
					const InputSequence::Result& result = sequence->GetResult();
					if (result.Failed)
						return EditorCommandResult::Fail(result.Error, DescribeSequenceResult(result));
					return EditorCommandResult::Ok(DescribeSequenceResult(result));
				});
			}, EditorCommandKind::Session }));

		registry.Register(MayDefer({ "input.key",
			"Presses a key for the running game (play mode), as if a player did: scripts see it through Input::IsKeyDown/IsKeyPressed/"
			"IsKeyReleased, also while the viewport has no focus. tap (default) holds the key for 'steps' fixed steps of the game (or 'seconds' "
			"of game time), press holds it until a release (or input.releaseAll, play.stop), release ends every hold of the key. Each command ends "
			"only its own hold: overlapping taps of one key keep it down until the last ends. Steps are the game's own time base, so a tap lasts "
			"the same game time whatever the frame rate, and input given while the game is paused reaches it in the next step play.step runs or "
			"after play.pause false. Answers once the game has seen the input (a tap: its "
			"release), or at once with wait false (the default while paused). Returns the key, seen, and the keys and buttons still held.",
			WithDurations(ObjectSchema({
				{ "key", StringSchema(fmt::format("The key, named like the SDK's Key:: constants (case does not matter): {}",
					JoinNames(InputNames::GetKeyNames()))) },
				{ "action", ActionSchema("key") },
				{ "wait", WaitSchema() } }, { "key" }), "a tap", true),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string name = arguments.GetString("key");
				int64_t steps = 1;
				const std::optional<ButtonAction> action = ReadButtonAction(parameters, arguments, context, steps);
				const bool wait = ReadWait(arguments);
				const std::optional<KeyCode> key = InputNames::FindKey(name);
				if (arguments.IsValid() && !key)
					arguments.SetError(fmt::format("Unknown key '{}': keys are named like the SDK's Key:: constants, e.g. Left, Space, A, D1, F5, Enter", name));
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!IsGameRunning(context))
					return NoGameError();

				nlohmann::json result = { { "key", InputNames::GetKeyName(*key) }, { "action", ActionToString(*action) } };
				if (*action == ButtonAction::Tap)
					result["steps"] = steps;
				return RunButtonAction(context, SimulatedControlType::Key, *key, *action, steps, wait, std::move(result), "key");
			}, EditorCommandKind::Session }));

		registry.Register(MayDefer({ "input.mouseButton",
			"Presses a mouse button for the running game (play mode), like input.key: scripts see it through Input::IsMouseButtonDown/Pressed/"
			"Released. tap (default) holds it for 'steps' fixed steps (or 'seconds'), press until a release; answers once the game has seen it, or at once "
			"with wait false (the default while paused). Move the pointer first with input.mouseMove if the game reads where the click is.",
			WithDurations(ObjectSchema({
				{ "button", StringSchema(fmt::format("The button (case does not matter): {}; Button0, Button1 and Button2 name Left, Right and "
					"Middle too", JoinNames(InputNames::GetMouseButtonNames()))) },
				{ "action", ActionSchema("button") },
				{ "wait", WaitSchema() } }, { "button" }), "a tap", true),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string name = arguments.GetString("button");
				int64_t steps = 1;
				const std::optional<ButtonAction> action = ReadButtonAction(parameters, arguments, context, steps);
				const bool wait = ReadWait(arguments);
				const std::optional<MouseCode> button = InputNames::FindMouseButton(name);
				if (arguments.IsValid() && !button)
					arguments.SetError(fmt::format("Unknown mouse button '{}': use Left, Right, Middle or Button3 to Button7", name));
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!IsGameRunning(context))
					return NoGameError();

				nlohmann::json result = { { "button", InputNames::GetMouseButtonName(*button) }, { "action", ActionToString(*action) } };
				if (*action == ButtonAction::Tap)
					result["steps"] = steps;
				return RunButtonAction(context, SimulatedControlType::MouseButton, *button, *action, steps, wait, std::move(result), "mouse button");
			}, EditorCommandKind::Session }));

		registry.Register(MayDefer({ "input.mouseMove",
			"Moves the simulated mouse pointer of the running game (play mode) to a position in the game view, in pixels from its top-left "
			"corner: scripts read it with Input::GetMousePosition (and the move with GetMouseDelta, from the second move on). Answers once the "
			"game has seen it (its next frame), or at once with wait false (the default while paused).",
			ObjectSchema({ { "position", Vec2Schema("[x, y] in pixels from the top-left corner of the game view") }, { "wait", WaitSchema() } }, { "position" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::optional<glm::vec2> position = ReadVec2(parameters, arguments, "position");
				const bool wait = ReadWait(arguments);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!IsGameRunning(context))
					return NoGameError();
				Input::SimulateMouseMove(*position);
				return AnswerWhenSeen(context, wait, Input::GetFrameIndex() + 1, { { "position", { position->x, position->y } } }, "pointer move");
			}, EditorCommandKind::Session }));

		registry.Register(MayDefer({ "input.scroll",
			"Turns the simulated scroll wheel of the running game (play mode) for one game frame: scripts read it with Input::GetScrollDelta "
			"(+y scrolls up/away). Answers once the game has seen it, or at once with wait false (the default while paused).",
			ObjectSchema({ { "delta", Vec2Schema("[x, y] scroll offset, e.g. [0, 1] for one notch up") }, { "wait", WaitSchema() } }, { "delta" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::optional<glm::vec2> delta = ReadVec2(parameters, arguments, "delta");
				const bool wait = ReadWait(arguments);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!IsGameRunning(context))
					return NoGameError();
				Input::SimulateScroll(*delta);
				return AnswerWhenSeen(context, wait, Input::GetFrameIndex() + 1, { { "delta", { delta->x, delta->y } } }, "scroll");
			}, EditorCommandKind::Session }));

		registry.Register(MayDefer({ "input.gamepadButton",
			"Presses a gamepad button for the running game (play mode), like input.key: scripts see it through Input::IsGamepadButtonDown/"
			"Pressed/Released, and the gamepad counts as connected (Input::IsGamepadConnected) from then until play stops, with or without a "
			"device. tap (default) holds it for 'steps' fixed steps (or 'seconds'), press until a release; answers once the game has seen it, or at once "
			"with wait false (the default while paused). Returns the gamepad, the button, seen, and what is still held.",
			WithDurations(ObjectSchema({
				{ "gamepad", GamepadSchema() },
				{ "button", StringSchema(fmt::format("The button, named like the SDK's GamepadButton:: constants (case does not matter): {}",
					JoinNames(InputNames::GetGamepadButtonNames()))) },
				{ "action", ActionSchema("button") },
				{ "wait", WaitSchema() } }, { "button" }), "a tap", true),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const uint32_t gamepad = ReadGamepad(arguments);
				const std::string name = arguments.GetString("button");
				int64_t steps = 1;
				const std::optional<ButtonAction> action = ReadButtonAction(parameters, arguments, context, steps);
				const bool wait = ReadWait(arguments);
				const std::optional<GamepadButton> button = InputNames::FindGamepadButton(name);
				if (arguments.IsValid() && !button)
					arguments.SetError(fmt::format("Unknown gamepad button '{}': use {}", name, JoinNames(InputNames::GetGamepadButtonNames())));
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!IsGameRunning(context))
					return NoGameError();

				nlohmann::json result = { { "gamepad", gamepad }, { "button", InputNames::GetGamepadButtonName(*button) },
					{ "action", ActionToString(*action) } };
				if (*action == ButtonAction::Tap)
					result["steps"] = steps;
				const uint16_t code = SimulatedInput::MakeGamepadCode(gamepad, static_cast<uint32_t>(*button));
				return RunButtonAction(context, SimulatedControlType::GamepadButton, code, *action, steps, wait, std::move(result), "gamepad button");
			}, EditorCommandKind::Session }));

		registry.Register(MayDefer({ "input.gamepadAxis",
			"Moves a gamepad stick or trigger of the running game (play mode): scripts read the value through Input::GetGamepadAxis and "
			"GetGamepadStick (sticks -1 to 1 with +x right and +y up, triggers 0 released to 1 pressed; values are clamped to that range), "
			"and the gamepad counts as connected until play stops. The value holds for 'steps' fixed steps (or 'seconds' of game time) and then "
			"returns to the device's, or without a duration until another input.gamepadAxis of the axis, a value of null (back to the device's at "
			"once), input.releaseAll or play.stop. A new value of the axis replaces the one before. Answers once the game has seen it (with a "
			"duration: its return), or at once with wait false (the default while paused).",
			WithDurations(ObjectSchema({
				{ "gamepad", GamepadSchema() },
				{ "axis", StringSchema(fmt::format("The axis, named like the SDK's GamepadAxis:: constants (case does not matter): {}",
					JoinNames(InputNames::GetGamepadAxisNames()))) },
				{ "value", { { "type", { "number", "null" } }, { "description", "The axis' value (sticks -1 to 1, triggers 0 to 1), or null to give the axis "
					"back to the device" } } },
				{ "wait", WaitSchema() } }, { "axis", "value" }), "the value", false),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const uint32_t gamepad = ReadGamepad(arguments);
				const std::string name = arguments.GetString("axis");
				const int64_t steps = ReadDuration(parameters, arguments, context).value_or(0);
				const bool wait = ReadWait(arguments);
				// Null is a value here (give the axis back), which CommandArguments treats as absent: read it directly.
				const auto value = parameters.find("value");
				std::optional<float> axisValue;
				if (arguments.IsValid())
				{
					if (value == parameters.end())
						arguments.SetError("Missing parameter 'value'");
					else if (value->is_number() && std::isfinite(value->get<double>()) && std::abs(value->get<double>()) <= c_MaxInputCoordinate)
						axisValue = static_cast<float>(value->get<double>());
					else if (!value->is_null())
						arguments.SetError("Parameter 'value' must be a finite number or null");
				}
				const std::optional<GamepadAxis> axis = InputNames::FindGamepadAxis(name);
				if (arguments.IsValid() && !axis)
					arguments.SetError(fmt::format("Unknown gamepad axis '{}': use {}", name, JoinNames(InputNames::GetGamepadAxisNames())));
				if (arguments.IsValid() && !axisValue && steps > 0)
					arguments.SetError("A duration ('steps', 'seconds' or 'frames') needs a value: null gives the axis back to the device at once");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!IsGameRunning(context))
					return NoGameError();

				SimulatedInput& input = context.GetSimulatedInput();
				const uint16_t code = SimulatedInput::MakeGamepadCode(gamepad, static_cast<uint32_t>(*axis));
				nlohmann::json result = { { "gamepad", gamepad }, { "axis", InputNames::GetGamepadAxisName(*axis) } };
				if (!axisValue)
				{
					result["value"] = nullptr;
					result["released"] = input.Release(SimulatedControlType::GamepadAxis, code);
					return AnswerWhenSeen(context, wait, Input::GetFrameIndex() + 1, std::move(result), "gamepad axis");
				}

				Ref<const SimulatedHold> hold = input.HoldAxis(gamepad, *axis, *axisValue, static_cast<uint32_t>(steps), context.GetFixedStepCount());
				result["value"] = hold->Value;
				if (steps == 0)
					return AnswerWhenSeen(context, wait, hold->FirstFrame, std::move(result), "gamepad axis");
				result["steps"] = steps;
				if (!wait)
					return AnswerNow(context, std::move(result));
				return EditorCommandResult::Defer([hold, result = std::move(result)](EditorContext& pollContext) -> std::optional<EditorCommandResult>
				{
					if (!IsGameRunning(pollContext))
						return EditorCommandResult::Fail("Play mode stopped before the gamepad axis returned");
					if (!hold->Ended || Input::GetFrameIndex() < hold->EndFrame)
						return std::nullopt;
					nlohmann::json answer = result;
					answer["seen"] = true;
					// Ended early by another value of the axis, a null value, input.releaseAll or a restart of play mode.
					answer["interrupted"] = hold->Interrupted;
					answer["held"] = DescribeSimulatedInput(pollContext);
					return EditorCommandResult::Ok(std::move(answer));
				});
			}, EditorCommandKind::Session }));

		registry.Register(MayDefer({ "input.releaseAll",
			"Ends every hold of simulated input in the running game (play mode): held keys, mouse buttons and gamepad buttons go up and held "
			"gamepad axes return to the device's values in the game's next frame, "
			"unfinished taps end there (their commands answer with interrupted true), and presses still waiting for a game frame are dropped. "
			"Answers once the game has seen it, or at once with wait false (the default while paused), with what was released.",
			ObjectSchema({ { "wait", WaitSchema() } }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const bool wait = ReadWait(arguments);
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!IsGameRunning(context))
					return NoGameError();
				SimulatedInput& input = context.GetSimulatedInput();
				nlohmann::json released = {
					{ "keys", DescribeButtons(input, SimulatedControlType::Key) },
					{ "mouseButtons", DescribeButtons(input, SimulatedControlType::MouseButton) },
					{ "gamepadButtons", DescribeGamepadButtons(input) },
					{ "gamepadAxes", DescribeGamepadAxes(input) } };
				input.ReleaseAll();
				return AnswerWhenSeen(context, wait, Input::GetFrameIndex() + 1, { { "released", std::move(released) } }, "releases");
			}, EditorCommandKind::Session }));

		registry.Register({ "input.state",
			"Simulated input in the running game: the keys, mouse buttons and gamepad buttons the input.* commands hold, the gamepad axes they "
			"hold with their values, the gamepads simulated input connected (gamepads), the pointer position the game sees "
			"(null until input.mouseMove), whether the game is paused so input waits for its next frame (paused), whether input is "
			"waiting for that frame (queued), every hold with the fixed steps it covers (holds: [{type, name, firstStep, lastStep (null until "
			"released)}]), the game's fixed steps so far (fixedSteps) and whether an input.sequence runs (sequenceRunning). Device input (a person "
			"at the keyboard) is not included.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				nlohmann::json state = DescribeSimulatedInput(context);
				state["playing"] = IsGameRunning(context);
				state["paused"] = Input::IsSuspended();
				state["queued"] = Input::HasQueuedSimulatedInput();
				nlohmann::json gamepads = nlohmann::json::array();
				for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
				{
					if (Input::IsSimulatedGamepadConnected(gamepad))
						gamepads.push_back(gamepad);
				}
				state["gamepads"] = std::move(gamepads);
				// Every hold with the fixed steps it covers (lastStep null for a press that lasts until released).
				nlohmann::json holds = nlohmann::json::array();
				for (const Ref<const SimulatedHold>& hold : context.GetSimulatedInput().GetHolds())
				{
					const char* type = hold->Type == SimulatedControlType::Key ? "key" : hold->Type == SimulatedControlType::MouseButton ? "mouseButton"
						: hold->Type == SimulatedControlType::GamepadButton ? "gamepadButton" : "gamepadAxis";
					const char* name = nullptr;
					switch (hold->Type)
					{
						case SimulatedControlType::Key: name = InputNames::GetKeyName(hold->Code); break;
						case SimulatedControlType::MouseButton: name = InputNames::GetMouseButtonName(hold->Code); break;
						case SimulatedControlType::GamepadButton:
							name = InputNames::GetGamepadButtonName(static_cast<GamepadButton>(SimulatedInput::GetGamepadControl(hold->Code)));
							break;
						case SimulatedControlType::GamepadAxis:
							name = InputNames::GetGamepadAxisName(static_cast<GamepadAxis>(SimulatedInput::GetGamepadControl(hold->Code)));
							break;
					}
					holds.push_back({ { "type", type }, { "name", name ? nlohmann::json(name) : nlohmann::json(hold->Code) },
						{ "firstStep", hold->FirstStep }, { "lastStep", hold->IsTap() ? nlohmann::json(hold->LastStep) : nlohmann::json(nullptr) } });
				}
				state["holds"] = std::move(holds);
				state["fixedSteps"] = context.GetFixedStepCount();
				state["sequenceRunning"] = context.IsInputSequenceRunning();
				return EditorCommandResult::Ok(std::move(state));
			}, EditorCommandKind::Query, EditorCommandResults::Session });
	}


	namespace
	{

		// Whether `entity` is `ancestor` or below it.
		bool IsWithin(Entity entity, Entity ancestor)
		{
			for (Entity current = entity; current; current = current.GetParent())
			{
				if (current == ancestor)
					return true;
			}
			return false;
		}

		// The selectable a click on `entity` presses: itself or its nearest ancestor with a UISelectable, within its canvas.
		Entity FindSelectable(Entity entity)
		{
			for (Entity current = entity; current; current = current.GetParent())
			{
				if (current.HasComponent<UISelectableComponent>())
					return current;
				if (current.HasComponent<UICanvasComponent>())
					break;
			}
			return {};
		}

		const char* GetEventTypeName(UIEventType type)
		{
			switch (type)
			{
				case UIEventType::Click: return "click";
				case UIEventType::ValueChanged: return "valueChanged";
				case UIEventType::FocusChanged: return "focusChanged";
				case UIEventType::Submit: return "submit";
			}
			return "click";
		}

		nlohmann::json DescribeEvent(const UIEventInfo& event)
		{
			nlohmann::json described = { { "type", GetEventTypeName(event.Type) }, { "widget", UUIDToJson(event.WidgetID) },
				{ "name", event.Widget ? nlohmann::json(event.Widget.GetName()) : nlohmann::json(nullptr) } };
			if (event.Type == UIEventType::ValueChanged)
				described["value"] = event.Value;
			if (event.Type == UIEventType::FocusChanged)
				described["focused"] = event.Focused;
			return described;
		}

		// "Canvas/Menu/Play": the entity's names from its canvas down.
		std::string GetUIPath(Entity entity)
		{
			std::string path = entity.GetName();
			for (Entity current = entity.GetParent(); current; current = current.GetParent())
			{
				path = current.GetName() + "/" + path;
				if (current.HasComponent<UICanvasComponent>())
					break;
			}
			return path;
		}

	}

	void RegisterUIInputCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "input.click",
			"Clicks a game UI element of the running game (play mode) as a player would: moves the simulated pointer to the center of its "
			"rect (at the game view's size) and taps a mouse button there. Fails, changing nothing, when the element is not shown on a canvas "
			"that receives input, or when something else is on top of its center (the error names what a click would hit: the menu is "
			"covered). Answers once the game has seen the release, with entity, position (in the game view, as input.mouseMove takes it), "
			"clicked (a button clicked or a toggle or slider changed: the selectable the click pressed raised its event) and events (every UI "
			"event of the click: type click, valueChanged or focusChanged, widget, name, value, focused); with wait false at once, without "
			"clicked and events.",
			ObjectSchema({
				{ "entity", EntitySchema("The UI element to click (a button, toggle, slider, or any element whose center is a raycast target)") },
				{ "button", StringSchema("The mouse button (default Left; case does not matter)") },
				{ "frames", IntegerSchema("Fixed steps of the game the button stays down (default 1)", 1, c_MaxHoldSteps) },
				{ "wait", WaitSchema() } }, { "entity" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				Scene& scene = *context.GetActiveScene();
				CommandArguments arguments(parameters);
				const Entity target = arguments.GetEntity(scene, "entity");
				const std::string buttonName = arguments.GetString("button", "Left");
				const int64_t frames = arguments.GetInt("frames", 1, 1, c_MaxHoldSteps);
				const bool wait = ReadWait(arguments);
				const std::optional<MouseCode> button = InputNames::FindMouseButton(buttonName);
				if (arguments.IsValid() && !button)
					arguments.SetError(fmt::format("Unknown mouse button '{}': use Left, Right, Middle or Button3 to Button7", buttonName));
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!IsGameRunning(context))
					return NoGameError();
				if (!GameUIScene::HasCanvases(scene))
					return EditorCommandResult::Fail(fmt::format("'{}' is not on a UI canvas: the running scene has none", target.GetName()));

				// Where the player sees it: the layout of the game view (the UI's input screen).
				GameUIScene& ui = GameUIScene::Get(scene);
				const UIInputScreen screen = GameUIScene::GetInputScreen();
				const std::span<const UICanvasView> laidOut = ui.EnsureLayout(screen.Screen);
				const std::vector<UICanvasView> views(laidOut.begin(), laidOut.end());
				UICanvasView view;
				uint32_t element = 0;
				if (!ui.FindElement(target.GetHandle(), screen.Screen, view, element))
					return EditorCommandResult::Fail(fmt::format("'{}' is not an element of a UI canvas (it needs a UIRect under a UICanvas)", target.GetName()));
				if (!view.ReceivesInput)
					return EditorCommandResult::Fail(fmt::format("'{}' is on the canvas '{}', which does not receive input", target.GetName(),
						Entity(view.Canvas, &scene).GetName()));
				if (!ui.IsElementVisible(view, element))
					return EditorCommandResult::Fail(fmt::format("'{}' is not shown (it is inactive, or inside the checkmark of a toggle that is off)", target.GetName()));
				const UIBox box = ui.GetVisibleBox(view, element);
				if (!(box.Size.x > 0.0f && box.Size.y > 0.0f))
				{
					return EditorCommandResult::Fail(ui.GetElementClip(view, element)
						? fmt::format("'{}' is outside the clip rect of a UI Mask above it: nothing of it is shown", target.GetName())
						: fmt::format("'{}' has an empty rect", target.GetName()));
				}
				// A canvas in the scene shows the element where the game camera projects its center.
				const std::optional<glm::mat4> viewProjection = GameUIPointer::GetGameViewProjection(scene, screen.Screen);
				glm::vec2 pixel = box.GetCenter() * view.Scale;
				if (view.WorldSpace)
				{
					const std::optional<glm::vec2> projected = viewProjection
						? GameUIPointer::ProjectCanvasPoint(*viewProjection, ui.GetCanvasWorldMatrix(view), screen.Screen.Size, box.GetCenter())
						: std::nullopt;
					if (!projected)
						return EditorCommandResult::Fail(fmt::format("'{}' is on a canvas in the scene that the game's camera does not see", target.GetName()));
					pixel = *projected;
				}
				const std::optional<UIPointerRay> ray = viewProjection ? GameUIPointer::MakeRay(*viewProjection, screen.Screen.Size, pixel) : std::nullopt;
				GameUIHitTester tester;
				tester.Update(ui, views);
				const std::optional<UIHitResult> hit = tester.HitTest(pixel, false, ray ? &*ray : nullptr);
				if (!hit)
					return EditorCommandResult::Fail(fmt::format("Nothing at the center of '{}' takes clicks: it is no raycast target (an image or text that "
						"is one, or a selectable)", target.GetName()));
				const Entity hitEntity(hit->Entity, &scene);
				if (!IsWithin(hitEntity, target))
				{
					return EditorCommandResult::Fail(fmt::format("'{}' is covered: a click at its center would hit '{}'", target.GetName(), GetUIPath(hitEntity)),
						nlohmann::json { { "cover", UUIDToJson(hitEntity.GetUUID()) }, { "path", GetUIPath(hitEntity) } });
				}

				const glm::vec2 position = pixel / screen.InputToPixels;
				const Entity pressed = FindSelectable(hitEntity);
				nlohmann::json result = { { "entity", UUIDToJson(target.GetUUID()) }, { "position", { position.x, position.y } },
					{ "button", InputNames::GetMouseButtonName(*button) } };
				Input::SimulateMouseMove(position);
				if (!wait)
				{
					context.GetSimulatedInput().Press(SimulatedControlType::MouseButton, *button, static_cast<uint32_t>(frames), context.GetFixedStepCount());
					return AnswerNow(context, std::move(result));
				}

				// The click's events, collected until the game has seen the release.
				const Ref<std::vector<UIEventInfo>> events = CreateRef<std::vector<UIEventInfo>>();
				IUIEvents* source = scene.GetService<IUIEvents>();
				const UIEventListenerID listener = source ? source->AddUIEventListener([events](const UIEventInfo& event) { events->push_back(event); }) : 0;
				const std::weak_ptr<Scene> playing = context.GetActiveScene();
				Ref<const SimulatedHold> hold = context.GetSimulatedInput().Press(SimulatedControlType::MouseButton, *button, static_cast<uint32_t>(frames), context.GetFixedStepCount());
				const UUID pressedID = pressed ? pressed.GetUUID() : UUID::Null();
				return EditorCommandResult::Defer([hold, events, listener, playing, pressedID, result = std::move(result)](EditorContext& pollContext)
					-> std::optional<EditorCommandResult>
				{
					const Ref<Scene> scene = playing.lock();
					auto stopListening = [&]()
					{
						if (scene && listener != 0)
						{
							if (IUIEvents* source = scene->GetService<IUIEvents>())
								source->RemoveUIEventListener(listener);
						}
					};
					if (!IsGameRunning(pollContext) || scene != pollContext.GetActiveScene())
					{
						stopListening();
						return EditorCommandResult::Fail("Play mode stopped before the game saw the click");
					}
					if (!hold->Ended || Input::GetFrameIndex() < hold->EndFrame)
						return std::nullopt;
					stopListening();
					nlohmann::json answer = result;
					answer["seen"] = true;
					answer["interrupted"] = hold->Interrupted;
					bool clicked = false;
					nlohmann::json described = nlohmann::json::array();
					for (const UIEventInfo& event : *events)
					{
						clicked |= pressedID.IsValid() && event.WidgetID == pressedID && event.Type != UIEventType::FocusChanged;
						described.push_back(DescribeEvent(event));
					}
					answer["clicked"] = clicked;
					answer["events"] = std::move(described);
					answer["held"] = DescribeSimulatedInput(pollContext);
					return EditorCommandResult::Ok(std::move(answer));
				});
			}, EditorCommandKind::Session });

		registry.Register({ "input.type",
			"Types text into the running game (play mode), as a keyboard with its layout and input method would: scripts read it with "
			"Input::GetTypedText in the next game frame (UTF-8, at most 1,024 bytes a frame; malformed UTF-8 arrives as U+FFFD). Answers once "
			"the game has seen it, or at once with wait false (the default while paused).",
			ObjectSchema({ { "text", StringSchema("The text to type (UTF-8, 1 to 1,024 bytes)") }, { "wait", WaitSchema() } }, { "text" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string text = arguments.GetString("text");
				const bool wait = ReadWait(arguments);
				if (arguments.IsValid() && (text.empty() || text.size() > Input::c_MaxTypedTextBytes))
					arguments.SetError(fmt::format("Parameter 'text' must hold 1 to {} bytes, not {}", Input::c_MaxTypedTextBytes, text.size()));
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!IsGameRunning(context))
					return NoGameError();
				Input::SimulateText(text);
				return AnswerWhenSeen(context, wait, Input::GetFrameIndex() + 1, { { "text", text } }, "text");
			}, EditorCommandKind::Session });
	}

}
