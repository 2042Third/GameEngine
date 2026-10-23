#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Scripting/ScriptTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Input/Input.h>
#include <Strata/Input/InputActions.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Scripting/ScriptSystem.h>

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// A project playing the API test module's InputProbe (it samples the input queries every update into its fields), with
	// frames run the way the application runs them: input frame, input actions, editor update, then the pending commands.
	struct InputHarness
	{
		EditorContext Context { EditorContextSpecification { false, false } };
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;
		std::string Probe;

		InputHarness()
		{
			Input::Reset();
			const std::filesystem::path directory = CreateTemporaryDirectory("EditorInput") / "Input Game";
			Run("project.create", { { "directory", FileSystem::ToUTF8(directory) }, { "name", "Input Game" } });
			Run("script.load", { { "path", FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)) } });
			Probe = Run("entity.create", { { "name", "Probe" } })["id"].get<std::string>();
			Run("script.add", { { "entity", Probe }, { "class", "InputProbe" } });
		}

		~InputHarness()
		{
			Context.Stop();
			Input::Reset();
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			INFO(std::string(name), ": ", result.Error);
			REQUIRE(result.Success);
			REQUIRE_FALSE(result.IsPending());
			return result.Value;
		}

		EditorCommandResult Fail(std::string_view name, const nlohmann::json& parameters)
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			REQUIRE_FALSE(result.IsPending());
			CHECK_FALSE(result.Success);
			return result;
		}

		// Starts a command through the runner; `outResult` receives its result when it completes.
		void Start(std::string_view name, const nlohmann::json& parameters, std::optional<EditorCommandResult>& outResult)
		{
			outResult.reset();
			Runner.Run(Context, Commands, name, parameters, [&outResult](const EditorCommandResult& result) { outResult = result; });
		}

		void Frame(float seconds = 1.0f / 60.0f)
		{
			Input::BeginFrame();
			InputActions::Update();
			Context.Update(Timestep(seconds));
			Runner.Update(Context);
		}

		// The project's input actions: Gameplay (Jump on J, Steer an Axis on A and D, Move a Vector2 on the left stick, Left on
		// Q beside UI/Left) and Vehicle (Horn on H), disabled at start.
		void AddActions()
		{
			RunCommandToCompletion(Context, Commands, "inputActions.setMap", { { "map", "Gameplay" } });
			RunCommandToCompletion(Context, Commands, "inputActions.setAction", { { "action", "Gameplay/Jump" }, { "bindings", { "Key/J" } } });
			RunCommandToCompletion(Context, Commands, "inputActions.setAction", { { "action", "Gameplay/Steer" }, { "kind", "Axis" },
				{ "bindings", { "1D:Key/A,Key/D" } } });
			RunCommandToCompletion(Context, Commands, "inputActions.setAction", { { "action", "Gameplay/Move" }, { "kind", "Vector2" },
				{ "bindings", { "Gamepad/LeftStick" } } });
			RunCommandToCompletion(Context, Commands, "inputActions.setAction", { { "action", "Gameplay/Left" }, { "bindings", { "Key/Q" } } });
			RunCommandToCompletion(Context, Commands, "inputActions.setMap", { { "map", "Vehicle" }, { "enabled", false } });
			RunCommandToCompletion(Context, Commands, "inputActions.setAction", { { "action", "Vehicle/Horn" }, { "bindings", { "Key/H" } } });
		}

		// A ground (a static box whose top is y = 0) and "Hero", a character controller at the origin walked by the API
		// module's KeyboardWalker (W walks toward -Z at 3 m/s, Space jumps). Returns Hero's id.
		std::string AddWalker()
		{
			Run("entity.create", { { "name", "Ground" }, { "components", { { "Transform", { { "Translation", { 0, -0.5, 0 } } } },
				{ "BoxCollider", { { "HalfExtents", { 20, 0.5, 20 } } } } } } });
			const std::string hero = Run("entity.create", { { "name", "Hero" }, { "components", { { "CharacterController", nlohmann::json::object() } } } })["id"]
				.get<std::string>();
			Run("script.add", { { "entity", hero }, { "class", "KeyboardWalker" } });
			return hero;
		}

		// Runs a command through the runner, frames of `seconds` each, until it answers.
		EditorCommandResult RunToEnd(std::string_view name, const nlohmann::json& parameters, float seconds = 1.0f / 60.0f)
		{
			std::optional<EditorCommandResult> result;
			Start(name, parameters, result);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
			while (!result && std::chrono::steady_clock::now() < deadline)
				Frame(seconds);
			REQUIRE(result);
			return *result;
		}

		bool ProbeField(std::string_view field)
		{
			Ref<Scene> scene = Context.GetActiveScene();
			Entity probe = scene->GetEntityByUUID(*UUIDFromJson(Probe));
			return GetField<bool>(GetScriptSystem(*scene), probe, "InputProbe", field);
		}

		// The entry of input.state's actions for an action, or null.
		nlohmann::json FindActionState(std::string_view action)
		{
			const nlohmann::json state = Run("input.state");
			for (const nlohmann::json& entry : state["actions"])
			{
				if (entry["action"] == std::string(action))
					return entry;
			}
			return nullptr;
		}

		glm::vec2 ProbeVector(std::string_view field)
		{
			Ref<Scene> scene = Context.GetActiveScene();
			Entity probe = scene->GetEntityByUUID(*UUIDFromJson(Probe));
			return GetField<glm::vec2>(GetScriptSystem(*scene), probe, "InputProbe", field);
		}
	};

}

