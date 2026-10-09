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

		void Frame()
		{
			Input::BeginFrame();
			Context.Update(Timestep(1.0f / 60.0f));
			Runner.Update(Context);
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
		CHECK(result->Value["frames"] == 2);
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
}
