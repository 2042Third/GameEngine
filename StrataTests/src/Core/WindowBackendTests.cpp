#include <doctest/doctest.h>

#include "Strata/Core/Application.h"
#include "Strata/Core/EngineModule.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Layer.h"
#include "Strata/Core/Log.h"
#include "Strata/Core/PlatformDetection.h"
#include "Strata/Core/Process.h"
#include "Strata/Core/Window.h"
#include "Strata/Events/Event.h"
#include "Strata/Input/Input.h"
#include "StubWindow.h"
#include "TestHelpers.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace Strata;

// The window seam: what an Application does with the window its backend (or a test's factory) makes.
namespace
{

	// Records whether the application started it.
	class StartRecordingModule final : public EngineModule
	{
	public:
		explicit StartRecordingModule(bool& started)
			: m_Started(started)
		{
		}

		std::string_view GetName() const override { return "StartRecording"; }
		bool OnInit(EngineContext&) override
		{
			m_Started = true;
			return true;
		}
	private:
		bool& m_Started;
	};

	ApplicationSpecification MakeWindowedSpecification()
	{
		ApplicationSpecification specification;
		specification.Name = "WindowBackendTest";
		specification.Headless = false;
		specification.Jobs.WorkerThreadCount = 1;
		specification.Jobs.IOThreadCount = 1;
		return specification;
	}

	// A specification whose window is a stub the test keeps (valid while the application lives).
	ApplicationSpecification MakeStubSpecification(Tests::StubWindow*& outWindow)
	{
		ApplicationSpecification specification = MakeWindowedSpecification();
		specification.WindowFactory = [&outWindow](const WindowSpecification&) -> Scope<Window>
		{
			Scope<Tests::StubWindow> window = CreateScope<Tests::StubWindow>();
			outWindow = window.get();
			return window;
		};
		return specification;
	}

	// Records the events it sees and its updates; calls OnFrame in every update with the update's number (from 1). Handles
	// the first close request when told to, as a layer that asks about unsaved changes does.
	class RecordingLayer final : public Layer
	{
	public:
		RecordingLayer(std::vector<EventType>& events, uint32_t& updates, std::function<void(uint32_t)> onFrame = {}, bool handleFirstClose = false)
			: Layer("Recording"), m_Events(events), m_Updates(updates), m_OnFrame(std::move(onFrame)), m_HandleFirstClose(handleFirstClose)
		{
		}

		void OnUpdate(Timestep) override
		{
			m_Updates++;
			if (m_OnFrame)
				m_OnFrame(m_Updates);
		}

		void OnEvent(Event& event) override
		{
			m_Events.push_back(event.GetEventType());
			if (event.GetEventType() == EventType::WindowClose && m_HandleFirstClose)
			{
				m_HandleFirstClose = false;
				event.Handled = true;
			}
		}
	private:
		std::vector<EventType>& m_Events;
		uint32_t& m_Updates;
		std::function<void(uint32_t)> m_OnFrame;
		bool m_HandleFirstClose = false;
	};

}

