#include <doctest/doctest.h>

#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "Scripting/ScriptTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Input/Input.h>
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
	// frames run the way the application runs them: input frame, editor update, then the pending commands.
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
			Context.Update(Timestep(seconds));
			Runner.Update(Context);
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
}
