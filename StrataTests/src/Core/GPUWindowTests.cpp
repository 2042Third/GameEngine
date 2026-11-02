#include <doctest/doctest.h>

#include "Strata/Core/Log.h"
#include "Strata/Core/PlatformDetection.h"
#include "Strata/Core/Process.h"
#include "Strata/Core/Window.h"
#include "Strata/Runtime/GameWindowService.h"
#include "TestHelpers.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace Strata;

namespace
{

	// The keys that type a letter, a digit or a punctuation mark of the US layout.
	bool TypesCharacter(KeyCode key)
	{
		constexpr KeyCode c_Punctuation[] = { Key::Apostrophe, Key::Minus, Key::Comma, Key::Period, Key::Slash, Key::Semicolon, Key::Equal,
			Key::LeftBracket, Key::Backslash, Key::RightBracket, Key::GraveAccent };
		return (key >= Key::A && key <= Key::Z) || (key >= Key::D0 && key <= Key::D9)
			|| std::find(std::begin(c_Punctuation), std::end(c_Punctuation), key) != std::end(c_Punctuation);
	}

}

// A window needs a display, which runs of the gpu label have (like the editor's windowed smoke tests).
TEST_SUITE("GPU.Window")
{
	TEST_CASE("Windows give ImGui's platform layer cursor shapes, keys as the layout names them and their handle")
	{
		const uint64_t logStart = Log::GetBuffer().GetLatestSequence();
		{
			WindowSpecification specification;
			specification.Title = "Strata window test";
			specification.Width = 320;
			specification.Height = 200;
			specification.Visible = false;
			const Scope<Window> window = Window::Create(specification);
			REQUIRE(window);

			CHECK(window->GetCursorMode() == CursorMode::Normal);
			window->SetCursorMode(CursorMode::Hidden);
			CHECK(window->GetCursorMode() == CursorMode::Hidden);
			window->SetCursorMode(CursorMode::Normal);
			CHECK(window->GetCursorMode() == CursorMode::Normal);

			// Every shape (one the system lacks shows as the arrow), then the arrow again.
			for (int shape = 0; shape <= static_cast<int>(CursorShape::Hidden); shape++)
				window->SetCursorShape(static_cast<CursorShape>(shape));
			window->SetCursorShape(CursorShape::Arrow);

			// Keys that type nothing stay themselves; the letter keys name what they type, in whatever layout this machine has.
			for (const KeyCode key : { static_cast<KeyCode>(Key::F1), static_cast<KeyCode>(Key::Enter), static_cast<KeyCode>(Key::Left),
				static_cast<KeyCode>(Key::KP5), static_cast<KeyCode>(Key::LeftControl), static_cast<KeyCode>(Key::Space) })
			{
				CAPTURE(key);
				CHECK(window->GetLayoutKey(key) == key);
			}
			for (KeyCode key = Key::A; key <= Key::Z; key++)
			{
				CAPTURE(key);
				CHECK(TypesCharacter(window->GetLayoutKey(key)));
			}

			// Input methods place their candidate lists by the window's handle on Windows; elsewhere the window system does.
#if defined(ST_PLATFORM_WINDOWS)
			CHECK(window->GetPlatformHandle() != nullptr);
#else
			CHECK(window->GetPlatformHandle() == nullptr);
#endif
		}
		// Nothing of it was an error to the system (a cursor theme without a shape, a key without a name).
		for (const LogEntry& entry : Log::GetBuffer().GetEntries(logStart))
			CHECK_MESSAGE(entry.Level < LogLevel::Error, entry.Message);
	}

	TEST_CASE("A game's window requests apply to the window after the frame, sized in pixels")
	{
		// Only windowed changes: a full screen mode would show the hidden window over the display of whoever runs the tests.
		WindowSpecification specification;
		specification.Title = "Strata game window test";
		specification.Width = 320;
		specification.Height = 200;
		specification.Visible = false;
		const Scope<Window> window = Window::Create(specification);
		REQUIRE(window);
		CHECK(window->GetMode() == WindowMode::Windowed);

		GameWindowService service(window.get());
		CHECK(service.GetState().Mode == GameWindowMode::Windowed);
		CHECK(service.SetResolution(400, 300));
		CHECK(service.SetVSync(false));
		CHECK(service.SetWindowMode(GameWindowMode::Windowed));
		// Nothing changes until the requests apply.
		CHECK(window->IsVSync());
		CHECK(window->GetFramebufferSize() != glm::uvec2(400, 300));
		service.ApplyRequests();
		CHECK_FALSE(service.HasPendingRequests());
		CHECK(window->GetFramebufferSize() == glm::uvec2(400, 300)); // Pixels, whatever the display's scale
		CHECK_FALSE(window->IsVSync());
		const GameWindowState state = service.GetState();
		CHECK(state.Width == 400);
		CHECK(state.Height == 300);
		CHECK_FALSE(state.VSync);
		CHECK(state.Mode == GameWindowMode::Windowed);

		// Zero sizes leave the window as it is.
		window->SetResolution(0, 100);
		CHECK(window->GetFramebufferSize() == glm::uvec2(400, 300));
	}

	TEST_CASE("A window that cannot present through Vulkan gives a device that renders offscreen, and ImGui refuses it")
	{
		// In a child (the stub-window-graphics helper): an application's Graphics module initializes the process-wide renderer
		// the shared GPU context holds here.
		const auto run = [](std::vector<std::string> arguments)
		{
			ProcessSpecification specification;
			specification.Executable = Tests::GetTestExecutablePath();
			specification.Arguments = std::move(arguments);
			specification.Output = ProcessOutputMode::CaptureSeparate;
			return Process::Run(specification, std::chrono::seconds(120));
		};

		const Process::RunResult offscreen = run({ "--strata-test-helper=stub-window-graphics" });
		REQUIRE_MESSAGE(offscreen.Started, offscreen.Error);
		REQUIRE_FALSE(offscreen.TimedOut);
		INFO(offscreen.Output);
		INFO(offscreen.ErrorOutput);
		CHECK(offscreen.ExitCode == 0);
		CHECK(offscreen.Output.find("Frames: 10, can present: no, errors: 0") != std::string::npos);
		const std::string log = offscreen.Output + offscreen.ErrorOutput;
		CHECK(log.find("the window cannot present through Vulkan; rendering offscreen only") != std::string::npos);
		// Not "no warning": a machine without the validation layer warns at device creation, which says nothing of this path.
		CHECK(log.find("[error]") == std::string::npos);
		CHECK(log.find("[critical]") == std::string::npos);

		const Process::RunResult imgui = run({ "--strata-test-helper=stub-window-graphics", "--imgui" });
		REQUIRE_MESSAGE(imgui.Started, imgui.Error);
		REQUIRE_FALSE(imgui.TimedOut);
		INFO(imgui.Output);
		INFO(imgui.ErrorOutput);
		CHECK(imgui.ExitCode == 1);
		CHECK((imgui.Output + imgui.ErrorOutput).find("The ImGui module needs a window the Graphics module presents into") != std::string::npos);
	}

	TEST_CASE("A window of the backend gives a device that presents, and the swapchain's log names the offered modes")
	{
		// In a child, for the reason above: a hidden window, so that nothing shows on the desktop.
		ProcessSpecification specification;
		specification.Executable = Tests::GetTestExecutablePath();
		specification.Arguments = { "--strata-test-helper=window-graphics" };
		specification.Output = ProcessOutputMode::CaptureSeparate;
		const Process::RunResult result = Process::Run(specification, std::chrono::seconds(120));
		REQUIRE_MESSAGE(result.Started, result.Error);
		REQUIRE_FALSE(result.TimedOut);
		INFO(result.Output);
		INFO(result.ErrorOutput);
		CHECK(result.ExitCode == 0);
		CHECK(result.Output.find("can present: yes, errors: 0") != std::string::npos);
		// Whether vsync paces the frames is the presentation engine's: the line says which mode was chosen and which the
		// surface offers (FIFO always), so a report of the frame rate starts from the log.
		const std::string log = result.Output + result.ErrorOutput;
		CHECK(log.find("Swapchain: ") != std::string::npos);
		CHECK(log.find("images, vsync (FIFO; the surface offers ") != std::string::npos);
		CHECK(log.find("[error]") == std::string::npos);
	}
}
