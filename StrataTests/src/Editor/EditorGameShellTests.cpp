#include <doctest/doctest.h>

#include "Editor/CommandTestUtils.h"
#include "Editor/EditorCommandRunner.h"
#include "Editor/EditorCommands.h"
#include "Editor/EditorContext.h"
#include "FeatureTest/FeatureTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "TestHelpers.h"

#include <Strata/Core/EngineServices.h>
#include <Strata/Core/FileSystem.h>
#include <Strata/Core/Log.h>
#include <Strata/Core/Platform.h>
#include <Strata/Core/SaveFiles.h>
#include <Strata/Core/Timestep.h>
#include <Strata/Input/Input.h>
#include <Strata/Input/InputWindow.h>
#include <Strata/Reflection/PropertyJson.h>
#include <Strata/Runtime/GameRuntime.h>
#include <Strata/Scripting/ScriptSystem.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// An editor with a project that runs the API test module, frames run the way the application runs them: input frame,
	// editor update, then the pending commands.
	struct ShellHarness
	{
		EditorContext Context { EditorContextSpecification { false, false } };
		EditorCommandRegistry Commands;
		EditorCommandRunner Runner;
		std::filesystem::path Directory;

		explicit ShellHarness(const std::string& name = "Shell Game")
			: Directory(CreateTemporaryDirectory("EditorGameShell") / name)
		{
			Input::Reset();
			Run("project.create", { { "directory", FileSystem::ToUTF8(Directory) }, { "name", name } });
			Run("script.load", { { "path", FileSystem::ToUTF8(GetTestScriptModule(STRATA_TEST_SCRIPTS_API)) } });
		}

		~ShellHarness()
		{
			Context.Stop();
			Input::Reset();
		}

		nlohmann::json Run(std::string_view name, const nlohmann::json& parameters = nlohmann::json::object())
		{
			const EditorCommandResult result = Tests::CompleteCommand(Context, Commands.Execute(Context, name, parameters));
			INFO(std::string(name), ": ", result.Error);
			REQUIRE(result.Success);
			return result.Value;
		}

		EditorCommandResult Fail(std::string_view name, const nlohmann::json& parameters)
		{
			EditorCommandResult result = Commands.Execute(Context, name, parameters);
			REQUIRE_FALSE(result.IsPending());
			CHECK_FALSE(result.Success);
			return result;
		}

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

		std::string AddScript(const std::string& entityName, const std::string& className, const nlohmann::json& fields = nlohmann::json::object())
		{
			const std::string id = Run("entity.create", { { "name", entityName } })["id"].get<std::string>();
			Run("script.add", { { "entity", id }, { "class", className }, { "fields", fields } });
			return id;
		}

		template<typename T>
		T Field(const std::string& id, std::string_view className, std::string_view field)
		{
			Ref<Scene> scene = Context.GetActiveScene();
			Entity entity = scene->GetEntityByUUID(*UUIDFromJson(id));
			return GetField<T>(GetScriptSystem(*scene), entity, className, field);
		}
	};

	// A window as Input sees it, Input's window while it lives.
	struct CursorWindow final : public InputWindow
	{
		CursorWindow() { Input::SetWindow(this); }
		~CursorWindow() override { Input::SetWindow(nullptr); }
		CursorWindow(const CursorWindow&) = delete;
		CursorWindow& operator=(const CursorWindow&) = delete;

		uint32_t GetWidth() const override { return 800; }
		uint32_t GetHeight() const override { return 600; }
		void SetCursorMode(CursorMode mode) override { Cursor = mode; }

		CursorMode Cursor = CursorMode::Normal;
	};

}

