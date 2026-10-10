#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Events/Event.h"
#include "Strata/Input/InputWindow.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <functional>
#include <string>

namespace Strata
{

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
		virtual void SetFullscreen(bool fullscreen) = 0;
		virtual bool IsFullscreen() const = 0;
		virtual bool IsMinimized() const = 0;
		virtual bool IsFocused() const = 0;
		virtual void Maximize() = 0;

		virtual void* GetNativeWindow() const = 0;

		static Scope<Window> Create(const WindowSpecification& specification);
	};

}
