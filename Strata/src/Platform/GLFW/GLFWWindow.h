#pragma once

#include "Strata/Core/Window.h"

struct GLFWwindow;

namespace Strata
{

	class GLFWWindow : public Window
	{
	public:
		explicit GLFWWindow(const WindowSpecification& specification);
		~GLFWWindow() override;

		void ProcessEvents() override;

		uint32_t GetWidth() const override { return m_Data.Width; }
		uint32_t GetHeight() const override { return m_Data.Height; }
		glm::uvec2 GetFramebufferSize() const override;
		float GetContentScale() const override;

		void SetEventCallback(const EventCallbackFn& callback) override { m_Data.EventCallback = callback; }
		void SetVSync(bool enabled) override { m_Data.VSync = enabled; }
		bool IsVSync() const override { return m_Data.VSync; }
		void SetTitle(const std::string& title) override;
		void SetFullscreen(bool fullscreen) override;
		bool IsFullscreen() const override { return m_Data.Fullscreen; }
		bool IsMinimized() const override;
		bool IsFocused() const override;
		void SetCursorMode(CursorMode mode) override;
		void Maximize() override;

		void* GetNativeWindow() const override { return m_Window; }
	private:
		void SetIcon(const std::filesystem::path& iconPath);
		void PollGamepads();
	private:
		GLFWwindow* m_Window = nullptr;

		struct WindowData
		{
			std::string Title;
			uint32_t Width = 0;
			uint32_t Height = 0;
			bool VSync = true;
			bool Fullscreen = false;
			EventCallbackFn EventCallback;
		};

		WindowData m_Data;
		int m_WindowedX = 0;
		int m_WindowedY = 0;
		int m_WindowedWidth = 0;
		int m_WindowedHeight = 0;
	};

}