TEST_SUITE("Editor.Input")
{
	TEST_CASE("Simulated gamepad buttons and axes reach the game's scripts")
	{
		ShellHarness harness;
		const std::string probe = harness.AddScript("Pad", "GamepadProbe", { { "Gamepad", 2 } });
		CHECK(harness.Fail("input.gamepadButton", { { "button", "A" } }).ErrorKind == EditorCommandError::Failed); // Not playing
		harness.Run("play.start");
		harness.Frame();
		CHECK_FALSE(harness.Field<bool>(probe, "GamepadProbe", "Connected"));

		// Requests the commands refuse change nothing.
		CHECK(harness.Fail("input.gamepadButton", { { "button", "Triangle" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.gamepadButton", { { "button", "A" }, { "gamepad", 4 } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.gamepadAxis", { { "axis", "LeftZ" }, { "value", 1 } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.gamepadAxis", { { "axis", "LeftX" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.gamepadAxis", { { "axis", "LeftX" }, { "value", "full" } }).ErrorKind == EditorCommandError::InvalidParameters);
		CHECK(harness.Fail("input.gamepadAxis", { { "axis", "LeftX" }, { "value", nullptr }, { "frames", 2 } }).ErrorKind == EditorCommandError::InvalidParameters);
		harness.Frame();
		CHECK_FALSE(harness.Field<bool>(probe, "GamepadProbe", "Connected"));

		// A press and a held axis: the game sees them in its next frame.
		std::optional<EditorCommandResult> press;
		harness.Start("input.gamepadButton", { { "gamepad", 2 }, { "button", "a" }, { "action", "press" } }, press);
		std::optional<EditorCommandResult> axis;
		harness.Start("input.gamepadAxis", { { "gamepad", 2 }, { "axis", "LeftY" }, { "value", 0.75 } }, axis);
		harness.Frame();
		REQUIRE(press);
		REQUIRE(axis);
		CHECK(press->Success);
		CHECK(press->Value["seen"] == true);
		CHECK(axis->Value["value"].get<double>() == doctest::Approx(0.75));
		CHECK(harness.Field<bool>(probe, "GamepadProbe", "Connected"));
		CHECK(harness.Field<bool>(probe, "GamepadProbe", "APressed"));
		CHECK(harness.Field<float>(probe, "GamepadProbe", "LeftY") == doctest::Approx(0.75f));

		nlohmann::json state = harness.Run("input.state");
		CHECK(state["gamepads"] == nlohmann::json::array({ 2 }));
		REQUIRE(state["gamepadButtons"].size() == 1);
		CHECK(state["gamepadButtons"][0]["gamepad"] == 2);
		CHECK(state["gamepadButtons"][0]["button"] == "A");
		REQUIRE(state["gamepadAxes"].size() == 1);
		CHECK(state["gamepadAxes"][0]["axis"] == "LeftY");
		CHECK(state["gamepadAxes"][0]["value"].get<double>() == doctest::Approx(0.75));
		CHECK(harness.Context.GetSimulatedInput().DescribeHolds() == "Gamepad 3 A, Gamepad 3 LeftY 0.75");

		// A trigger beyond its range is clamped; a tap of an axis returns it to rest after its frames.
		std::optional<EditorCommandResult> trigger;
		harness.Start("input.gamepadAxis", { { "gamepad", 2 }, { "axis", "RightTrigger" }, { "value", -3 }, { "frames", 2 } }, trigger);
		harness.Frame();
		CHECK(harness.Field<float>(probe, "GamepadProbe", "RightTrigger") == 0.0f);
		CHECK_FALSE(trigger);
		harness.Run("input.gamepadAxis", { { "gamepad", 2 }, { "axis", "RightTrigger" }, { "value", 0.5 }, { "frames", 2 }, { "wait", false } });
		harness.Frame();
		CHECK(harness.Field<float>(probe, "GamepadProbe", "RightTrigger") == doctest::Approx(0.5f));
		REQUIRE(trigger); // Replaced: the earlier tap ended, interrupted, when the game saw the new value
		CHECK(trigger->Value["interrupted"] == true);
		harness.Frame();
		harness.Frame();
		CHECK(harness.Field<float>(probe, "GamepadProbe", "RightTrigger") == 0.0f);

		// A tap of a button ends with its release, then a null value gives the axis back.
		std::optional<EditorCommandResult> tap;
		harness.Start("input.gamepadButton", { { "gamepad", 2 }, { "button", "Start" }, { "frames", 2 } }, tap);
		harness.Frame();
		CHECK(harness.Field<bool>(probe, "GamepadProbe", "StartDown"));
		harness.Frame();
		harness.Frame();
		REQUIRE(tap);
		CHECK(tap->Value["seen"] == true);
		CHECK(tap->Value["interrupted"] == false);
		CHECK_FALSE(harness.Field<bool>(probe, "GamepadProbe", "StartDown"));
		const nlohmann::json released = harness.Run("input.gamepadAxis", { { "gamepad", 2 }, { "axis", "LeftY" }, { "value", nullptr }, { "wait", false } });
		CHECK(released["released"] == true);
		harness.Frame();
		CHECK(harness.Field<float>(probe, "GamepadProbe", "LeftY") == 0.0f);
		CHECK(harness.Field<bool>(probe, "GamepadProbe", "ADown"));

		// input.releaseAll lets go of the gamepad too.
		const nlohmann::json all = harness.Run("input.releaseAll", { { "wait", false } });
		CHECK(all["released"]["gamepadButtons"].size() == 1);
		harness.Frame();
		CHECK(harness.Field<bool>(probe, "GamepadProbe", "AReleased"));
		CHECK_FALSE(harness.Context.GetSimulatedInput().HasHolds());

		// Stopping play forgets the simulated gamepad.
		harness.Run("play.stop");
		CHECK_FALSE(Input::IsSimulatedGamepadConnected(2));
	}
}

TEST_SUITE("Scripting.Input")
{
	TEST_CASE("Stopping play gives the cursor a game locked back")
	{
		ShellHarness harness;
		CursorWindow window;
		harness.AddScript("Cursor", "CursorProbe");
		harness.Run("play.start");
		CHECK(Input::GetGameCursorMode() == CursorMode::Locked);
		CHECK(window.Cursor == CursorMode::Locked);
		CHECK(harness.Run("editor.status", { { "sections", { "game" } } })["game"]["cursor"] == "locked");

		harness.Run("play.stop");
		CHECK(Input::GetGameCursorMode() == CursorMode::Normal);
		CHECK(window.Cursor == CursorMode::Normal);
		CHECK(harness.Run("editor.status", { { "sections", { "game" } } })["game"]["cursor"] == "normal");
	}
}

TEST_SUITE("Scripting.Game")
{
	TEST_CASE("Window requests in the editor's play mode are reported, not applied, and forgotten when play stops")
	{
		ShellHarness harness;
		const std::string requests = harness.AddScript("Window", "WindowRequests", { { "Mode", 2 }, { "Width", 1600 }, { "Height", 900 } });
		CHECK(harness.Run("editor.status", { { "sections", { "game" } } })["game"]["windowRequests"].is_null());
		Input::SetViewport({ 0.0f, 0.0f }, { 512.0f, 288.0f }); // The game view
		harness.Run("play.start");
		harness.Frame();

		// The game sees the game view as its window; the requests are recorded for people and tools.
		CHECK(harness.Field<bool>(requests, "WindowRequests", "Accepted"));
		CHECK(harness.Field<glm::vec2>(requests, "WindowRequests", "Size") == glm::vec2(512.0f, 288.0f));
		const EditorGameWindow& window = harness.Context.GetGameWindow();
		CHECK(window.HasRequests());
		const nlohmann::json reported = harness.Run("editor.status", { { "sections", { "game" } } })["game"]["windowRequests"];
		CHECK(reported["mode"] == "borderless");
		CHECK(reported["resolution"] == nlohmann::json::array({ 1600, 900 }));
		CHECK(reported["vsync"] == false);
		CHECK(reported["applied"] == false);
		CHECK(window.DescribeForPeople() == "The game asked for a borderless window at 1600 x 900 pixels with vsync off; play mode keeps the editor's window.");
		const GameWindowState state = EngineServices::Get<IGameWindow>()->GetState();
		CHECK(state.Mode == GameWindowMode::Borderless);
		CHECK(state.Width == 512);

		harness.Run("play.stop");
		CHECK_FALSE(window.HasRequests());
		CHECK(harness.Run("editor.status", { { "sections", { "game" } } })["game"]["windowRequests"].is_null());
		Input::SetViewport({ 0.0f, 0.0f }, { 0.0f, 0.0f });
	}

	TEST_CASE("The game's window is the Game view's image in pixels, else the input viewport")
	{
		Input::Reset();
		EditorGameWindow window;
		window.SetContentScale(2.0f);
		Input::SetViewport({ 0.0f, 0.0f }, { 512.0f, 288.0f }); // Points
		CHECK(window.GetState().Width == 512);
		Input::SetViewport({ 0.0f, 0.0f }, { 512.0f, 288.0f }, { 1024.0f, 576.0f }); // The Game view's image in pixels
		const GameWindowState state = window.GetState();
		CHECK(state.Width == 1024);
		CHECK(state.Height == 576);
		CHECK(state.ContentScale == 2.0f);
		Input::SetViewport({ 0.0f, 0.0f }, { 512.0f, 288.0f });
		CHECK(window.GetState().Height == 288);
		Input::Reset();
	}

	TEST_CASE("A game that asks for the same display options every frame is reported once")
	{
		ShellHarness harness;
		harness.AddScript("Options", "WindowRequests", { { "Mode", 0 }, { "Width", 1280 }, { "Height", 720 }, { "VSync", true }, { "EveryFrame", true } });
		const ScopedLogLevel infoLog(LogLevel::Info);
		LogCapture log;
		harness.Run("play.start");
		harness.Frame();
		const EditorGameWindow& window = harness.Context.GetGameWindow();
		REQUIRE(window.HasRequests());
		const uint64_t version = window.GetVersion();
		for (int frame = 0; frame < 30; frame++)
			harness.Frame();
		CHECK(window.GetVersion() == version);
		size_t said = 0;
		for (const LogEntry& entry : log.GetEntries())
			said += entry.Message.find("The game asked for") != std::string::npos ? 1 : 0;
		CHECK(said == 3); // The mode, the resolution and vsync, once each
		// Each mode reads as words in the sentence for people.
		CHECK(window.DescribeForPeople() == "The game asked for a window at 1280 x 720 pixels with vsync on; play mode keeps the editor's window.");
		harness.Run("play.stop");
	}

	TEST_CASE("Saves of the editor's play mode go to the project's .strata/Saves")
	{
		ShellHarness harness;
		const std::filesystem::path saves = harness.Directory / ".strata" / "Saves";
		REQUIRE(harness.Context.GetGameSaveDirectory());
		CHECK(*harness.Context.GetGameSaveDirectory() == saves);
		CHECK(harness.Run("editor.status", { { "sections", { "game" } } })["game"]["saveDirectory"] == FileSystem::ToUTF8(saves));

		const std::string saver = harness.AddScript("Saver", "SaveRoundTrip");
		harness.Run("play.start");
		harness.Frame();
		CHECK(harness.Field<int32_t>(saver, "SaveRoundTrip", "Checks") >= 22);
		CHECK(harness.Field<std::string>(saver, "SaveRoundTrip", "Failure").empty());
		harness.Run("play.stop");
		CHECK(SaveFiles::List(saves) == std::vector<std::string> { "Kept" });
		CHECK(FileSystem::ReadText(saves / "Kept.save") == std::optional<std::string>("level=3;score=1200"));

		// Without a project there is nowhere to save. (The scene's unsaved script is of no interest here: discarded.)
		harness.Run("project.close", { { "force", true } });
		CHECK_FALSE(harness.Context.GetGameSaveDirectory());
		CHECK(harness.Run("editor.status", { { "sections", { "game" } } })["game"]["saveDirectory"].is_null());
	}

	TEST_CASE("An exported game saves in the player's data directory, or where the program running it says")
	{
		ShellHarness harness("Strata Save Test Game");
		harness.AddScript("Saver", "SaveRoundTrip");
		harness.Run("scene.saveAs", { { "path", "Scenes/Main.stscene" } });
		harness.Run("project.setStartScene", { { "scene", "Scenes/Main.stscene" } });
		const std::filesystem::path build = harness.Directory.parent_path() / "Build";
		const nlohmann::json exported = RunCommandToCompletion(harness.Context, harness.Commands, "project.export", { { "directory", FileSystem::ToUTF8(build) }, { "includeRuntime", false } });
		const std::filesystem::path manifest = FileSystem::FromUTF8(exported["manifest"].get<std::string>());

		{
			// The player's: <user data>/<game>/Saves. Only the path is checked: the test does not write into the user's data.
			std::string error;
			Scope<GameRuntime> game = GameRuntime::Create(manifest, &error);
			REQUIRE_MESSAGE(game, error);
			const std::optional<std::filesystem::path> directory = game->GetSaveDirectory();
			REQUIRE(directory);
			const std::filesystem::path expected = Platform::GetUserDataDirectory("Strata Save Test Game") / "Saves";
			CHECK(*directory == expected);
			std::error_code ignored;
			std::filesystem::remove(expected.parent_path(), ignored); // Created by the lookup; empty
		}

		// Where the program running the game says: the script's saves land there while the game runs, and the editor's
		// directory is the provider again once it ends.
		const std::filesystem::path elsewhere = harness.Directory.parent_path() / "PlayerSaves";
		{
			GameRuntimeOptions options;
			options.SaveFilesDirectory = elsewhere;
			std::string error;
			Scope<GameRuntime> game = GameRuntime::Create(manifest, &error, options);
			REQUIRE_MESSAGE(game, error);
			CHECK(game->GetSaveDirectory() == std::optional<std::filesystem::path>(elsewhere));
			game->Update(Timestep(1.0f / 60.0f));
			ScriptSystem& system = GetScriptSystem(*game->GetScene());
			const Entity entity = game->GetScene()->FindEntityByName("Saver");
			CHECK(GetField<int32_t>(system, entity, "SaveRoundTrip", "Checks") >= 22);
			CHECK(GetField<std::string>(system, entity, "SaveRoundTrip", "Failure").empty());
		}
		CHECK(SaveFiles::List(elsewhere) == std::vector<std::string> { "Kept" });
		CHECK(SaveFiles::List(harness.Directory / ".strata" / "Saves").empty());
		REQUIRE(EngineServices::Get<IGameSaves>());
		CHECK(EngineServices::Get<IGameSaves>()->GetSaveDirectory() == harness.Context.GetGameSaveDirectory());
	}
}
