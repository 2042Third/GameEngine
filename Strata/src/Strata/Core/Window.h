#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Events/Event.h"
#include "Strata/Input/InputWindow.h"
#include "Strata/Input/KeyCodes.h"
#include "Strata/Input/MouseCodes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace Strata
{

	// One size of a window icon: RGBA8 pixels, rows top to bottom, Width * Height * 4 bytes.
	struct WindowIconImage
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		std::span<const uint8_t> Pixels;
	};

	// The shape of the mouse cursor over a window (Window::SetCursorShape): the system's standard cursors, and none.
	enum class CursorShape : uint8_t
	{
		Arrow = 0,
		TextInput,  // An I-beam
		ResizeAll,
		ResizeNS,   // Up and down
		ResizeEW,   // Left and right
		ResizeNESW, // Diagonal, from the bottom left to the top right
		ResizeNWSE, // Diagonal, from the top left to the bottom right
		Hand,
		NotAllowed,
		Hidden      // No cursor is shown over the window (it still moves and clicks)
	};

	// How a window shows: in a frame, or covering a display, with the display switched to the window's resolution
	// (Fullscreen) or at the desktop's (Borderless).
	enum class WindowMode : uint8_t
	{
		Windowed = 0,
		Fullscreen,
		Borderless
	};

	struct WindowSpecification
	{
		std::string Title = "Strata";
		uint32_t Width = 1600;
		uint32_t Height = 900;
		bool Fullscreen = false;
		bool Resizable = true;
		bool Maximized = false;
		bool Decorated = true;
		bool VSync = true;
		bool Visible = true;
		// Width and Height are framebuffer pixels, whatever the display's scale (a windowed window only). Otherwise they are
		// screen coordinates scaled with the monitor's content scale, so a window takes the same share of every display.
		bool SizeInPixels = false;
		std::filesystem::path IconPath; // Optional PNG used as the window icon
	};

	// Platform window. The swapchain is owned by the graphics device, which queries the native handle. The size
	// (GetWidth, GetHeight, in screen coordinates) and SetCursorMode come from InputWindow, the part Input uses.
	class Window : public InputWindow
	{
	public:
		using EventCallbackFn = std::function<void(Event&)>;

		virtual ~Window() = default;

		// Pumps the OS event queue, dispatching events through the event callback and into Input.
		virtual void ProcessEvents() = 0;

		virtual glm::uvec2 GetFramebufferSize() const = 0; // Pixels (differs from window size on HiDPI displays)
		virtual float GetContentScale() const = 0;

		virtual void SetEventCallback(const EventCallbackFn& callback) = 0;
		virtual void SetVSync(bool enabled) = 0;
		virtual bool IsVSync() const = 0;
		virtual void SetTitle(const std::string& title) = 0;
		// A window created with WindowSpecification::Fullscreen starts Borderless (at the desktop's resolution). Going back to
		// Windowed restores the window's last windowed place and size.
		virtual void SetMode(WindowMode mode) = 0;
		virtual WindowMode GetMode() const = 0;
		// The size of the framebuffer in pixels: the window's in Windowed mode (whatever the display's scale), the display
		// mode's in Fullscreen. Borderless windows keep the desktop's resolution and take this one when they become Fullscreen
		// or Windowed. Zero sizes are ignored.
		virtual void SetResolution(uint32_t width, uint32_t height) = 0;
		virtual bool IsMinimized() const = 0;
		virtual bool IsFocused() const = 0;
		virtual void Maximize() = 0;
		// The cursor mode last set (InputWindow::SetCursorMode): a game that locks or hides the cursor owns it until it sets
		// Normal again.
		virtual CursorMode GetCursorMode() const = 0;
		// The cursor's shape over the window while its mode is Normal; a shape the system has no cursor for shows as the
		// arrow. A call into the system when the shape changes: set it when it changes, not every frame.
		virtual void SetCursorShape(CursorShape shape) = 0;
		// The text on the system's clipboard (UTF-8), empty when it holds no text; and putting text there.
		virtual std::string GetClipboardText() const = 0;
		virtual void SetClipboardText(const std::string& text) = 0;
		// What a key of the key events (KeyCode: where the key is, named after a US keyboard, which is what games want)
		// stands for in the keyboard layout in use: the letter, digit or punctuation it types there, e.g. Key::Q for the key
		// a French keyboard has A on, and the key itself when it types nothing of the US layout's (other keys, characters
		// the US layout has no key for). For shortcuts named after what they type (Ctrl+Z to undo).
		virtual KeyCode GetLayoutKey(KeyCode key) const = 0;
		// The window as the operating system knows it, for its text input services (input methods place their candidate
		// lists by it): the HWND on Windows; null elsewhere, where the window system places them itself.
		virtual void* GetPlatformHandle() const = 0;
		// Sets the window's icon from images of several sizes (the system picks the sizes it shows); the pixels are copied.
		// Images whose pixel count does not match their size are skipped with a warning. Platforms whose windows have no
		// icon of their own (macOS: the application bundle's; Wayland: the desktop entry's) ignore it.
		virtual void SetIcon(std::span<const WindowIconImage> images) = 0;

		virtual void* GetNativeWindow() const = 0;

		static Scope<Window> Create(const WindowSpecification& specification);
	};

}
