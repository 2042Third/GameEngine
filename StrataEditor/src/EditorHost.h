#pragma once

#include <Strata/Renderer/GraphicsDevice.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace Strata
{

	struct ReadbackImage;

	// What the editor's UI layer (EditorLayer) needs from the application that runs it: closing, the exit code, the frame
	// count, the window (title, size, focus, UI scale), the frame rate and frame time, and screenshots. EditorApplication implements it on
	// top of Application; UI tests supply a fake (ImGuiHarness), so the layer runs without an Application, a window or a GPU.
	class EditorHost
	{
	public:
		virtual ~EditorHost() = default;

		virtual bool IsRunning() const = 0;
		// Ends the application after the current frame.
		virtual void Close() = 0;
		virtual void SetExitCode(int exitCode) = 0;
		// Frames that ran so far.
		virtual uint64_t GetFrameCount() const = 0;
		// Seconds since the application started (monotonic). The layer measures idle time and frame rates with it.
		virtual double GetTime() const = 0;
		// Seconds since the process was created (Platform::GetProcessUptime): the layer reads it once the first frame is on
		// screen, as the editor's startup time. nullopt when the system does not tell.
		virtual std::optional<double> GetProcessUptime() const = 0;
		virtual bool HasGraphicsDevice() const = 0;
		// The GPU the editor renders with (adapter, driver, API version); nullopt without a graphics device.
		virtual std::optional<GraphicsDeviceInfo> GetGraphicsDeviceInfo() const = 0;

		// The window. Without one the title is ignored, the size is zero and it never has the focus.
		virtual bool HasWindow() const = 0;
		virtual void SetWindowTitle(const std::string& title) = 0;
		virtual glm::uvec2 GetWindowSize() const = 0;
		virtual bool IsWindowFocused() const = 0;
		// The scale the UI is drawn at: the window's content scale, or the --ui-scale override.
		virtual float GetUIScale() const = 0;

		// Frames per second the application does not exceed from the next frame on (0: unlimited).
		virtual void SetMaxFrameRate(uint32_t framesPerSecond) = 0;
		virtual uint32_t GetMaxFrameRate() const = 0;
		// CPU seconds the last frame cost (Application::GetLastFrameWorkTime): without the waits for the GPU, the display
		// and the frame rate cap.
		virtual double GetLastFrameWorkTime() const = 0;

		// Reads the window back at the end of the next rendered frame (with the UI) and passes the image to the callback on
		// the main thread; the image is empty when nothing can be captured.
		virtual void RequestScreenshot(std::function<void(const ReadbackImage&)> callback) = 0;
	};

}
