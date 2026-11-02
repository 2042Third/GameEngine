#pragma once

#include "Strata/Core/Window.h"
#include "Platform/Vulkan/VulkanPresentation.h"

#include <array>
#include <string>
#include <vector>

struct GLFWcursor;
struct GLFWwindow;

namespace Strata
{

	// The window of the GLFW backend; it gives the Vulkan device its surfaces through GLFW (VulkanPresentation).
	class GLFWWindow : public Window, private VulkanPresentation
	{
	public:
		GLFWWindow() = default;
		~GLFWWindow() override;

		// Opens the window (Window::Create only); false, with the reason logged, when GLFW cannot be initialized (no display)
		// or the window cannot be created. A window that was never opened must not be used, only destroyed.
		bool Open(const WindowSpecification& specification);

		void ProcessEvents() override;

		uint32_t GetWidth() const override { return m_Data.Width; }
		uint32_t GetHeight() const override { return m_Data.Height; }
		glm::uvec2 GetFramebufferSize() const override;
		float GetContentScale() const override;

		void SetEventCallback(const EventCallbackFn& callback) override { m_Data.EventCallback = callback; }
		void SetVSync(bool enabled) override { m_Data.VSync = enabled; }
		bool IsVSync() const override { return m_Data.VSync; }
		void SetTitle(const std::string& title) override;
		void SetMode(WindowMode mode) override;
		WindowMode GetMode() const override { return m_Mode; }
		void SetResolution(uint32_t width, uint32_t height) override;
		bool IsMinimized() const override;
		bool IsFocused() const override;
		void SetCursorMode(CursorMode mode) override;
		CursorMode GetCursorMode() const override { return m_CursorMode; }
		void SetCursorShape(CursorShape shape) override;
		std::string GetClipboardText() const override;
		void SetClipboardText(const std::string& text) override;
		KeyCode GetLayoutKey(KeyCode key) const override;
		void* GetPlatformHandle() const override;
		void Maximize() override;
		void SetIcon(std::span<const WindowIconImage> images) override;

		const VulkanPresentation* GetVulkanPresentation() const override { return this; }
	private:
		// VulkanPresentation, through GLFW's Vulkan functions (GLFW loads Vulkan through VulkanLoader).
		bool GetRequiredInstanceExtensions(std::vector<std::string>& outExtensions) const override;
		bool SupportsPresentation(VkInstance instance, VkPhysicalDevice physicalDevice, uint32_t queueFamily) const override;
		VkResult CreateSurface(VkInstance instance, VkSurfaceKHR& outSurface) const override;


		// Decodes a PNG file (WindowSpecification::IconPath) into the icon.
		void SetIconFromFile(const std::filesystem::path& iconPath);
		void PollGamepads();
		// Sizes the window (in Windowed mode) so that its framebuffer has `width` x `height` pixels.
		void SetFramebufferSize(int width, int height);
		// The system cursor of a shape, created on first use; null for the arrow (GLFW's default cursor) and for shapes the
		// system has no cursor for.
		GLFWcursor* GetCursor(CursorShape shape);
	private:
		GLFWwindow* m_Window = nullptr;
		CursorMode m_CursorMode = CursorMode::Normal;
		CursorShape m_CursorShape = CursorShape::Arrow;
		std::array<GLFWcursor*, static_cast<size_t>(CursorShape::Hidden) + 1> m_Cursors = {};
		std::array<bool, static_cast<size_t>(CursorShape::Hidden) + 1> m_CursorsCreated = {};

		struct WindowData
		{
			std::string Title;
			uint32_t Width = 0;
			uint32_t Height = 0;
			bool VSync = true;
			EventCallbackFn EventCallback;
		};

		WindowData m_Data;
		WindowMode m_Mode = WindowMode::Windowed;
		glm::uvec2 m_Resolution = glm::uvec2(0); // SetResolution's pixels (zero: none asked for)
		int m_WindowedX = 0;
		int m_WindowedY = 0;
		int m_WindowedWidth = 0;
		int m_WindowedHeight = 0;
	};

}
