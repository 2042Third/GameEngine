#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Editor/SimulatedInput.h"

#include <Strata/Core/Log.h>
#include <Strata/Input/Input.h>
#include <Strata/Input/InputNames.h>

#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Strata
{

	using namespace CommandUtils;

	namespace
	{

		// About half an hour of game frames at 60 frames per second.
		constexpr int64_t c_MaxHoldFrames = 100'000;
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
				{ "description", fmt::format("tap (default): hold the {0} for 'frames' game frames, then let go; press: hold it until a release, "
					"input.releaseAll or play.stop; release: end every hold of the {0} (taps too)", what) } };
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

		// Reads action and frames; frames only means something for taps.
		std::optional<ButtonAction> ReadButtonAction(CommandArguments& arguments, int64_t& outFrames)
		{
			const std::optional<ButtonAction> action = ReadAction(arguments);
			outFrames = arguments.GetInt("frames", 1, 1, c_MaxHoldFrames);
			if (action && *action != ButtonAction::Tap && arguments.Has("frames"))
				arguments.SetError("Parameter 'frames' only applies to action tap");
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

		nlohmann::json DescribeButtons(const SimulatedInput& input, SimulatedButtonType type)
		{
			nlohmann::json names = nlohmann::json::array();
			for (uint16_t code : input.GetHeld(type))
			{
				const char* name = type == SimulatedButtonType::Key ? InputNames::GetKeyName(code) : InputNames::GetMouseButtonName(code);
				// Commands only hold named buttons; a code without a name is reported by number.
				names.push_back(name ? nlohmann::json(name) : nlohmann::json(code));
			}
			return names;
		}

		// What the commands hold, and where the simulated pointer is.
		nlohmann::json DescribeSimulatedInput(const EditorContext& context)
		{
			const SimulatedInput& input = context.GetSimulatedInput();
			const std::optional<glm::vec2> position = Input::GetSimulatedMousePosition();
			return {
				{ "keys", DescribeButtons(input, SimulatedButtonType::Key) },
				{ "mouseButtons", DescribeButtons(input, SimulatedButtonType::MouseButton) },
				{ "mousePosition", position ? nlohmann::json { position->x, position->y } : nlohmann::json(nullptr) } };
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

		// Shared by input.key and input.mouseButton.
		EditorCommandResult RunButtonAction(EditorContext& context, SimulatedButtonType type, uint16_t code, ButtonAction action, int64_t frames,
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
				Ref<const SimulatedHold> hold = input.Press(type, code, 0);
				return AnswerWhenSeen(context, wait, hold->FirstFrame, std::move(result), std::move(what));
			}

			// A tap answers once the game has seen its end: the frame after its last one (unless another hold keeps the button down).
			Ref<const SimulatedHold> hold = input.Press(type, code, static_cast<uint32_t>(frames));
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

	void RegisterInputCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "input.key",
			"Presses a key for the running game (play mode), as if a player did: scripts see it through Input::IsKeyDown/IsKeyPressed/"
			"IsKeyReleased, also while the viewport has no focus. tap (default) holds the key for 'frames' game frames, press holds it until "
			"a release (or input.releaseAll, play.stop), release ends every hold of the key. Each command ends only its own hold: overlapping "
			"taps of one key keep it down until the last ends. Frames count the game's updates, so input given while the game is paused "
			"reaches it in the next frame play.step runs or after play.pause false. Answers once the game has seen the input (a tap: its "
			"release), or at once with wait false (the default while paused). Returns the key, seen, and the keys and buttons still held.",
			ObjectSchema({
				{ "key", StringSchema(fmt::format("The key, named like the SDK's Key:: constants (case does not matter): {}",
					JoinNames(InputNames::GetKeyNames()))) },
				{ "action", ActionSchema("key") },
				{ "frames", IntegerSchema("Game frames a tap holds the key (default 1)", 1, c_MaxHoldFrames) },
				{ "wait", WaitSchema() } }, { "key" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string name = arguments.GetString("key");
				int64_t frames = 1;
				const std::optional<ButtonAction> action = ReadButtonAction(arguments, frames);
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
					result["frames"] = frames;
				return RunButtonAction(context, SimulatedButtonType::Key, *key, *action, frames, wait, std::move(result), "key");
			} });

		registry.Register({ "input.mouseButton",
			"Presses a mouse button for the running game (play mode), like input.key: scripts see it through Input::IsMouseButtonDown/Pressed/"
			"Released. tap (default) holds it for 'frames' game frames, press until a release; answers once the game has seen it, or at once "
			"with wait false (the default while paused). Move the pointer first with input.mouseMove if the game reads where the click is.",
			ObjectSchema({
				{ "button", StringSchema(fmt::format("The button (case does not matter): {}; Button0, Button1 and Button2 name Left, Right and "
					"Middle too", JoinNames(InputNames::GetMouseButtonNames()))) },
				{ "action", ActionSchema("button") },
				{ "frames", IntegerSchema("Game frames a tap holds the button (default 1)", 1, c_MaxHoldFrames) },
				{ "wait", WaitSchema() } }, { "button" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string name = arguments.GetString("button");
				int64_t frames = 1;
				const std::optional<ButtonAction> action = ReadButtonAction(arguments, frames);
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
					result["frames"] = frames;
				return RunButtonAction(context, SimulatedButtonType::MouseButton, *button, *action, frames, wait, std::move(result), "mouse button");
			} });

		registry.Register({ "input.mouseMove",
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
			} });

		registry.Register({ "input.scroll",
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
			} });

		registry.Register({ "input.releaseAll",
			"Ends every hold of simulated input in the running game (play mode): held keys and mouse buttons go up in the game's next frame, "
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
					{ "keys", DescribeButtons(input, SimulatedButtonType::Key) },
					{ "mouseButtons", DescribeButtons(input, SimulatedButtonType::MouseButton) } };
				input.ReleaseAll();
				return AnswerWhenSeen(context, wait, Input::GetFrameIndex() + 1, { { "released", std::move(released) } }, "releases");
			} });

		registry.Register({ "input.state",
			"Simulated input in the running game: the keys and mouse buttons the input.* commands hold, the pointer position the game sees "
			"(null until input.mouseMove), whether the game is paused so input waits for its next frame (paused), and whether input is "
			"waiting for that frame (queued). Device input (a person at the keyboard) is not included.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				nlohmann::json state = DescribeSimulatedInput(context);
				state["playing"] = IsGameRunning(context);
				state["paused"] = Input::IsSuspended();
				state["queued"] = Input::HasQueuedSimulatedInput();
				return EditorCommandResult::Ok(std::move(state));
			} });
	}

}