TEST_SUITE("Core.WindowBackend")
{
	TEST_CASE("A window it is given runs the frame loop: its events reach Input and the layers")
	{
		Tests::StubWindow* stub = nullptr;
		ApplicationSpecification specification = MakeStubSpecification(stub);
		specification.MaxFrames = 2;
		Application application(specification);
		REQUIRE(application.IsRunning());
		REQUIRE(stub);
		CHECK(application.GetWindow() == stub);

		stub->OnProcessEvents = [](Tests::StubWindow& window, uint32_t call)
		{
			if (call == 0)
			{
				window.QueueKey(Key::A, true);
				window.QueueMouseMove(glm::vec2(10.0f, 20.0f));
				window.QueueMouseButton(Mouse::ButtonLeft, true);
				window.QueueScroll(glm::vec2(0.0f, 1.0f));
				window.QueueText('a');
				window.QueueResize(800, 600);
				Tests::StubWindow::GamepadState gamepad;
				gamepad.Buttons[static_cast<size_t>(GamepadButton::A)] = true;
				gamepad.Axes[static_cast<size_t>(GamepadAxis::LeftX)] = 0.5f;
				window.SetGamepad(0, gamepad);
			}
			else if (call == 1)
			{
				window.QueueKey(Key::A, false);
				window.SetGamepad(0, std::nullopt);
			}
		};

		std::vector<EventType> events;
		uint32_t updates = 0;
		// Doctest's checks are on this thread: the frame loop runs here too. Read in the updates, checked after Run.
		bool keyPressed = false;
		glm::vec2 mousePosition(0.0f);
		bool buttonPressed = false;
		bool gamepadConnected = false;
		bool gamepadButtonPressed = false;
		float gamepadAxis = 0.0f;
		std::string typedText;
		bool keyReleased = false;
		bool gamepadConnectedLater = true;
		application.PushLayer(new RecordingLayer(events, updates, [&](uint32_t update)
		{
			if (update == 1)
			{
				keyPressed = Input::IsKeyPressed(Key::A);
				mousePosition = Input::GetMousePosition();
				buttonPressed = Input::IsMouseButtonPressed(Mouse::ButtonLeft);
				gamepadConnected = Input::IsGamepadConnected(0);
				gamepadButtonPressed = Input::IsGamepadButtonPressed(0, GamepadButton::A);
				gamepadAxis = Input::GetGamepadAxis(0, GamepadAxis::LeftX);
				typedText = Input::GetTypedText();
			}
			else if (update == 2)
			{
				keyReleased = Input::IsKeyReleased(Key::A);
				gamepadConnectedLater = Input::IsGamepadConnected(0);
			}
		}));
		application.Run();

		CHECK(application.GetFrameCount() == 2);
		CHECK(updates == 2);
		CHECK(stub->ProcessEventsCalls == application.GetFrameCount());
		CHECK(keyPressed);
		CHECK(mousePosition == glm::vec2(10.0f, 20.0f));
		CHECK(buttonPressed);
		CHECK(gamepadConnected);
		CHECK(gamepadButtonPressed);
		CHECK(gamepadAxis == 0.5f);
		CHECK(typedText == "a");
		CHECK(keyReleased);
		CHECK_FALSE(gamepadConnectedLater);
		CHECK(stub->Width == 800);
		CHECK(stub->Framebuffer == glm::uvec2(800, 600));
		const std::vector<EventType> expected = { EventType::KeyPressed, EventType::MouseMoved, EventType::MouseButtonPressed, EventType::MouseScrolled,
			EventType::KeyTyped, EventType::WindowResize, EventType::KeyReleased };
		CHECK(events == expected);
	}

	TEST_CASE("Frames while the window is minimized are skipped and not counted")
	{
		Tests::StubWindow* stub = nullptr;
		ApplicationSpecification specification = MakeStubSpecification(stub);
		specification.MaxFrames = 5;
		Application application(specification);
		REQUIRE(stub);
		stub->OnProcessEvents = [](Tests::StubWindow& window, uint32_t call) { window.Minimized = call >= 1 && call <= 3; };
		std::vector<EventType> events;
		uint32_t updates = 0;
		application.PushLayer(new RecordingLayer(events, updates));
		application.Run();

		CHECK(stub->ProcessEventsCalls == 8);
		CHECK(application.GetFrameCount() == 5);
		CHECK(updates == 5);
	}

	TEST_CASE("A close request ends the loop unless a layer handles it")
	{
		for (const bool handleFirst : { false, true })
		{
			CAPTURE(handleFirst);
			Tests::StubWindow* stub = nullptr;
			ApplicationSpecification specification = MakeStubSpecification(stub);
			specification.MaxFrames = 20; // A bound in case the close is lost
			Application application(specification);
			REQUIRE(stub);
			stub->OnProcessEvents = [](Tests::StubWindow& window, uint32_t call)
			{
				if (call == 2 || call == 4)
					window.QueueClose();
			};
			std::vector<EventType> events;
			uint32_t updates = 0;
			application.PushLayer(new RecordingLayer(events, updates, {}, handleFirst));
			application.Run();

			// The frame that receives the close still runs to its end.
			CHECK(application.GetFrameCount() == (handleFirst ? 5u : 3u));
			CHECK(std::count(events.begin(), events.end(), EventType::WindowClose) == (handleFirst ? 2 : 1));
		}
	}

	TEST_CASE("A window that cannot be opened ends the start through the fatal-error path with exit code 1")
	{
		ApplicationSpecification specification = MakeWindowedSpecification();
		specification.WindowFactory = [](const WindowSpecification&) -> Scope<Window> { return nullptr; };
		bool moduleStarted = false;
		specification.Modules.push_back([&moduleStarted]() -> Scope<EngineModule> { return CreateScope<StartRecordingModule>(moduleStarted); });

		const Tests::LogCapture log;
		Application application(specification);
		CHECK_FALSE(application.IsRunning());
		CHECK(application.GetExitCode() == 1);
		CHECK(application.GetWindow() == nullptr);
		CHECK_FALSE(moduleStarted);
		const auto start = std::chrono::steady_clock::now();
		application.Run();
		CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
		CHECK(application.GetFrameCount() == 0);

		const std::vector<LogEntry> entries = log.GetEntries();
		const size_t reports = static_cast<size_t>(std::count_if(entries.begin(), entries.end(), [](const LogEntry& entry)
		{
			return entry.Level == LogLevel::Critical && entry.Message.find("its window could not be opened") != std::string::npos;
		}));
		CHECK(reports == 1);
	}

#if defined(ST_PLATFORM_LINUX) && ST_WITH_GLFW
	TEST_CASE("Window::Create returns null without a display instead of aborting")
	{
		// The child's environment only (a helper sets it): no X11 display, no Wayland socket.
		ProcessSpecification specification;
		specification.Executable = Tests::GetTestExecutablePath();
		specification.Arguments = { "--strata-test-helper=run-with-environment", "DISPLAY=", "WAYLAND_DISPLAY=", "XDG_RUNTIME_DIR=", "--",
			FileSystem::ToUTF8(Tests::GetTestExecutablePath()), "--strata-test-helper=open-window" };
		specification.Output = ProcessOutputMode::CaptureSeparate;
		const Process::RunResult result = Process::Run(specification, std::chrono::seconds(60));
		REQUIRE_MESSAGE(result.Started, result.Error);
		REQUIRE_FALSE(result.TimedOut);
		INFO(result.Output);
		INFO(result.ErrorOutput);
		CHECK(result.ExitCode == 0);
		CHECK(result.Output.find("No window") != std::string::npos);
		const std::string log = result.Output + result.ErrorOutput;
		CHECK(log.find("GLFW error") != std::string::npos);
		CHECK(log.find("could not be initialized") != std::string::npos);
	}
#endif

#if !ST_WITH_GLFW
	TEST_CASE("A build without a window backend opens no window and says how to run")
	{
		{
			const Tests::LogCapture log;
			WindowSpecification windowSpecification;
			windowSpecification.Title = "No backend";
			const Scope<Window> window = Window::Create(windowSpecification);
			CHECK_FALSE(window);
			const std::vector<LogEntry> entries = log.GetEntries();
			CHECK(std::any_of(entries.begin(), entries.end(), [](const LogEntry& entry)
			{
				return entry.Level == LogLevel::Error && entry.Message.find("STRATA_WITH_GLFW") != std::string::npos;
			}));
		}

		// A windowed start without a factory ends through the fatal-error path.
		const Tests::LogCapture log;
		Application application(MakeWindowedSpecification());
		CHECK_FALSE(application.IsRunning());
		CHECK(application.GetExitCode() == 1);
		const std::vector<LogEntry> entries = log.GetEntries();
		CHECK(std::any_of(entries.begin(), entries.end(), [](const LogEntry& entry)
		{
			return entry.Level == LogLevel::Critical && entry.Message.find("its window could not be opened") != std::string::npos;
		}));
	}
#endif
}
