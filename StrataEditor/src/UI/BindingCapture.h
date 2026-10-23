#pragma once

#include <Strata/Input/InputActions.h>

#include <optional>
#include <string>
#include <string_view>

namespace Strata
{

	// The binding capture the editor's UI runs for what asked for it (InputActions::BeginCapture: the next key, mouse button
	// or gamepad control pressed, read from the devices, also while play is paused). While it listens it takes the keyboard
	// from ImGui (every keyboard and gamepad key's owner, locked until released), so the key it captures runs no shortcut,
	// navigation or widget, and keeps it after the capture ended until every key is up (Ctrl captured, then Z, is no undo).
	// A mouse press it captured clicks nothing: the mouse buttons stay locked until released. It ends with the first control,
	// after c_TimeoutSeconds, with Cancel, or when a frame went by in which its owner did not ask after it (IsListening): the
	// page that asked is no longer drawn. Docs/DeveloperGuide.md, "Input actions". Main thread, within an ImGui frame.
	class BindingCapture
	{
	public:
		static constexpr double c_TimeoutSeconds = 10.0;

		// Starts listening for `owner` (an id of what asked), ending one another owner ran. False when InputActions refuses.
		static bool Begin(std::string_view owner, const InputCaptureDevices& devices = {});
		// Whether `owner`'s capture listens; asking keeps it for the next frame.
		static bool IsListening(std::string_view owner);
		// The binding text `owner`'s capture took, once, after which the capture is over; nullopt while it listens, and once for
		// a capture that ended without one (play started or stopped), after which it is over too.
		static std::optional<std::string> Take(std::string_view owner);
		// Ends `owner`'s capture without a binding.
		static void Cancel(std::string_view owner);
		// The seconds left before the capture times out.
		static double GetRemainingSeconds();
		// At the start of the editor's UI each frame, before any shortcut: ends a capture that timed out or whose owner did not
		// ask after it during the last frame, and holds the keyboard while one runs and until every key is up after it.
		static void BeginFrame();
	};

}