TEST_SUITE("Editor.Input")
{
	TEST_CASE("Simulated input needs a running game and valid names")
	{
		InputHarness harness;
		EditorCommandResult result = harness.Fail("input.key", { { "key", "W" } });
		CHECK(result.ErrorKind == EditorCommandError::Failed);
		CHECK(result.Error.find("play.start") != std::string::npos);
		CHECK(harness.Fail("input.mouseMove", { { "position", { 1, 2 } } }).ErrorKind == EditorCommandError::Failed);
		CHECK(harness.Fail("input.releaseAll", nlohmann::json::object()).ErrorKind == EditorCommandError::Failed);
		CHECK(harness.Run("input.state")["playing"] == false);

		harness.Run("play.simulate"); // Physics only: no scripts to receive input
		CHECK(harness.Fail("input.key", { { "key", "W" } }).ErrorKind == EditorCommandError::Failed);
		harness.Run("play.stop");

		harness.Run("play.start");
		result = harness.Fail("input.key", { { "key", "Windows" } });
		CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(result.Error.find("Windows") != std::string::npos);
		CHECK(harness.Fail("input.key", { { "key", "W" }, { "action", "hold" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.key", { { "key", "W" }, { "action", "press" }, { "frames", 3 } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.key", { { "key", "W" }, { "frames", 0 } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.key", nlohmann::json::object()).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.mouseButton", { { "button", "Button8" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.mouseMove", { { "position", { 1, "2" } } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.scroll", { { "delta", { 0, 1, 2 } } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.scroll", { { "delta", { 0, 1e300 } } }).ErrorKind == EditorCommandError::InvalidParameters);

		// Nothing was simulated by the refused requests.
		harness.Frame();
		CHECK_FALSE(harness.ProbeField("WDown"));
		CHECK(harness.Run("input.state")["keys"].empty());
	}

	TEST_CASE("A tap holds a key for the given frames and answers after the game saw the release")
	{
		InputHarness harness;
		harness.Run("play.start");
		harness.Frame();
		CHECK_FALSE(harness.ProbeField("WDown"));

		std::optional<EditorCommandResult> result;
		harness.Start("input.key", { { "key", "w" }, { "frames", 2 } }, result); // Names ignore case
		CHECK_FALSE(result);

		harness.Frame();
		CHECK(harness.ProbeField("WDown"));
		CHECK(harness.ProbeField("WPressed"));
		CHECK_FALSE(result);

		harness.Frame();
		CHECK(harness.ProbeField("WDown"));
		CHECK_FALSE(harness.ProbeField("WPressed"));
		CHECK_FALSE(result);

		harness.Frame();
		CHECK_FALSE(harness.ProbeField("WDown"));
		CHECK(harness.ProbeField("WReleased"));
		REQUIRE(result);
		REQUIRE(result->Success);
		CHECK(result->Value["key"] == "W");
		CHECK(result->Value["action"] == "tap");
		CHECK(result->Value["steps"] == 2); // frames is an older name of steps
		CHECK(result->Value["held"]["keys"].empty());

		harness.Frame();
		CHECK_FALSE(harness.ProbeField("WReleased"));
	}

	TEST_CASE("Pressed keys stay down across commands until released, and reach an unfocused game view")
	{
		InputHarness harness;
		harness.Run("play.start");
		// The editor disables device input while its game view has no focus; a tool's input still comes through.
		Input::SetEnabled(false);

		std::optional<EditorCommandResult> result;
		harness.Start("input.key", { { "key", "W" }, { "action", "press" } }, result);
		harness.Frame();
		CHECK(harness.ProbeField("WPressed"));
		REQUIRE(result);
		REQUIRE(result->Success);
		CHECK(result->Value["held"]["keys"] == nlohmann::json { "W" });
		CHECK_FALSE(result->Value.contains("frames"));

		harness.Start("input.mouseButton", { { "button", "Left" }, { "action", "press" } }, result);
		harness.Frame();
		REQUIRE(result);
		CHECK(harness.ProbeField("LeftPressed"));
		for (int frame = 0; frame < 5; frame++)
			harness.Frame();
		CHECK(harness.ProbeField("WDown"));
		CHECK(harness.ProbeField("LeftDown"));
		const nlohmann::json state = harness.Run("input.state");
		CHECK(state["playing"] == true);
		CHECK(state["keys"] == nlohmann::json { "W" });
		CHECK(state["mouseButtons"] == nlohmann::json { "Left" });

		harness.Start("input.key", { { "key", "W" }, { "action", "release" } }, result);
		harness.Frame();
		CHECK(harness.ProbeField("WReleased"));
		CHECK(harness.ProbeField("LeftDown"));
		REQUIRE(result);
		CHECK(result->Value["held"]["keys"].empty());
		CHECK(result->Value["held"]["mouseButtons"] == nlohmann::json { "Left" });

		harness.Start("input.releaseAll", nlohmann::json::object(), result);
		harness.Frame();
		CHECK(harness.ProbeField("LeftReleased"));
		REQUIRE(result);
		CHECK(result->Value["released"]["mouseButtons"] == nlohmann::json { "Left" });
		CHECK(result->Value["released"]["keys"].empty());
		CHECK(result->Value["held"]["mouseButtons"].empty());
		Input::SetEnabled(true);
	}

	TEST_CASE("Pointer moves and scrolling reach the game")
	{
		InputHarness harness;
		harness.Run("play.start");

		std::optional<EditorCommandResult> result;
		harness.Start("input.mouseMove", { { "position", { 100, 50 } } }, result);
		harness.Frame();
		REQUIRE(result);
		REQUIRE(result->Success);
		CHECK(harness.ProbeVector("MousePosition") == glm::vec2(100.0f, 50.0f));
		CHECK(harness.ProbeVector("MouseDelta") == glm::vec2(0.0f)); // The first position only places the pointer
		CHECK(result->Value["held"]["mousePosition"] == nlohmann::json { 100.0, 50.0 });

		harness.Start("input.mouseMove", { { "position", { 110, 45 } } }, result);
		harness.Frame();
		CHECK(harness.ProbeVector("MousePosition") == glm::vec2(110.0f, 45.0f));
		CHECK(harness.ProbeVector("MouseDelta") == glm::vec2(10.0f, -5.0f));

		harness.Start("input.scroll", { { "delta", { 0, 2 } } }, result);
		harness.Frame();
		REQUIRE(result);
		CHECK(harness.ProbeVector("ScrollDelta") == glm::vec2(0.0f, 2.0f));
		CHECK(harness.ProbeVector("MouseDelta") == glm::vec2(0.0f));
		harness.Frame();
		CHECK(harness.ProbeVector("ScrollDelta") == glm::vec2(0.0f));
		CHECK(harness.ProbeVector("MousePosition") == glm::vec2(110.0f, 45.0f));
	}

	TEST_CASE("Stopping the game drops simulated input and fails unfinished taps")
	{
		InputHarness harness;
		harness.Run("play.start");
		std::optional<EditorCommandResult> press;
		harness.Start("input.key", { { "key", "W" }, { "action", "press" } }, press);
		harness.Frame();
		REQUIRE(press);

		std::optional<EditorCommandResult> tap;
		harness.Start("input.mouseButton", { { "button", "Right" }, { "frames", 10 } }, tap);
		harness.Frame();
		harness.Run("play.stop");
		harness.Frame();
		REQUIRE(tap);
		CHECK_FALSE(tap->Success);
		CHECK(tap->Error.find("stopped") != std::string::npos);
		CHECK(harness.Run("input.state")["keys"].empty());

		// A new session starts without the key the previous one held.
		harness.Run("play.start");
		harness.Frame();
		CHECK_FALSE(harness.ProbeField("WDown"));
		CHECK_FALSE(harness.ProbeField("WReleased"));
	}

	TEST_CASE("Input given while the game is paused reaches the next stepped frame")
	{
		InputHarness harness;
		harness.Run("play.start");
		harness.Frame();
		harness.Run("play.pause", { { "paused", true } });

		// While paused the commands answer at once: the game has not seen the input yet.
		nlohmann::json pressed = harness.Run("input.key", { { "key", "W" }, { "action", "press" } });
		CHECK(pressed["seen"] == false);
		CHECK(pressed["held"]["keys"] == nlohmann::json { "W" });
		nlohmann::json state = harness.Run("input.state");
		CHECK(state["paused"] == true);
		CHECK(state["queued"] == true);
		for (int frame = 0; frame < 5; frame++)
			harness.Frame();
		CHECK_FALSE(harness.ProbeField("WDown")); // The probe has not run since the press

		// The first stepped frame sees the press as a press, not just a held key (play.step answers at once with wait: false, so
		// the test runs the frames itself).
		harness.Run("play.step", { { "frames", 1 }, { "wait", false } });
		harness.Frame();
		CHECK(harness.ProbeField("WDown"));
		CHECK(harness.ProbeField("WPressed"));
		harness.Frame();
		harness.Run("play.step", { { "frames", 1 }, { "wait", false } });
		harness.Frame();
		CHECK(harness.ProbeField("WDown"));
		CHECK_FALSE(harness.ProbeField("WPressed"));

		// A one-frame tap while paused: down in the next stepped frame, up in the one after.
		harness.Run("input.key", { { "key", "W" }, { "action", "release" } });
		harness.Run("input.mouseButton", { { "button", "Left" } });
		harness.Run("play.step", { { "frames", 1 }, { "wait", false } });
		harness.Frame();
		CHECK(harness.ProbeField("WReleased"));
		CHECK(harness.ProbeField("LeftPressed"));
		CHECK(harness.ProbeField("LeftDown"));
		for (int frame = 0; frame < 3; frame++)
			harness.Frame();
		harness.Run("play.step", { { "frames", 1 }, { "wait", false } });
		harness.Frame();
		CHECK_FALSE(harness.ProbeField("LeftDown"));
		CHECK(harness.ProbeField("LeftReleased"));

		// Asked to wait, a tap answers once a stepped frame saw its release.
		std::optional<EditorCommandResult> tap;
		harness.Start("input.key", { { "key", "W" }, { "wait", true } }, tap);
		for (int frame = 0; frame < 3; frame++)
			harness.Frame();
		CHECK_FALSE(tap);
		harness.Run("play.step", { { "frames", 2 }, { "wait", false } });
		harness.Frame();
		CHECK(harness.ProbeField("WPressed"));
		CHECK_FALSE(tap);
		harness.Frame();
		CHECK(harness.ProbeField("WReleased"));
		REQUIRE(tap);
		REQUIRE(tap->Success);
		CHECK(tap->Value["seen"] == true);
		CHECK(tap->Value["interrupted"] == false);

		// Resuming applies what waited, too.
		harness.Run("input.key", { { "key", "W" }, { "action", "press" } });
		harness.Run("play.pause", { { "paused", false } });
		CHECK(harness.Run("input.state")["paused"] == false);
		harness.Frame();
		CHECK(harness.ProbeField("WPressed"));
	}

	TEST_CASE("Each command ends only its own hold")
	{
		InputHarness harness;
		harness.Run("play.start");
		harness.Frame();

		// Overlapping taps of one key: down with the first, up when the longer one ends.
		std::optional<EditorCommandResult> longTap;
		std::optional<EditorCommandResult> shortTap;
		harness.Start("input.key", { { "key", "W" }, { "frames", 4 } }, longTap);
		harness.Start("input.key", { { "key", "W" }, { "frames", 1 } }, shortTap);
		harness.Frame();
		CHECK(harness.ProbeField("WPressed"));
		harness.Frame();
		REQUIRE(shortTap); // Its hold ended; the key stays down for the other
		CHECK(shortTap->Value["seen"] == true);
		CHECK(shortTap->Value["held"]["keys"] == nlohmann::json { "W" });
		for (int frame = 0; frame < 2; frame++)
		{
			harness.Frame();
			CHECK(harness.ProbeField("WDown"));
			CHECK_FALSE(harness.ProbeField("WReleased"));
		}
		CHECK_FALSE(longTap);
		harness.Frame();
		CHECK(harness.ProbeField("WReleased"));
		REQUIRE(longTap);
		CHECK(longTap->Value["interrupted"] == false);

		// releaseAll ends a running tap; a later press of the key is not ended when the tap's frames run out.
		std::optional<EditorCommandResult> interrupted;
		harness.Start("input.key", { { "key", "D" }, { "frames", 5 } }, interrupted);
		harness.Frame();
		CHECK(harness.ProbeField("WDown") == false);
		harness.Run("input.releaseAll", { { "wait", false } });
		harness.Run("input.key", { { "key", "D" }, { "action", "press" }, { "wait", false } });
		for (int frame = 0; frame < 8; frame++)
			harness.Frame();
		REQUIRE(interrupted);
		CHECK(interrupted->Value["interrupted"] == true);
		CHECK(harness.Run("input.state")["keys"] == nlohmann::json { "D" });

		// releaseAll also drops a press that is still waiting for its frame.
		harness.Run("input.releaseAll", { { "wait", false } });
		harness.Run("input.key", { { "key", "W" }, { "action", "press" }, { "wait", false } });
		harness.Run("input.releaseAll", { { "wait", false } });
		harness.Frame();
		CHECK_FALSE(harness.ProbeField("WDown"));
		CHECK_FALSE(harness.ProbeField("WPressed"));
		CHECK(harness.Run("input.state")["keys"].empty());
	}

	TEST_CASE("Key and button names match ignoring case, and the schemas allow that")
	{
		InputHarness harness;
		// An enum in the schema would make clients that validate requests refuse "left", which the commands accept.
		const EditorCommand* key = harness.Commands.Find("input.key");
		const EditorCommand* button = harness.Commands.Find("input.mouseButton");
		REQUIRE(key);
		REQUIRE(button);
		const nlohmann::json& keySchema = key->Parameters["properties"]["key"];
		const nlohmann::json& buttonSchema = button->Parameters["properties"]["button"];
		CHECK_FALSE(keySchema.contains("enum"));
		CHECK_FALSE(buttonSchema.contains("enum"));
		CHECK(keySchema["description"].get<std::string>().find("KPEnter") != std::string::npos);
		CHECK(buttonSchema["description"].get<std::string>().find("Button0") != std::string::npos);

		harness.Run("play.start");
		harness.Run("input.mouseButton", { { "button", "left" }, { "action", "press" }, { "wait", false } });
		harness.Run("input.key", { { "key", "w" }, { "action", "press" }, { "wait", false } });
		harness.Frame();
		CHECK(harness.ProbeField("LeftPressed"));
		CHECK(harness.ProbeField("WPressed"));
		harness.Run("input.mouseButton", { { "button", "BUTTON0" }, { "action", "release" }, { "wait", false } });
		harness.Frame();
		CHECK(harness.ProbeField("LeftReleased"));
	}

	TEST_CASE("Holds are listed for people, so they can be released when the tool is gone")
	{
		InputHarness harness;
		harness.Run("play.start");
		const SimulatedInput& simulated = harness.Context.GetSimulatedInput();
		CHECK_FALSE(simulated.HasHolds());
		CHECK(simulated.DescribeHolds().empty());
		CHECK(simulated.GetHeldCount() == 0);

		harness.Run("input.key", { { "key", "W" }, { "action", "press" }, { "wait", false } });
		harness.Run("input.key", { { "key", "Space" }, { "frames", 100 }, { "wait", false } });
		harness.Run("input.mouseButton", { { "button", "Left" }, { "action", "press" }, { "wait", false } });
		CHECK(simulated.HasHolds());
		CHECK(simulated.DescribeHolds() == "Space, W, Mouse Left");
		// What the status bar's compact pill counts.
		CHECK(simulated.GetHeldCount() == 3);

		// What the status bar's Release button runs.
		harness.Run("input.releaseAll", { { "wait", false } });
		CHECK_FALSE(simulated.HasHolds());
		harness.Frame();
		CHECK_FALSE(harness.ProbeField("WDown"));
		CHECK_FALSE(harness.ProbeField("LeftDown"));
	}

	TEST_CASE("A tap lasts its fixed steps whatever the frame rate")
	{
		InputHarness harness;
		harness.Run("play.start");
		harness.Frame();
		// Frames of 1 ms: a fixed step about every 17 frames. The key is down for exactly 30 of them (the base held it for 30
		// frames: about 2 steps).
		std::optional<EditorCommandResult> tap;
		harness.Start("input.key", { { "key", "W" }, { "steps", 30 } }, tap);
		uint64_t stepsDown = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
		while (!tap && std::chrono::steady_clock::now() < deadline)
		{
			const uint64_t before = harness.Context.GetFixedStepCount();
			harness.Frame(0.001f);
			if (harness.ProbeField("WDown"))
				stepsDown += harness.Context.GetFixedStepCount() - before;
		}
		REQUIRE(tap);
		REQUIRE(tap->Success);
		CHECK(tap->Value["steps"] == 30);
		CHECK(stepsDown == 30);

		// Seconds of game time round up to whole fixed steps; one duration only.
		const nlohmann::json timed = harness.Run("input.key", { { "key", "W" }, { "seconds", 0.1 }, { "wait", false } });
		CHECK(timed["steps"] == 6);
		CHECK(harness.Fail("input.key", { { "key", "W" }, { "steps", 2 }, { "frames", 2 } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.key", { { "key", "W" }, { "seconds", -1 } }).ErrorKind == EditorCommandError::InvalidParameters);
		// input.state reports holds with their steps.
		const nlohmann::json state = harness.Run("input.state");
		REQUIRE(state["holds"].size() == 1);
		CHECK(state["holds"][0]["name"] == "W");
		CHECK(state["holds"][0]["lastStep"].get<uint64_t>() - state["holds"][0]["firstStep"].get<uint64_t>() == 5);
	}

	TEST_CASE("An input sequence gives the same game whatever the frame rate")
	{
		const nlohmann::json sequence = {
			{ "events", { { { "step", 0 }, { "key", "W" }, { "action", "tap" }, { "steps", 60 } }, { { "step", 20 }, { "key", "Space" } } } },
			{ "steps", 90 } };
		std::optional<nlohmann::json> reference;
		for (const float frameSeconds : { 0.001f, 1.0f / 60.0f, 0.05f })
		{
			CAPTURE(frameSeconds);
			InputHarness harness;
			const std::string hero = harness.AddWalker();
			// Started right away, before the game's first update: every run starts from the same state.
			harness.Run("play.start");
			nlohmann::json parameters = sequence;
			parameters["observe"] = { { { "entity", hero }, { "world", true }, { "every", 10 } }, { { "entity", hero }, { "script", "KeyboardWalker" },
				{ "field", "Jumps" } } };
			const uint64_t start = harness.Context.GetFixedStepCount();
			const EditorCommandResult result = harness.RunToEnd("input.sequence", parameters, frameSeconds);
			REQUIRE_MESSAGE(result.Success, result.Error);
			CHECK(result.Value["stepsRun"] == 90);
			CHECK(harness.Context.GetFixedStepCount() == start + 90);
			CHECK(result.Value["seen"].size() == 2);
			CHECK(result.Value["interrupted"] == false);
			const nlohmann::json& samples = result.Value["samples"];
			REQUIRE(samples.size() == 9); // Steps 10, 20, ..., 90
			CHECK(samples.back()["step"] == 90);
			// The final sample has both observations: the jump happened once, and 60 steps at 3 m/s walk 3 m along -Z.
			REQUIRE(samples.back()["values"].size() == 2);
			CHECK(samples.back()["values"][1]["value"] == 1);
			const nlohmann::json& world = samples.back()["values"][0]["value"];
			CHECK(world["position"][2].get<double>() == doctest::Approx(-3.0).epsilon(1e-4));
			CHECK(world["position"][0].get<double>() == doctest::Approx(0.0).epsilon(1e-4));
			if (reference)
				CHECK(samples == *reference); // Bit for bit
			else
				reference = samples;
			// The game runs again (it ran before), and the sequence's holds are gone.
			CHECK_FALSE(harness.Context.IsPaused());
			CHECK(harness.Run("input.state")["holds"].empty());
			harness.Run("play.stop");
		}
	}

	TEST_CASE("A command event that fails ends its sequence with the game running as before; misspelled parameters are refused up front")
	{
		InputHarness harness;
		harness.Run("play.start");
		harness.Frame();
		REQUIRE_FALSE(harness.Context.IsPaused());

		// A parameter the command does not declare is refused before anything runs, as the command would refuse it.
		const EditorCommandResult misspelled = harness.Fail("input.sequence", { { "events", {
			{ { "step", 0 }, { "key", "W" }, { "action", "press" } },
			{ { "step", 0 }, { "command", "input.key" }, { "parameters", { { "kee", "W" } } } } } }, { "steps", 5 } });
		CHECK(misspelled.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(misspelled.Error.find("Unknown parameter 'kee' for input.key") != std::string::npos);
		CHECK_FALSE(harness.Context.IsPaused());
		CHECK(harness.Run("input.state")["holds"].empty());

		// A value only the command checks fails the sequence where it runs: in its first step, or later. The sequence's holds
		// go, and the game, which ran before, runs again.
		const EditorCommandResult first = harness.Fail("input.sequence", { { "events", {
			{ { "step", 0 }, { "key", "W" }, { "action", "press" } },
			{ { "step", 0 }, { "command", "input.key" }, { "parameters", { { "key", "Nope" } } } } } }, { "steps", 5 } });
		CHECK(first.Error.find("Unknown key 'Nope'") != std::string::npos);
		CHECK_FALSE(harness.Context.IsPaused());
		CHECK(harness.Run("input.state")["holds"].empty());
		std::optional<EditorCommandResult> sequence;
		harness.Start("input.sequence", { { "events", {
			{ { "step", 0 }, { "key", "W" }, { "action", "press" } },
			{ { "step", 2 }, { "command", "input.key" }, { "parameters", { { "key", "Nope" } } } } } }, { "steps", 5 } }, sequence);
		for (int frame = 0; frame < 6 && !sequence; frame++)
			harness.Frame();
		REQUIRE(sequence);
		CHECK_FALSE(sequence->Success);
		CHECK(sequence->Error.find("Unknown key 'Nope'") != std::string::npos);
		CHECK_FALSE(harness.Context.IsPaused());
		CHECK(harness.Run("input.state")["holds"].empty());

		// A command without a wait parameter (input.state) runs as an event too.
		harness.Start("input.sequence", { { "events", { { { "step", 0 }, { "command", "input.state" } } } }, { "steps", 2 } }, sequence);
		for (int frame = 0; frame < 4 && !sequence; frame++)
			harness.Frame();
		REQUIRE(sequence);
		CHECK_MESSAGE(sequence->Success, sequence->Error);
	}

	TEST_CASE("An input sequence keeps a paused game paused, refuses events beyond its steps and play.step while it runs")
	{
		InputHarness harness;
		const std::string hero = harness.AddWalker();
		harness.Run("play.start");
		harness.Frame();
		harness.Run("play.pause", { { "paused", true } });

		// Events past the last step, unknown keys and two kinds in one event are refused before anything runs.
		CHECK(harness.Fail("input.sequence", { { "events", { { { "step", 5 }, { "key", "W" } } } }, { "steps", 5 } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.sequence", { { "events", { { { "step", 0 }, { "key", "Nope" } } } }, { "steps", 5 } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.sequence", { { "events", { { { "step", 0 }, { "key", "W" }, { "scroll", { 0, 1 } } } } }, { "steps", 5 } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.sequence", { { "events", nlohmann::json::array() }, { "steps", 5 }, { "observe", { { { "entity", hero } } } } }).ErrorKind
			== EditorCommandError::InvalidParameters);
		CHECK_FALSE(harness.Context.IsInputSequenceRunning());

		// A press kept held, and a command event: the sequence's press outlives it with keepHeld.
		std::optional<EditorCommandResult> sequence;
		harness.Start("input.sequence", { { "events", {
			{ { "step", 0 }, { "key", "W" }, { "action", "press" } },
			{ { "step", 1 }, { "command", "input.mouseMove" }, { "parameters", { { "position", { 10, 20 } } } } } } },
			{ "steps", 4 }, { "keepHeld", true } }, sequence);
		CHECK(harness.Context.IsInputSequenceRunning());
		const EditorCommandResult stepping = harness.Fail("play.step", { { "steps", 1 } });
		CHECK(stepping.Error.find("input sequence is running") != std::string::npos);
		CHECK(harness.Fail("input.sequence", { { "events", nlohmann::json::array() }, { "steps", 1 } }).Error.find("input sequence is running")
			!= std::string::npos);
		for (int frame = 0; frame < 6 && !sequence; frame++)
			harness.Frame();
		REQUIRE(sequence);
		REQUIRE_MESSAGE(sequence->Success, sequence->Error);
		CHECK(harness.Context.IsPaused()); // It was paused before
		CHECK(harness.ProbeVector("MousePosition") == glm::vec2(10.0f, 20.0f));
		CHECK(harness.Run("input.state")["keys"] == nlohmann::json { "W" });

		// Without keepHeld its holds are released at the end; stopping play fails a running sequence with its steps.
		harness.Run("input.releaseAll");
		harness.Start("input.sequence", { { "events", { { { "step", 0 }, { "key", "W" }, { "action", "press" } } } }, { "steps", 3 } }, sequence);
		for (int frame = 0; frame < 5 && !sequence; frame++)
			harness.Frame();
		REQUIRE(sequence);
		CHECK(sequence->Success);
		CHECK(harness.Run("input.state")["keys"].empty());
		harness.Start("input.sequence", { { "events", nlohmann::json::array() }, { "steps", 50 } }, sequence);
		harness.Frame();
		harness.Frame();
		harness.Run("play.stop");
		for (int frame = 0; frame < 3 && !sequence; frame++)
			harness.Frame();
		REQUIRE(sequence);
		CHECK_FALSE(sequence->Success);
		CHECK(sequence->ErrorData["stepsRun"] == 2);
	}

	TEST_CASE("input.action taps, presses and releases an action for the given steps")
	{
		InputHarness harness;
		harness.AddActions();
		CHECK(harness.Fail("input.action", { { "action", "Jump" } }).ErrorKind == EditorCommandError::Failed); // No game runs
		harness.Run("play.start");
		harness.Frame();

		// Refused, changing nothing: unknown and ambiguous names, values the kind cannot take, durations of other modes.
		const auto refuse = [&harness](const nlohmann::json& parameters)
		{
			INFO(parameters.dump());
			const EditorCommandResult result = harness.Fail("input.action", parameters);
			CHECK(result.ErrorKind == EditorCommandError::InvalidParameters);
			return result.Error;
		};
		CHECK(refuse({ { "action", "Fly" } }).find("There is no action 'Fly'") != std::string::npos);
		CHECK(refuse({ { "action", "Left" } }).find("Gameplay/Left, UI/Left") != std::string::npos);
		CHECK(refuse({ { "action", "Jump" }, { "value", { 1, 0 } } }).find("Button action takes no value") != std::string::npos);
		CHECK(refuse({ { "action", "Steer" }, { "value", { 0.5, 0.5 } } }).find("[x, 0]") != std::string::npos);
		CHECK(refuse({ { "action", "Steer" }, { "value", { 2, 0 } } }).find("-1 to 1") != std::string::npos);
		CHECK(refuse({ { "action", "Move" } }).find("needs a value") != std::string::npos);
		CHECK(refuse({ { "action", "Move" }, { "value", { 1, 1 } } }).find("length of at most 1") != std::string::npos);
		CHECK(refuse({ { "action", "Move" }, { "mode", "release" }, { "value", { 0, 1 } } }).find("release takes no value") != std::string::npos);
		CHECK(refuse({ { "action", "Jump" }, { "mode", "press" }, { "steps", 2 } }).find("mode tap") != std::string::npos);
		CHECK(refuse({ { "action", "Jump" }, { "mode", "hold" } }).find("'mode'") != std::string::npos);
		CHECK(refuse(nlohmann::json::object()).find("action") != std::string::npos);
		harness.Frame();
		CHECK_FALSE(InputActions::IsDown("Jump"));
		CHECK(harness.Run("input.state")["simulatedActions"].empty());

		// A tap of two steps, named in any case: down from the next frame for two steps, answered once the game saw the release.
		std::optional<EditorCommandResult> result;
		harness.Start("input.action", { { "action", "gameplay/jump" }, { "steps", 2 } }, result);
		harness.Frame();
		CHECK(InputActions::IsPressed("Jump"));
		CHECK_FALSE(result);
		harness.Frame();
		CHECK(InputActions::IsDown("Jump"));
		CHECK_FALSE(InputActions::IsPressed("Jump"));
		CHECK_FALSE(result);
		harness.Frame();
		CHECK_FALSE(InputActions::IsDown("Jump"));
		CHECK(InputActions::IsReleased("Jump"));
		REQUIRE(result);
		REQUIRE(result->Success);
		CHECK(result->Value["action"] == "Gameplay/Jump");
		CHECK(result->Value["kind"] == "Button");
		CHECK(result->Value["mode"] == "tap");
		CHECK(result->Value["steps"] == 2);
		CHECK(result->Value["interrupted"] == false);
		CHECK(result->Value["held"]["simulatedActions"].empty());

		// A press holds a value until released; a new press of the action replaces it without letting the action go.
		harness.Start("input.action", { { "action", "Move" }, { "mode", "press" }, { "value", { 0, 1 } } }, result);
		harness.Frame();
		REQUIRE(result);
		CHECK(result->Value["seen"] == true);
		CHECK(InputActions::IsPressed("Move"));
		CHECK(InputActions::GetValue("Move") == glm::vec2(0.0f, 1.0f));
		harness.Run("input.action", { { "action", "Move" }, { "mode", "press" }, { "value", { -0.5, 0 } }, { "wait", false } });
		harness.Frame();
		CHECK(InputActions::IsDown("Move"));
		CHECK_FALSE(InputActions::IsPressed("Move"));
		CHECK(InputActions::GetValue("Move") == glm::vec2(-0.5f, 0.0f));
		const nlohmann::json held = harness.Run("input.state")["simulatedActions"];
		REQUIRE(held.size() == 1);
		CHECK(held[0]["action"] == "Gameplay/Move");
		// An Axis is held at 1 unless a value is given.
		harness.Run("input.action", { { "action", "Steer" }, { "mode", "press" }, { "wait", false } });
		harness.Frame();
		CHECK(InputActions::GetValue("Steer") == glm::vec2(1.0f, 0.0f));
		const nlohmann::json released = harness.Run("input.action", { { "action", "Move" }, { "mode", "release" }, { "wait", false } });
		CHECK(released["released"] == true);
		harness.Frame();
		CHECK(InputActions::IsReleased("Move"));
		CHECK(InputActions::GetValue("Move") == glm::vec2(0.0f));
		// input.releaseAll lets go of held actions too.
		CHECK(harness.Run("input.releaseAll", { { "wait", false } })["released"]["simulatedActions"].size() == 1);
		harness.Frame();
		CHECK(InputActions::IsReleased("Steer"));

		// A tap ended early by a release answers interrupted.
		harness.Start("input.action", { { "action", "Jump" }, { "steps", 10 } }, result);
		harness.Frame();
		harness.Run("input.action", { { "action", "Jump" }, { "mode", "release" }, { "wait", false } });
		harness.Frame();
		harness.Frame();
		REQUIRE(result);
		CHECK(result->Value["interrupted"] == true);
		CHECK_FALSE(InputActions::IsDown("Jump"));

		// A held action's map disabled: the game reads it as up. Play stopping drops the hold.
		harness.Run("input.action", { { "action", "Horn" }, { "mode", "press" }, { "wait", false } });
		harness.Frame();
		CHECK_FALSE(InputActions::IsDown("Horn"));
		CHECK(InputActions::SetMapEnabled("Vehicle", true));
		harness.Frame();
		CHECK(InputActions::IsPressed("Horn"));
		harness.Run("play.stop");
		harness.Run("play.start");
		harness.Frame();
		harness.Frame();
		CHECK_FALSE(InputActions::IsDown("Horn"));
		CHECK(harness.Run("input.state")["simulatedActions"].empty());
	}

	TEST_CASE("input.sequence runs action steps")
	{
		InputHarness harness;
		harness.AddActions();
		harness.Run("play.start");
		harness.Frame();
		// Checked before anything runs; a button event's "action" is still its tap, press or release.
		EditorCommandResult refused = harness.Fail("input.sequence", { { "steps", 2 }, { "events", { { { "step", 0 }, { "action", "Fly" } } } } });
		CHECK(refused.ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(refused.Error.find("events[0]: 'action': There is no action 'Fly'") != std::string::npos);
		refused = harness.Fail("input.sequence", { { "steps", 2 }, { "events", { { { "step", 0 }, { "action", "Move" }, { "mode", "press" } } } } });
		CHECK(refused.Error.find("events[0]: 'value': a Vector2 action needs a value") != std::string::npos);
		refused = harness.Fail("input.sequence", { { "steps", 2 }, { "events", { { { "step", 0 }, { "action", "Jump" }, { "key", "W" } } } } });
		CHECK(refused.Error.find("must be press, release or tap, not 'Jump'") != std::string::npos);

		// Move held up from step 0 to 3, Jump tapped for two steps from step 1, W tapped at step 1: one step per frame.
		std::optional<EditorCommandResult> result;
		harness.Start("input.sequence", { { "steps", 4 }, { "events", {
			{ { "step", 0 }, { "action", "Move" }, { "mode", "press" }, { "value", { 0, 1 } } },
			{ { "step", 1 }, { "action", "Jump" }, { "steps", 2 } },
			{ { "step", 1 }, { "key", "W" }, { "action", "tap" } },
			{ { "step", 3 }, { "action", "Gameplay/Move" }, { "mode", "release" } } } } }, result);
		harness.Frame();
		CHECK(InputActions::IsPressed("Move"));
		CHECK(InputActions::GetValue("Move") == glm::vec2(0.0f, 1.0f));
		CHECK_FALSE(InputActions::IsDown("Jump"));
		harness.Frame();
		CHECK(InputActions::IsPressed("Jump"));
		CHECK(harness.ProbeField("WPressed"));
		harness.Frame();
		CHECK(InputActions::IsDown("Jump"));
		CHECK_FALSE(InputActions::IsPressed("Jump"));
		CHECK(InputActions::IsDown("Move"));
		harness.Frame();
		CHECK(InputActions::IsReleased("Jump"));
		CHECK(InputActions::IsReleased("Move"));
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
		while (!result && std::chrono::steady_clock::now() < deadline)
			harness.Frame();
		REQUIRE(result);
		INFO(result->Error);
		REQUIRE(result->Success);
		CHECK(result->Value["stepsRun"] == 4);
		CHECK(result->Value["seen"].size() == 4);
		CHECK(result->Value["interrupted"] == false);
		CHECK(harness.Run("input.state")["simulatedActions"].empty());
	}

	TEST_CASE("input.state reports actions, maps and player bindings")
	{
		InputHarness harness;
		harness.AddActions();
		// Also in Edit mode: the project's actions, in the set's order, as the game would read them.
		nlohmann::json state = harness.Run("input.state");
		CHECK(state["actions"].size() == 11);
		CHECK(state["actions"][0]["action"] == "UI/Submit");
		CHECK(harness.FindActionState("Gameplay/Jump") == nlohmann::json { { "action", "Gameplay/Jump" }, { "map", "Gameplay" }, { "kind", "Button" },
			{ "down", false }, { "value", { 0.0f, 0.0f } } });
		CHECK(state["maps"] == nlohmann::json::array({ { { "map", "UI" }, { "enabled", true } }, { { "map", "Gameplay" }, { "enabled", true } },
			{ { "map", "Vehicle" }, { "enabled", false } } }));
		CHECK(state["playerBindings"] == nlohmann::json::object());
		CHECK(state["capture"] == false);

		harness.Run("play.start");
		harness.Frame();
		harness.Run("input.action", { { "action", "Move" }, { "mode", "press" }, { "value", { 0.6, 0.8 } }, { "wait", false } });
		harness.Frame();
		const nlohmann::json move = harness.FindActionState("Gameplay/Move");
		CHECK(move["down"] == true);
		CHECK(move["kind"] == "Vector2");
		CHECK(move["value"][0].get<float>() == doctest::Approx(0.6f));
		CHECK(move["value"][1].get<float>() == doctest::Approx(0.8f));
		state = harness.Run("input.state");
		REQUIRE(state["holds"].size() == 1);
		CHECK(state["holds"][0]["type"] == "action");
		CHECK(state["holds"][0]["name"] == "Gameplay/Move");
		CHECK(state["holds"][0]["lastStep"].is_null());

		// A script enables the Vehicle map and rebinds Jump; a binding capture listens.
		CHECK(InputActions::SetMapEnabled("Vehicle", true));
		REQUIRE(InputActions::SetBindings("Gameplay/Jump", std::vector<InputBinding> { *InputBinding::Parse("Key/K") }));
		REQUIRE(InputActions::BeginCapture({}));
		harness.Frame();
		state = harness.Run("input.state");
		CHECK(state["maps"][2] == nlohmann::json { { "map", "Vehicle" }, { "enabled", true } });
		CHECK(state["playerBindings"] == nlohmann::json { { "Gameplay/Jump", { "Key/K" } } });
		CHECK(state["capture"] == true);
		CHECK(harness.FindActionState("Gameplay/Move")["down"] == false); // Every action reads as up while a capture listens
		InputActions::CancelCapture();
		harness.Run("play.stop");
	}
}
