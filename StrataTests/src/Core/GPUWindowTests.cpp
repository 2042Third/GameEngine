#include <doctest/doctest.h>

#include "Strata/Core/Log.h"
#include "Strata/Core/PlatformDetection.h"
#include "Strata/Core/Window.h"
#include "Strata/Runtime/GameWindowService.h"

#include <algorithm>
#include <iterator>

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
}
