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
#if defined(ST_PLATFORM_WINDOWS)
	// The HWND, for the system's text input services (GetPlatformHandle).
	#define GLFW_EXPOSE_NATIVE_WIN32
	#include <GLFW/glfw3native.h>
#endif

#include <stb_image.h>

namespace Strata
{

	static uint32_t s_GLFWWindowCount = 0;

	static void GLFWErrorCallback(int error, const char* description)
	{
		ST_CORE_ERROR("GLFW error ({}): {}", error, description);
	}

	// Runs a GLFW call whose failure is an answer rather than an error (a cursor the system lacks, a clipboard without
	// text): without reporting it through the error callback.
	template<typename Function>
	static auto WithoutErrorReports(Function&& function)
	{
		const GLFWerrorfun previous = glfwSetErrorCallback(nullptr);
		auto result = function();
		glfwSetErrorCallback(previous);
		glfwGetError(nullptr);
		return result;
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
		const bool sizeInPixels = specification.SizeInPixels && !specification.Fullscreen;
		glfwWindowHint(GLFW_SCALE_TO_MONITOR, sizeInPixels ? GLFW_FALSE : GLFW_TRUE);

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
			m_Mode = WindowMode::Borderless;
		}

		m_Window = glfwCreateWindow(width, height, m_Data.Title.c_str(), monitor, nullptr);
		ST_CORE_VERIFY(m_Window, "Could not create GLFW window");
		s_GLFWWindowCount++;

		int windowWidth = 0;
		int windowHeight = 0;
		glfwGetWindowSize(m_Window, &windowWidth, &windowHeight);
		if (sizeInPixels)
		{
			// Where screen coordinates are not pixels (a Retina display), the framebuffer differs: size the window so that it
			// has the pixels asked for.
			int framebufferWidth = 0;
			int framebufferHeight = 0;
			glfwGetFramebufferSize(m_Window, &framebufferWidth, &framebufferHeight);
			if (framebufferWidth > 0 && framebufferHeight > 0 && (framebufferWidth != width || framebufferHeight != height))
			{
				const auto scaled = [](int size, int windowSize, int framebufferSize)
				{
					return std::max(1, static_cast<int>(std::lround(static_cast<double>(size) * windowSize / framebufferSize)));
				};
				glfwSetWindowSize(m_Window, scaled(width, windowWidth, framebufferWidth), scaled(height, windowHeight, framebufferHeight));
				glfwGetWindowSize(m_Window, &windowWidth, &windowHeight);
			}
		}
		m_Data.Width = static_cast<uint32_t>(windowWidth);
		m_Data.Height = static_cast<uint32_t>(windowHeight);
		m_WindowedWidth = static_cast<int>(specification.Width);
		m_WindowedHeight = static_cast<int>(specification.Height);

		if (!specification.IconPath.empty())
			SetIconFromFile(specification.IconPath);

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

