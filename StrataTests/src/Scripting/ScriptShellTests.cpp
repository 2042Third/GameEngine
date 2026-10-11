#include <doctest/doctest.h>

#include "FeatureTest/FeatureTestUtils.h"
#include "Scripting/ScriptTestUtils.h"
#include "Strata/Core/EngineServices.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/SaveFiles.h"
#include "Strata/Input/Input.h"
#include "Strata/Input/InputWindow.h"
#include "Strata/Runtime/GameWindowService.h"
#include "TestHelpers.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace Strata;
using namespace Strata::Tests;

namespace
{

	// The program's window as the game sees it, recording what scripts ask of it.
	struct RecordingGameWindow final : public IGameWindow
	{
		std::vector<std::string> Requests;
		GameWindowState State { GameWindowMode::Windowed, 640, 360, true };

		bool SetWindowMode(GameWindowMode mode) override
		{
			Requests.push_back("mode " + std::to_string(static_cast<int>(mode)));
			return true;
		}

		bool SetResolution(uint32_t width, uint32_t height) override
		{
			Requests.push_back("resolution " + std::to_string(width) + "x" + std::to_string(height));
			return true;
		}

		bool SetVSync(bool enabled) override
		{
			Requests.push_back(enabled ? "vsync on" : "vsync off");
			return true;
		}

		GameWindowState GetState() override { return State; }
	};

	bool Logged(const std::vector<LogEntry>& entries, std::string_view text)
	{
		return std::any_of(entries.begin(), entries.end(), [&](const LogEntry& entry) { return entry.Message.find(text) != std::string::npos; });
	}

	// A window as Input sees it (only the cursor mode matters here), Input's window while it lives.
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

TEST_SUITE("Scripting.Input")
{
	TEST_CASE("Scripts read gamepads from devices and from simulated input")
	{
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity device = scene.CreateEntity("Device");
		AddScriptEntry(device, "GamepadProbe");
		Entity simulated = scene.CreateEntity("Simulated");
		AddFieldOverride(AddScriptEntry(simulated, "GamepadProbe"), "Gamepad", PropertyType::Int, PropertyValue(int32_t(1)));
		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);

		Input::Reset();
		Input::BeginFrame();
		float axes[c_GamepadAxisCount] = { -0.5f, 0.25f, 0.0f, 0.0f, 0.0f, 1.0f };
		bool buttons[c_GamepadButtonCount] = {};
		buttons[static_cast<size_t>(GamepadButton::Start)] = true;
		Input::ProcessGamepad(0, true, axes, buttons);
		Input::SimulateGamepadButton(1, GamepadButton::A, true);
		Input::SimulateGamepadAxis(1, GamepadAxis::LeftX, 0.1f); // Within the default dead zone
		Input::SimulateGamepadAxis(1, GamepadAxis::LeftY, 0.6f);
		Input::SimulateGamepadAxis(1, GamepadAxis::RightTrigger, 0.5f);
		scene.OnUpdateRuntime(0.0f);
		// Simulated input waits for the next input frame; the device's state is there at once.
		CHECK(GetField<bool>(system, device, "GamepadProbe", "Connected"));
		CHECK(GetField<bool>(system, device, "GamepadProbe", "StartDown"));
		CHECK(GetField<float>(system, device, "GamepadProbe", "LeftX") == doctest::Approx(-0.5f));
		CHECK(GetField<float>(system, device, "GamepadProbe", "LeftXDeadZone") == doctest::Approx((-0.5f + 0.15f) / 0.85f));
		CHECK(GetField<float>(system, device, "GamepadProbe", "RightTrigger") == 1.0f);
		CHECK_FALSE(GetField<bool>(system, simulated, "GamepadProbe", "Connected"));

		Input::BeginFrame();
		scene.OnUpdateRuntime(0.0f);
		CHECK(GetField<bool>(system, simulated, "GamepadProbe", "Connected"));
		CHECK(GetField<bool>(system, simulated, "GamepadProbe", "ADown"));
		CHECK(GetField<bool>(system, simulated, "GamepadProbe", "APressed"));
		CHECK_FALSE(GetField<bool>(system, simulated, "GamepadProbe", "AReleased"));
		CHECK_FALSE(GetField<bool>(system, simulated, "GamepadProbe", "StartDown"));
		CHECK(GetField<float>(system, simulated, "GamepadProbe", "LeftX") == doctest::Approx(0.1f));
		CHECK(GetField<float>(system, simulated, "GamepadProbe", "LeftXDeadZone") == 0.0f);
		CHECK(GetField<float>(system, simulated, "GamepadProbe", "LeftY") == doctest::Approx(0.6f));
		CHECK(GetField<float>(system, simulated, "GamepadProbe", "RightTrigger") == doctest::Approx(0.5f));
		// The stick's round dead zone keeps its direction and rescales its length.
		const glm::vec2 stick = GetField<glm::vec2>(system, simulated, "GamepadProbe", "LeftStick");
		const float tilt = std::sqrt(0.1f * 0.1f + 0.6f * 0.6f);
		const float length = (tilt - 0.15f) / 0.85f;
		CHECK(stick.x == doctest::Approx(0.1f / tilt * length));
		CHECK(stick.y == doctest::Approx(0.6f / tilt * length));
		CHECK_FALSE(GetField<bool>(system, simulated, "GamepadProbe", "InvalidButtonDown"));
		CHECK_FALSE(GetField<bool>(system, simulated, "GamepadProbe", "InvalidGamepadConnected"));
		CHECK(GetField<float>(system, simulated, "GamepadProbe", "InvalidAxis") == 0.0f);

		Input::SimulateGamepadButton(1, GamepadButton::A, false);
		Input::ReleaseSimulatedGamepadAxis(1, GamepadAxis::LeftY);
		Input::BeginFrame();
		scene.OnUpdateRuntime(0.0f);
		CHECK_FALSE(GetField<bool>(system, simulated, "GamepadProbe", "ADown"));
		CHECK(GetField<bool>(system, simulated, "GamepadProbe", "AReleased"));
		CHECK(GetField<float>(system, simulated, "GamepadProbe", "LeftY") == 0.0f);

		// A game without the input (the editor's game view lost the focus) sees the device at rest, but simulated input still.
		Input::SetEnabled(false);
		Input::BeginFrame();
		scene.OnUpdateRuntime(0.0f);
		CHECK_FALSE(GetField<bool>(system, device, "GamepadProbe", "StartDown"));
		CHECK(GetField<float>(system, device, "GamepadProbe", "LeftX") == 0.0f);
		CHECK(GetField<float>(system, simulated, "GamepadProbe", "LeftX") == doctest::Approx(0.1f));

		scene.OnRuntimeStop();
		Input::Reset();
	}

