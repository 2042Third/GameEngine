#include "Editor/CommandUtils.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"

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

		// About half an hour at 60 frames per second.
		constexpr int64_t c_MaxHoldFrames = 100'000;
		// Pointer positions and scroll offsets beyond this are mistakes, not input.
		constexpr double c_MaxInputCoordinate = 1'000'000.0;

		enum class ButtonAction : uint8_t
		{
			Tap = 0,
			Press,
			Release
		};

		// Input::SimulateKey or Input::SimulateMouseButton (key and mouse codes share their type).
		using SimulateButtonFunction = void (*)(uint16_t code, bool down);

		nlohmann::json NamesSchema(std::span<const char* const> names, std::string description)
		{
			nlohmann::json values = nlohmann::json::array();
			for (const char* name : names)
				values.push_back(name);
			return { { "type", "string" }, { "enum", std::move(values) }, { "description", std::move(description) } };
		}

		nlohmann::json ActionSchema(const char* what)
		{
			return { { "type", "string" }, { "enum", { "tap", "press", "release" } },
				{ "description", fmt::format("tap (default): hold the {0} for 'frames' frames, then release it; press: hold it until a release (or "
					"play.stop); release: let it go", what) } };
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

		// Simulated input is meant for the game's scripts, which only run in play mode.
		bool IsGameRunning(const EditorContext& context)
		{
			return context.GetSceneState() == SceneState::Play;
		}

		EditorCommandResult NoGameError()
		{
			return EditorCommandResult::Fail("Simulated input goes to a running game: start it with play.start first");
		}

		std::vector<KeyCode> GetSimulatedKeys()
		{
			std::vector<KeyCode> keys;
			for (KeyCode key = 0; key < c_MaxKeyCode; key++)
			{
				if (Input::IsSimulatedKeyDown(key))
					keys.push_back(key);
			}
			return keys;
		}

		std::vector<MouseCode> GetSimulatedMouseButtons()
		{
			std::vector<MouseCode> buttons;
			for (MouseCode button = 0; button < c_MaxMouseButtons; button++)
			{
				if (Input::IsSimulatedMouseButtonDown(button))
					buttons.push_back(button);
			}
			return buttons;
		}

		nlohmann::json DescribeKeys(const std::vector<KeyCode>& keys)
		{
			nlohmann::json names = nlohmann::json::array();
			for (KeyCode key : keys)
			{
				// Only named keys can be simulated through commands; a code without a name is reported by number.
				const char* name = InputNames::GetKeyName(key);
				names.push_back(name ? nlohmann::json(name) : nlohmann::json(key));
			}
			return names;
		}

		nlohmann::json DescribeMouseButtons(const std::vector<MouseCode>& buttons)
		{
			nlohmann::json names = nlohmann::json::array();
			for (MouseCode button : buttons)
				names.push_back(InputNames::GetMouseButtonName(button));
			return names;
		}

		nlohmann::json DescribeSimulatedInput()
		{
			const std::optional<glm::vec2> position = Input::GetSimulatedMousePosition();
			return {
				{ "keys", DescribeKeys(GetSimulatedKeys()) },
				{ "mouseButtons", DescribeMouseButtons(GetSimulatedMouseButtons()) },
				{ "mousePosition", position ? nlohmann::json { position->x, position->y } : nlohmann::json(nullptr) } };
		}

		// Answers once the game had a frame with the queued input applied.
		EditorCommandResult AnswerAfterNextFrame(nlohmann::json result, std::string what)
		{
			return EditorCommandResult::Defer([result = std::move(result), what = std::move(what)](EditorContext& context) -> std::optional<EditorCommandResult>
			{
				if (!IsGameRunning(context))
					return EditorCommandResult::Fail(fmt::format("Play mode stopped before the game saw the {}", what));
				nlohmann::json answer = result;
				answer["held"] = DescribeSimulatedInput();
				return EditorCommandResult::Ok(std::move(answer));
			});
		}

		// Shared by input.key and input.mouseButton.
		EditorCommandResult RunButtonAction(SimulateButtonFunction simulate, uint16_t code, ButtonAction action, int64_t frames, nlohmann::json result,
			std::string what)
		{
			if (action != ButtonAction::Tap)
			{
				simulate(code, action == ButtonAction::Press);
				return AnswerAfterNextFrame(std::move(result), what);
			}

			// Polled after the update of each frame: the first poll follows the first frame that saw the button down.
			simulate(code, true);
			return EditorCommandResult::Defer([simulate, code, frames, result = std::move(result), what = std::move(what), held = int64_t(0),
				released = false](EditorContext& context) mutable -> std::optional<EditorCommandResult>
			{
				if (!IsGameRunning(context))
					return EditorCommandResult::Fail(fmt::format("Play mode stopped before the {} was released", what));
				if (!released)
				{
					if (++held < frames)
						return std::nullopt;
					simulate(code, false);
					released = true;
					return std::nullopt;
				}
				nlohmann::json answer = result;
				answer["held"] = DescribeSimulatedInput();
				return EditorCommandResult::Ok(std::move(answer));
			});
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

	}

	void RegisterInputCommands(EditorCommandRegistry& registry)
	{
		registry.Register({ "input.key",
			"Presses a key for the running game (play mode), as if a player did: scripts see it through Input::IsKeyDown/IsKeyPressed/"
			"IsKeyReleased, also while the viewport has no focus. tap (default) holds the key for 'frames' frames and answers after the game "
			"saw it released; press and release answer after one frame. Keys stay held across commands until released, released by "
			"input.releaseAll, or dropped by play.stop. A paused game only sees input in the frames play.step runs. Returns the key and the "
			"simulated keys and buttons still held.",
			ObjectSchema({
				{ "key", NamesSchema(InputNames::GetKeyNames(), "The key, named like the SDK's Key:: constants (Left, Space, A, D1, F5, Enter, "
					"LeftShift; case does not matter)") },
				{ "action", ActionSchema("key") },
				{ "frames", IntegerSchema("Frames a tap holds the key (default 1)", 1, c_MaxHoldFrames) } }, { "key" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string name = arguments.GetString("key");
				int64_t frames = 1;
				const std::optional<ButtonAction> action = ReadButtonAction(arguments, frames);
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
				return RunButtonAction(&Input::SimulateKey, *key, *action, frames, std::move(result), "key");
			} });

		registry.Register({ "input.mouseButton",
			"Presses a mouse button for the running game (play mode), like input.key: scripts see it through Input::IsMouseButtonDown/Pressed/"
			"Released. tap (default) holds it for 'frames' frames; press and release answer after one frame. Move the pointer first with "
			"input.mouseMove if the game reads where the click is.",
			ObjectSchema({
				{ "button", NamesSchema(InputNames::GetMouseButtonNames(), "The button: Left, Right, Middle, Button3 to Button7") },
				{ "action", ActionSchema("button") },
				{ "frames", IntegerSchema("Frames a tap holds the button (default 1)", 1, c_MaxHoldFrames) } }, { "button" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::string name = arguments.GetString("button");
				int64_t frames = 1;
				const std::optional<ButtonAction> action = ReadButtonAction(arguments, frames);
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
				return RunButtonAction(&Input::SimulateMouseButton, *button, *action, frames, std::move(result), "mouse button");
			} });

		registry.Register({ "input.mouseMove",
			"Moves the simulated mouse pointer of the running game (play mode) to a position in the game view, in pixels from its top-left "
			"corner: scripts read it with Input::GetMousePosition (and the move with GetMouseDelta, from the second move on). Answers after one frame.",
			ObjectSchema({ { "position", Vec2Schema("[x, y] in pixels from the top-left corner of the game view") } }, { "position" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::optional<glm::vec2> position = ReadVec2(parameters, arguments, "position");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!IsGameRunning(context))
					return NoGameError();
				Input::SimulateMouseMove(*position);
				return AnswerAfterNextFrame({ { "position", { position->x, position->y } } }, "pointer move");
			} });

		registry.Register({ "input.scroll",
			"Turns the simulated scroll wheel of the running game (play mode) for one frame: scripts read it with Input::GetScrollDelta "
			"(+y scrolls up/away). Answers after that frame.",
			ObjectSchema({ { "delta", Vec2Schema("[x, y] scroll offset, e.g. [0, 1] for one notch up") } }, { "delta" }),
			[](EditorContext& context, const nlohmann::json& parameters)
			{
				CommandArguments arguments(parameters);
				const std::optional<glm::vec2> delta = ReadVec2(parameters, arguments, "delta");
				if (!arguments.IsValid())
					return arguments.Fail();
				if (!IsGameRunning(context))
					return NoGameError();
				Input::SimulateScroll(*delta);
				return AnswerAfterNextFrame({ { "delta", { delta->x, delta->y } } }, "scroll");
			} });

		registry.Register({ "input.releaseAll",
			"Releases every key and mouse button that simulated input holds in the running game (play mode); the game sees them released "
			"in the next frame. Answers after that frame with what was released.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				if (!IsGameRunning(context))
					return NoGameError();
				const std::vector<KeyCode> keys = GetSimulatedKeys();
				const std::vector<MouseCode> buttons = GetSimulatedMouseButtons();
				for (KeyCode key : keys)
					Input::SimulateKey(key, false);
				for (MouseCode button : buttons)
					Input::SimulateMouseButton(button, false);
				return AnswerAfterNextFrame({ { "released", { { "keys", DescribeKeys(keys) }, { "mouseButtons", DescribeMouseButtons(buttons) } } } },
					"releases");
			} });

		registry.Register({ "input.state",
			"The simulated input the running game sees this frame: held keys and mouse buttons and the pointer position (null until "
			"input.mouseMove). Device input (a person at the keyboard) is not included.",
			ObjectSchema({}),
			[](EditorContext& context, const nlohmann::json&)
			{
				nlohmann::json state = DescribeSimulatedInput();
				state["playing"] = context.GetSceneState() == SceneState::Play;
				return EditorCommandResult::Ok(std::move(state));
			} });
	}

}