		glfwSetWindowContentScaleCallback(m_Window, [](GLFWwindow* window, float scaleX, float scaleY)
		{
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			// Like GetContentScale: one scale for both axes, the larger one.
			WindowContentScaleEvent event(std::max(scaleX, scaleY));
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

		glfwSetCursorEnterCallback(m_Window, [](GLFWwindow* window, int entered)
		{
			// Entering needs no event: moves over the window follow, with the cursor's position.
			WindowData& data = *static_cast<WindowData*>(glfwGetWindowUserPointer(window));
			if (entered || !data.EventCallback)
				return;
			MouseLeaveEvent event;
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
		for (GLFWcursor* cursor : m_Cursors)
		{
			if (cursor)
				glfwDestroyCursor(cursor);
		}
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
				// Input's conventions (GamepadCodes.h): sticks +Y up (GLFW reports +Y down), triggers 0 to 1 (GLFW: -1 to 1).
				float axes[c_GamepadAxisCount];
				axes[static_cast<size_t>(GamepadAxis::LeftX)] = state.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
				axes[static_cast<size_t>(GamepadAxis::LeftY)] = -state.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
				axes[static_cast<size_t>(GamepadAxis::RightX)] = state.axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
				axes[static_cast<size_t>(GamepadAxis::RightY)] = -state.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
				axes[static_cast<size_t>(GamepadAxis::LeftTrigger)] = (state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] + 1.0f) * 0.5f;
				axes[static_cast<size_t>(GamepadAxis::RightTrigger)] = (state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] + 1.0f) * 0.5f;
				Input::ProcessGamepad(gamepad, true, axes, buttons);
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

	void GLFWWindow::SetMode(WindowMode mode)
	{
		if (mode == m_Mode)
			return;

		// The window covers the display it is on (the primary one while it is windowed).
		GLFWmonitor* monitor = glfwGetWindowMonitor(m_Window);
		if (!monitor)
			monitor = glfwGetPrimaryMonitor();
		const GLFWvidmode* desktop = monitor ? glfwGetVideoMode(monitor) : nullptr;
		if (mode != WindowMode::Windowed && !desktop)
		{
			ST_CORE_WARN("Window: there is no display to cover; the window stays as it is");
			return;
		}

		if (m_Mode == WindowMode::Windowed)
		{
			glfwGetWindowPos(m_Window, &m_WindowedX, &m_WindowedY);
			glfwGetWindowSize(m_Window, &m_WindowedWidth, &m_WindowedHeight);
		}
		switch (mode)
		{
			case WindowMode::Windowed:
				glfwSetWindowMonitor(m_Window, nullptr, m_WindowedX, m_WindowedY, m_WindowedWidth, m_WindowedHeight, 0);
				m_Mode = mode;
				if (m_Resolution.x > 0 && m_Resolution.y > 0)
					SetFramebufferSize(static_cast<int>(m_Resolution.x), static_cast<int>(m_Resolution.y));
				return;
			case WindowMode::Fullscreen:
			{
				// The display switches to the resolution asked for, or stays at the desktop's.
				const int width = m_Resolution.x > 0 ? static_cast<int>(m_Resolution.x) : desktop->width;
				const int height = m_Resolution.y > 0 ? static_cast<int>(m_Resolution.y) : desktop->height;
				glfwSetWindowMonitor(m_Window, monitor, 0, 0, width, height, GLFW_DONT_CARE);
				break;
			}
			case WindowMode::Borderless:
				// A full screen window at the desktop's video mode does not switch the display's mode.
				glfwSetWindowMonitor(m_Window, monitor, 0, 0, desktop->width, desktop->height, desktop->refreshRate);
				break;
		}
		m_Mode = mode;
	}

	void GLFWWindow::SetResolution(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0)
			return;
		m_Resolution = glm::uvec2(width, height);
		if (m_Mode == WindowMode::Windowed)
		{
			SetFramebufferSize(static_cast<int>(width), static_cast<int>(height));
		}
		else if (m_Mode == WindowMode::Fullscreen)
		{
			if (GLFWmonitor* monitor = glfwGetWindowMonitor(m_Window))
				glfwSetWindowMonitor(m_Window, monitor, 0, 0, static_cast<int>(width), static_cast<int>(height), GLFW_DONT_CARE);
		}
	}

	void GLFWWindow::SetFramebufferSize(int width, int height)
	{
		// Where screen coordinates are not pixels (a scaled display), the window's size in screen coordinates differs from
		// its framebuffer's: scale by the ratio of the two.
		int windowWidth = 0;
		int windowHeight = 0;
		int framebufferWidth = 0;
		int framebufferHeight = 0;
		glfwGetWindowSize(m_Window, &windowWidth, &windowHeight);
		glfwGetFramebufferSize(m_Window, &framebufferWidth, &framebufferHeight);
		const auto scaled = [](int size, int windowSize, int framebufferSize)
		{
			if (windowSize <= 0 || framebufferSize <= 0)
				return size;
			return std::max(1, static_cast<int>(std::lround(static_cast<double>(size) * windowSize / framebufferSize)));
		};
		glfwSetWindowSize(m_Window, scaled(width, windowWidth, framebufferWidth), scaled(height, windowHeight, framebufferHeight));
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
		m_CursorMode = mode;
	}

	GLFWcursor* GLFWWindow::GetCursor(CursorShape shape)
	{
		const size_t index = static_cast<size_t>(shape);
		if (index >= m_Cursors.size() || shape == CursorShape::Arrow)
			return nullptr;
		if (m_CursorsCreated[index])
			return m_Cursors[index];
		m_CursorsCreated[index] = true;
		if (shape == CursorShape::Hidden)
		{
			// A cursor image without a visible pixel: hiding the cursor this way leaves the cursor mode to the game.
			std::array<unsigned char, 16 * 16 * 4> pixels = {};
			const GLFWimage image { 16, 16, pixels.data() };
			m_Cursors[index] = WithoutErrorReports([&]() { return glfwCreateCursor(&image, 0, 0); });
			return m_Cursors[index];
		}
		int standard = GLFW_ARROW_CURSOR;
		switch (shape)
		{
			case CursorShape::TextInput:  standard = GLFW_IBEAM_CURSOR; break;
			case CursorShape::ResizeAll:  standard = GLFW_RESIZE_ALL_CURSOR; break;
			case CursorShape::ResizeNS:   standard = GLFW_RESIZE_NS_CURSOR; break;
			case CursorShape::ResizeEW:   standard = GLFW_RESIZE_EW_CURSOR; break;
			case CursorShape::ResizeNESW: standard = GLFW_RESIZE_NESW_CURSOR; break;
			case CursorShape::ResizeNWSE: standard = GLFW_RESIZE_NWSE_CURSOR; break;
			case CursorShape::Hand:       standard = GLFW_POINTING_HAND_CURSOR; break;
			case CursorShape::NotAllowed: standard = GLFW_NOT_ALLOWED_CURSOR; break;
			case CursorShape::Arrow:
			case CursorShape::Hidden:     break;
		}
		// Cursor themes may lack the newer shapes (X11): GLFW then fails with GLFW_CURSOR_UNAVAILABLE, and the arrow shows.
		m_Cursors[index] = WithoutErrorReports([standard]() { return glfwCreateStandardCursor(standard); });
		return m_Cursors[index];
	}

	void GLFWWindow::SetCursorShape(CursorShape shape)
	{
		if (shape == m_CursorShape)
			return;
		m_CursorShape = shape;
		glfwSetCursor(m_Window, GetCursor(shape));
	}

	std::string GLFWWindow::GetClipboardText() const
	{
		// A clipboard without text is no error (GLFW_FORMAT_UNAVAILABLE).
		const char* text = WithoutErrorReports([this]() { return glfwGetClipboardString(m_Window); });
		return text ? std::string(text) : std::string();
	}

	void GLFWWindow::SetClipboardText(const std::string& text)
	{
		glfwSetClipboardString(m_Window, text.c_str());
	}

	KeyCode GLFWWindow::GetLayoutKey(KeyCode key) const
	{
		// The keys that type a character on a US keyboard; the keypad types the same in every layout.
		constexpr std::string_view c_Punctuation = "'-,./;=[\\]`";
		constexpr KeyCode c_PunctuationKeys[] = { Key::Apostrophe, Key::Minus, Key::Comma, Key::Period, Key::Slash, Key::Semicolon, Key::Equal,
			Key::LeftBracket, Key::Backslash, Key::RightBracket, Key::GraveAccent };
		static_assert(std::size(c_PunctuationKeys) == c_Punctuation.size());
		const bool types = (key >= Key::A && key <= Key::Z) || (key >= Key::D0 && key <= Key::D9)
			|| std::find(std::begin(c_PunctuationKeys), std::end(c_PunctuationKeys), key) != std::end(c_PunctuationKeys);
		if (!types)
			return key;
		// What the key types in the layout, as one character (letters in either case); several bytes are a character the US
		// layout has no key for.
		const char* name = WithoutErrorReports([key]() { return glfwGetKeyName(static_cast<int>(key), 0); });
		if (!name || name[0] == '\0' || name[1] != '\0')
			return key;
		const char character = name[0];
		if (character >= 'a' && character <= 'z')
			return static_cast<KeyCode>(Key::A + (character - 'a'));
		if (character >= 'A' && character <= 'Z')
			return static_cast<KeyCode>(Key::A + (character - 'A'));
		if (character >= '0' && character <= '9')
			return static_cast<KeyCode>(Key::D0 + (character - '0'));
		const size_t punctuation = c_Punctuation.find(character);
		return punctuation != std::string_view::npos ? c_PunctuationKeys[punctuation] : key;
	}

	void* GLFWWindow::GetPlatformHandle() const
	{
#if defined(ST_PLATFORM_WINDOWS)
		return glfwGetWin32Window(m_Window);
#else
		return nullptr;
#endif
	}

	void GLFWWindow::Maximize()
	{
		glfwMaximizeWindow(m_Window);
	}

	void GLFWWindow::SetIcon(std::span<const WindowIconImage> images)
	{
		// Windows there have no icons of their own: GLFW would report the call as an error.
		const int platform = glfwGetPlatform();
		if (platform == GLFW_PLATFORM_COCOA || platform == GLFW_PLATFORM_WAYLAND)
			return;

		std::vector<GLFWimage> glfwImages;
		glfwImages.reserve(images.size());
		for (const WindowIconImage& image : images)
		{
			const uint64_t expectedBytes = static_cast<uint64_t>(image.Width) * image.Height * 4;
			if (image.Width == 0 || image.Height == 0 || image.Width > INT32_MAX || image.Height > INT32_MAX || image.Pixels.size() != expectedBytes)
			{
				ST_CORE_WARN("Window icon image {} x {} has {} bytes of pixels, expected {}; it is skipped", image.Width, image.Height, image.Pixels.size(), expectedBytes);
				continue;
			}
			GLFWimage& glfwImage = glfwImages.emplace_back();
			glfwImage.width = static_cast<int>(image.Width);
			glfwImage.height = static_cast<int>(image.Height);
			// GLFW only reads the pixels (it copies them into the platform's icon).
			glfwImage.pixels = const_cast<unsigned char*>(image.Pixels.data());
		}
		if (!glfwImages.empty())
			glfwSetWindowIcon(m_Window, static_cast<int>(glfwImages.size()), glfwImages.data());
	}

	void GLFWWindow::SetIconFromFile(const std::filesystem::path& iconPath)
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

		const WindowIconImage image { static_cast<uint32_t>(width), static_cast<uint32_t>(height),
			std::span<const uint8_t>(pixels, static_cast<size_t>(width) * static_cast<size_t>(height) * 4) };
		SetIcon(std::span<const WindowIconImage>(&image, 1));
		stbi_image_free(pixels);
	}

}
