#include "stpch.h"
#include "Platform/GLFW/GLFWWindow.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Events/ApplicationEvent.h"
#include "Strata/Events/KeyEvent.h"
#include "Strata/Events/MouseEvent.h"
#include "Strata/Input/Input.h"
#include "Platform/Vulkan/VulkanLoader.h"

// The Vulkan header comes first so GLFW declares its Vulkan functions (glfwInitVulkanLoader).
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <stb_image.h>

namespace Strata
{

	static uint32_t s_GLFWWindowCount = 0;

	static void GLFWErrorCallback(int error, const char* description)
	{
		ST_CORE_ERROR("GLFW error ({}): {}", error, description);
	}

	Scope<Window> Window::Create(const WindowSpecification& specification)
	{
		return CreateScope<GLFWWindow>(specification);
	}

	GLFWWindow::GLFWWindow(const WindowSpecification& specification)
	{
		m_Data.Title = specification.Title;
		m_Data.Width = specification.Width;
		m_Data.Height = specification.Height;
		m_Data.VSync = specification.VSync;

		if (s_GLFWWindowCount == 0)
		{
			glfwSetErrorCallback(GLFWErrorCallback);
			// GLFW must use the engine's Vulkan loader: surfaces created through another loader are not valid for
			// the engine's instance.
			if (PFN_vkGetInstanceProcAddr getInstanceProcAddr = VulkanLoader::GetInstanceProcAddr())
				glfwInitVulkanLoader(getInstanceProcAddr);
			const int success = glfwInit();
			ST_CORE_VERIFY(success, "Could not initialize GLFW");
		}

		// Rendering goes through Vulkan; GLFW must not create an OpenGL context.
		glfwDefaultWindowHints();
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_RESIZABLE, specification.Resizable ? GLFW_TRUE : GLFW_FALSE);
		glfwWindowHint(GLFW_DECORATED, specification.Decorated ? GLFW_TRUE : GLFW_FALSE);
		glfwWindowHint(GLFW_MAXIMIZED, specification.Maximized ? GLFW_TRUE : GLFW_FALSE);
		glfwWindowHint(GLFW_VISIBLE, specification.Visible ? GLFW_TRUE : GLFW_FALSE);
		glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);

		GLFWmonitor* monitor = nullptr;
		int width = static_cast<int>(specification.Width);
		int height = static_cast<int>(specification.Height);
		if (specification.Fullscreen)
		{
			monitor = glfwGetPrimaryMonitor();
			if (const GLFWvidmode* mode = glfwGetVideoMode(monitor))
			{
				width = mode->width;
				height = mode->height;
			}
			m_Data.Fullscreen = true;
		}

		m_Window = glfwCreateWindow(width, height, m_Data.Title.c_str(), monitor, nullptr);
		ST_CORE_VERIFY(m_Window, "Could not create GLFW window");
		s_GLFWWindowCount++;

		int windowWidth = 0;
		int windowHeight = 0;
		glfwGetWindowSize(m_Window, &windowWidth, &windowHeight);
		m_Data.Width = static_cast<uint32_t>(windowWidth);
		m_Data.Height = static_cast<uint32_t>(windowHeight);
		m_WindowedWidth = static_cast<int>(specification.Width);
		m_WindowedHeight = static_cast<int>(specification.Height);

		if (!specification.IconPath.empty())
			SetIcon(specification.IconPath);

		glfwSetWindowUserPointer(m_Window, &m_Data);

		glfwSetWindowSizeCallback(m_Window, [](GLFWwindow* window, int newWidth, int newHeight)
		{
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			data.Width = static_cast<uint32_t>(newWidth);
			data.Height = static_cast<uint32_t>(newHeight);

			WindowResizeEvent event(data.Width, data.Height);
			if (data.EventCallback)
				data.EventCallback(event);
		});

		glfwSetWindowCloseCallback(m_Window, [](GLFWwindow* window)
		{
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			WindowCloseEvent event;
			if (data.EventCallback)
				data.EventCallback(event);
		});

		glfwSetWindowFocusCallback(m_Window, [](GLFWwindow* window, int focused)
		{
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			if (!data.EventCallback)
				return;

			if (focused)
			{
				WindowFocusEvent event;
				data.EventCallback(event);
			}
			else
			{
				WindowLostFocusEvent event;
				data.EventCallback(event);
			}
		});

		glfwSetWindowPosCallback(m_Window, [](GLFWwindow* window, int x, int y)
		{
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			WindowMovedEvent event(x, y);
			if (data.EventCallback)
				data.EventCallback(event);
		});

		glfwSetKeyCallback(m_Window, [](GLFWwindow* window, int key, int, int action, int)
		{
			if (key < 0)
				return;

			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			const KeyCode keyCode = static_cast<KeyCode>(key);
			switch (action)
			{
				case GLFW_PRESS:
				{
					Input::ProcessKey(keyCode, true);
					KeyPressedEvent event(keyCode, false);
					if (data.EventCallback)
						data.EventCallback(event);
					break;
				}
				case GLFW_RELEASE:
				{
					Input::ProcessKey(keyCode, false);
					KeyReleasedEvent event(keyCode);
					if (data.EventCallback)
						data.EventCallback(event);
					break;
				}
				case GLFW_REPEAT:
				{
					KeyPressedEvent event(keyCode, true);
					if (data.EventCallback)
						data.EventCallback(event);
					break;
				}
			}
		});

		glfwSetCharCallback(m_Window, [](GLFWwindow* window, unsigned int codepoint)
		{
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			KeyTypedEvent event(codepoint);
			if (data.EventCallback)
				data.EventCallback(event);
		});

		glfwSetMouseButtonCallback(m_Window, [](GLFWwindow* window, int button, int action, int)
		{
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			const MouseCode mouseCode = static_cast<MouseCode>(button);
			if (action == GLFW_PRESS)
			{
				Input::ProcessMouseButton(mouseCode, true);
				MouseButtonPressedEvent event(mouseCode);
				if (data.EventCallback)
					data.EventCallback(event);
			}
			else if (action == GLFW_RELEASE)
			{
				Input::ProcessMouseButton(mouseCode, false);
				MouseButtonReleasedEvent event(mouseCode);
				if (data.EventCallback)
					data.EventCallback(event);
			}
		});

		glfwSetScrollCallback(m_Window, [](GLFWwindow* window, double xOffset, double yOffset)
		{
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			Input::ProcessScroll(glm::vec2(static_cast<float>(xOffset), static_cast<float>(yOffset)));
			MouseScrolledEvent event(static_cast<float>(xOffset), static_cast<float>(yOffset));
			if (data.EventCallback)
				data.EventCallback(event);
		});

		glfwSetCursorPosCallback(m_Window, [](GLFWwindow* window, double x, double y)
		{
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			Input::ProcessMouseMove(glm::vec2(static_cast<float>(x), static_cast<float>(y)));
			MouseMovedEvent event(static_cast<float>(x), static_cast<float>(y));
			if (data.EventCallback)
				data.EventCallback(event);
		});

		glfwSetDropCallback(m_Window, [](GLFWwindow* window, int count, const char** paths)
		{
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			std::vector<std::filesystem::path> droppedPaths;
			droppedPaths.reserve(static_cast<size_t>(count));
			for (int index = 0; index < count; index++)
				droppedPaths.push_back(FileSystem::FromUTF8(paths[index]));

			WindowFileDropEvent event(std::move(droppedPaths));
			if (data.EventCallback)
				data.EventCallback(event);
		});

		ST_CORE_INFO("Created window '{}' ({} x {})", m_Data.Title, m_Data.Width, m_Data.Height);
	}

	GLFWWindow::~GLFWWindow()
	{
		glfwDestroyWindow(m_Window);
		s_GLFWWindowCount--;
		if (s_GLFWWindowCount == 0)
			glfwTerminate();
	}

	void GLFWWindow::ProcessEvents()
	{
		glfwPollEvents();
		PollGamepads();
	}

	void GLFWWindow::PollGamepads()
	{
		for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
		{
			const int joystick = GLFW_JOYSTICK_1 + static_cast<int>(gamepad);
			GLFWgamepadstate state;
			if (glfwJoystickIsGamepad(joystick) && glfwGetGamepadState(joystick, &state))
			{
				bool buttons[c_GamepadButtonCount];
				for (uint32_t index = 0; index < c_GamepadButtonCount; index++)
					buttons[index] = state.buttons[index] == GLFW_PRESS;
				Input::ProcessGamepad(gamepad, true, state.axes, buttons);
			}
			else
			{
				Input::ProcessGamepad(gamepad, false, nullptr, nullptr);
			}
		}
	}

	glm::uvec2 GLFWWindow::GetFramebufferSize() const
	{
		int width = 0;
		int height = 0;
		glfwGetFramebufferSize(m_Window, &width, &height);
		return glm::uvec2(static_cast<uint32_t>(std::max(width, 0)), static_cast<uint32_t>(std::max(height, 0)));
	}

	float GLFWWindow::GetContentScale() const
	{
		float scaleX = 1.0f;
		float scaleY = 1.0f;
		glfwGetWindowContentScale(m_Window, &scaleX, &scaleY);
		return std::max(scaleX, scaleY);
	}

	void GLFWWindow::SetTitle(const std::string& title)
	{
		m_Data.Title = title;
		glfwSetWindowTitle(m_Window, title.c_str());
	}

	void GLFWWindow::SetFullscreen(bool fullscreen)
	{
		if (fullscreen == m_Data.Fullscreen)
			return;

		if (fullscreen)
		{
			glfwGetWindowPos(m_Window, &m_WindowedX, &m_WindowedY);
			glfwGetWindowSize(m_Window, &m_WindowedWidth, &m_WindowedHeight);

			GLFWmonitor* monitor = glfwGetPrimaryMonitor();
			const GLFWvidmode* mode = glfwGetVideoMode(monitor);
			if (!mode)
				return;
			glfwSetWindowMonitor(m_Window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
		}
		else
		{
			glfwSetWindowMonitor(m_Window, nullptr, m_WindowedX, m_WindowedY, m_WindowedWidth, m_WindowedHeight, 0);
		}
		m_Data.Fullscreen = fullscreen;
	}

	bool GLFWWindow::IsMinimized() const
	{
		return glfwGetWindowAttrib(m_Window, GLFW_ICONIFIED) == GLFW_TRUE;
	}

	bool GLFWWindow::IsFocused() const
	{
		return glfwGetWindowAttrib(m_Window, GLFW_FOCUSED) == GLFW_TRUE;
	}

	void GLFWWindow::SetCursorMode(CursorMode mode)
	{
		int glfwMode = GLFW_CURSOR_NORMAL;
		switch (mode)
		{
			case CursorMode::Normal: glfwMode = GLFW_CURSOR_NORMAL; break;
			case CursorMode::Hidden: glfwMode = GLFW_CURSOR_HIDDEN; break;
			case CursorMode::Locked: glfwMode = GLFW_CURSOR_DISABLED; break;
		}
		glfwSetInputMode(m_Window, GLFW_CURSOR, glfwMode);
		if (mode == CursorMode::Locked && glfwRawMouseMotionSupported())
			glfwSetInputMode(m_Window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
	}

	void GLFWWindow::Maximize()
	{
		glfwMaximizeWindow(m_Window);
	}

	void GLFWWindow::SetIcon(const std::filesystem::path& iconPath)
	{
		const std::optional<std::vector<uint8_t>> fileData = FileSystem::ReadBytes(iconPath);
		if (!fileData)
		{
			ST_CORE_WARN("Window icon '{}' could not be read", FileSystem::ToUTF8(iconPath));
			return;
		}

		int width = 0;
		int height = 0;
		int channels = 0;
		stbi_uc* pixels = stbi_load_from_memory(fileData->data(), static_cast<int>(fileData->size()), &width, &height, &channels, 4);
		if (!pixels)
		{
			ST_CORE_WARN("Window icon '{}' could not be decoded", FileSystem::ToUTF8(iconPath));
			return;
		}

		GLFWimage image;
		image.width = width;
		image.height = height;
		image.pixels = pixels;
		glfwSetWindowIcon(m_Window, 1, &image);
		stbi_image_free(pixels);
	}

}