	TEST_CASE("Scripts set the cursor mode, which applies while the game has the input")
	{
		CursorWindow window;
		Input::Reset();
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity entity = scene.CreateEntity("Cursor");
		AddScriptEntry(entity, "CursorProbe");
		LogCapture log;
		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);
		CheckScriptChecks(system, entity, "CursorProbe", 2);
		CHECK(GetField<int32_t>(system, entity, "CursorProbe", "Reported") == 2);
		CHECK(Input::GetGameCursorMode() == CursorMode::Locked);
		CHECK(window.Cursor == CursorMode::Locked);
		CHECK(Logged(log.GetEntries(), "7 is no cursor mode"));

		Input::SetEnabled(false);
		CHECK(window.Cursor == CursorMode::Normal);
		Input::SetEnabled(true);
		CHECK(window.Cursor == CursorMode::Locked);

		scene.OnRuntimeStop();
		Input::SetGameCursorMode(CursorMode::Normal);
		CHECK(window.Cursor == CursorMode::Normal);
		Input::Reset();
	}
}

TEST_SUITE("Scripting.Game")
{
	TEST_CASE("Scripts ask the program's window for a mode, a resolution and vsync")
	{
		RecordingGameWindow window;
		EngineServices::ScopedProvider<IGameWindow> provider(window);
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity entity = scene.CreateEntity("Window");
		ScriptEntry& entry = AddScriptEntry(entity, "WindowRequests");
		AddFieldOverride(entry, "Mode", PropertyType::Int, PropertyValue(int32_t(2)));
		AddFieldOverride(entry, "Width", PropertyType::Int, PropertyValue(int32_t(1920)));
		AddFieldOverride(entry, "Height", PropertyType::Int, PropertyValue(int32_t(1080)));
		scene.OnRuntimeStart();
		RunFrames(scene, 1);

		const ScriptSystem& system = GetScriptSystem(scene);
		CheckScriptChecks(system, entity, "WindowRequests", 5);
		CHECK(GetField<bool>(system, entity, "WindowRequests", "Accepted"));
		CHECK(GetField<glm::vec2>(system, entity, "WindowRequests", "Size") == glm::vec2(640.0f, 360.0f));
		// Refused requests never reach the window.
		CHECK(window.Requests == std::vector<std::string> { "mode 2", "resolution 1920x1080", "vsync off" });
		scene.OnRuntimeStop();
	}

	TEST_CASE("Without a window to change, window requests are refused and reported")
	{
		REQUIRE(EngineServices::Get<IGameWindow>() == nullptr);
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity entity = scene.CreateEntity("Window");
		AddScriptEntry(entity, "WindowRequests");
		LogCapture log;
		scene.OnRuntimeStart();
		RunFrames(scene, 1);
		const ScriptSystem& system = GetScriptSystem(scene);
		CheckScriptChecks(system, entity, "WindowRequests", 5);
		CHECK_FALSE(GetField<bool>(system, entity, "WindowRequests", "Accepted"));
		CHECK(GetField<glm::vec2>(system, entity, "WindowRequests", "Size") == glm::vec2(0.0f));
		CHECK(Logged(log.GetEntries(), "no window to change"));
		scene.OnRuntimeStop();
	}

	TEST_CASE("A headless game accepts window requests after the frame and reports them back")
	{
		GameWindowService service(nullptr);
		EngineServices::ScopedProvider<IGameWindow> provider(service);
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity entity = scene.CreateEntity("Window");
		AddScriptEntry(entity, "WindowRequests");
		scene.OnRuntimeStart();
		RunFrames(scene, 1);
		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, entity, "WindowRequests", "Accepted"));
		// Requests apply after the frame (ApplyRequests): during it the game still sees its window as it was.
		CHECK(GetField<glm::vec2>(system, entity, "WindowRequests", "Size") == glm::vec2(0.0f));
		CHECK(service.HasPendingRequests());
		service.ApplyRequests();
		CHECK_FALSE(service.HasPendingRequests());
		const GameWindowState state = service.GetState();
		CHECK(state.Mode == GameWindowMode::Fullscreen);
		CHECK(state.Width == 1280);
		CHECK(state.Height == 720);
		CHECK_FALSE(state.VSync);

		// Each kind of request keeps its latest value; the service refuses what the window could not take.
		CHECK(service.SetResolution(800, 600));
		CHECK(service.SetResolution(1024, 768));
		CHECK_FALSE(service.SetResolution(0, 768));
		CHECK_FALSE(service.SetResolution(1024, IGameWindow::c_MaxResolution + 1));
		CHECK_FALSE(service.SetWindowMode(static_cast<GameWindowMode>(9)));
		service.ApplyRequests();
		CHECK(service.GetState().Width == 1024);
		CHECK(service.GetState().Mode == GameWindowMode::Fullscreen);
		scene.OnRuntimeStop();
	}

	TEST_CASE("Scripts write, read, list and delete save files in the game's save directory only")
	{
		const std::filesystem::path root = CreateTemporaryDirectory("ScriptSaves");
		SaveDirectory saves(root / "Saves");
		EngineServices::ScopedProvider<IGameSaves> provider(saves);
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		{
			Scene scene;
			Entity entity = scene.CreateEntity("Saver");
			AddScriptEntry(entity, "SaveRoundTrip");
			LogCapture log;
			scene.OnRuntimeStart();
			RunFrames(scene, 1);
			const ScriptSystem& system = GetScriptSystem(scene);
			CheckScriptChecks(system, entity, "SaveRoundTrip", 22);
			CHECK(GetField<std::string>(system, entity, "SaveRoundTrip", "Listed") == "Binary.bin;Empty;Kept;");
			CHECK(Logged(log.GetEntries(), "path separator"));
			scene.OnRuntimeStop();
		}

		// What is left: the one save, in the save directory, and nothing beside it.
		CHECK(SaveFiles::List(root / "Saves") == std::vector<std::string> { "Kept" });
		const std::optional<std::string> text = FileSystem::ReadText(root / "Saves" / "Kept.save");
		REQUIRE(text);
		CHECK(*text == "level=3;score=1200");
		CHECK_FALSE(FileSystem::Exists(root / "Escape.save"));
		CHECK_FALSE(FileSystem::Exists(root / "Saves" / "Sub"));

		// Another scene (a later session) loads it when it starts.
		Scene scene;
		Entity loader = scene.CreateEntity("Loader");
		AddScriptEntry(loader, "SaveLoader");
		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK(GetField<bool>(system, loader, "SaveLoader", "Found"));
		CHECK(GetField<std::string>(system, loader, "SaveLoader", "Loaded") == "level=3;score=1200");
		scene.OnRuntimeStop();
	}

	TEST_CASE("Without a save directory, saves are refused and reported")
	{
		SaveDirectory none;
		EngineServices::ScopedProvider<IGameSaves> provider(none);
		ScopedScriptEngine engine(GetTestScriptModule(STRATA_TEST_SCRIPTS_API));
		Scene scene;
		Entity loader = scene.CreateEntity("Loader");
		AddScriptEntry(loader, "SaveLoader");
		LogCapture log;
		scene.OnRuntimeStart();
		const ScriptSystem& system = GetScriptSystem(scene);
		CHECK_FALSE(GetField<bool>(system, loader, "SaveLoader", "Found"));
		CHECK(Logged(log.GetEntries(), "no save directory"));
		scene.OnRuntimeStop();
	}
}
