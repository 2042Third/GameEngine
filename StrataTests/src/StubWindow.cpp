#include "StubWindow.h"

#include "Strata/Events/ApplicationEvent.h"
#include "Strata/Events/KeyEvent.h"
#include "Strata/Events/MouseEvent.h"
#include "Strata/Input/Input.h"

#include <cmath>
#include <utility>

namespace Strata::Tests
{

	void StubWindow::QueueKey(KeyCode key, bool down, bool repeat)
	{
		m_Queue.push_back([this, key, down, repeat]()
		{
			// As GLFW's backend: a repeat changes no key state.
			if (down && repeat)
			{
				KeyPressedEvent event(key, true);
				Dispatch(event);
				return;
			}
			Input::ProcessKey(key, down);
			if (down)
			{
				KeyPressedEvent event(key, false);
				Dispatch(event);
			}
			else
			{
				KeyReleasedEvent event(key);
				Dispatch(event);
			}
		});
	}

	void StubWindow::QueueText(uint32_t codepoint)
	{
		m_Queue.push_back([this, codepoint]()
		{
			Input::ProcessText(codepoint);
			KeyTypedEvent event(codepoint);
			Dispatch(event);
		});
	}

	void StubWindow::QueueMouseButton(MouseCode button, bool down)
	{
		m_Queue.push_back([this, button, down]()
		{
			Input::ProcessMouseButton(button, down);
			if (down)
			{
				MouseButtonPressedEvent event(button);
				Dispatch(event);
			}
			else
			{
				MouseButtonReleasedEvent event(button);
				Dispatch(event);
			}
		});
	}

	void StubWindow::QueueMouseMove(glm::vec2 position)
	{
		m_Queue.push_back([this, position]()
		{
			Input::ProcessMouseMove(position);
			MouseMovedEvent event(position.x, position.y);
			Dispatch(event);
		});
	}

	void StubWindow::QueueScroll(glm::vec2 offset)
	{
		m_Queue.push_back([this, offset]()
		{
			Input::ProcessScroll(offset);
			MouseScrolledEvent event(offset.x, offset.y);
			Dispatch(event);
		});
	}

	void StubWindow::QueueMouseLeave()
	{
		m_Queue.push_back([this]()
		{
			MouseLeaveEvent event;
			Dispatch(event);
		});
	}

	void StubWindow::QueueResize(uint32_t width, uint32_t height)
	{
		m_Queue.push_back([this, width, height]()
		{
			Width = width;
			Height = height;
			Framebuffer = glm::uvec2(glm::round(glm::vec2(width, height) * ContentScale));
			WindowResizeEvent event(width, height);
			Dispatch(event);
		});
	}

	void StubWindow::QueueFocus(bool focused)
	{
		m_Queue.push_back([this, focused]()
		{
			Focused = focused;
			if (focused)
			{
				WindowFocusEvent event;
				Dispatch(event);
			}
			else
			{
				WindowLostFocusEvent event;
				Dispatch(event);
			}
		});
	}

	void StubWindow::QueueContentScale(float scale)
	{
		m_Queue.push_back([this, scale]()
		{
			ContentScale = scale;
			WindowContentScaleEvent event(scale);
			Dispatch(event);
		});
	}

	void StubWindow::QueueFileDrop(std::vector<std::filesystem::path> paths)
	{
		m_Queue.push_back([this, paths = std::move(paths)]()
		{
			WindowFileDropEvent event(paths);
			Dispatch(event);
		});
	}

	void StubWindow::QueueClose()
	{
		m_Queue.push_back([this]()
		{
			WindowCloseEvent event;
			Dispatch(event);
		});
	}

	void StubWindow::SetGamepad(uint32_t gamepad, std::optional<GamepadState> state)
	{
		if (gamepad < c_MaxGamepads)
			m_Gamepads[gamepad] = std::move(state);
	}

	void StubWindow::ProcessEvents()
	{
		const uint32_t call = ProcessEventsCalls++;
		if (OnProcessEvents)
			OnProcessEvents(*this, call);

		// Events a delivered one queues (a callback that queues more) wait for the next call, as a window system's would.
		std::vector<std::function<void()>> queue = std::move(m_Queue);
		m_Queue.clear();
		for (const std::function<void()>& deliver : queue)
			deliver();

		for (uint32_t gamepad = 0; gamepad < c_MaxGamepads; gamepad++)
		{
			const std::optional<GamepadState>& state = m_Gamepads[gamepad];
			if (state)
				Input::ProcessGamepad(gamepad, true, state->Axes.data(), state->Buttons.data());
			else
				Input::ProcessGamepad(gamepad, false, nullptr, nullptr);
		}
	}

	void StubWindow::SetVSync(bool enabled)
	{
		VSync = enabled;
		VSyncChanges++;
	}

	void StubWindow::SetMode(WindowMode mode)
	{
		Mode = mode;
		ModeChanges++;
	}

	void StubWindow::SetResolution(uint32_t width, uint32_t height)
	{
		ResolutionChanges++;
		if (width == 0 || height == 0)
			return; // As Window::SetResolution: zero sizes are ignored
		Width = width;
		Height = height;
		Framebuffer = glm::uvec2(glm::round(glm::vec2(width, height) * ContentScale));
	}

	void StubWindow::SetCursorShape(CursorShape shape)
	{
		Shape = shape;
		ShapeChanges++;
	}

	KeyCode StubWindow::GetLayoutKey(KeyCode key) const
	{
		const auto it = Layout.find(key);
		return it != Layout.end() ? it->second : key;
	}

	void StubWindow::SetIcon(std::span<const WindowIconImage>)
	{
		IconSets++;
	}

	void StubWindow::Dispatch(Event& event)
	{
		if (m_EventCallback)
			m_EventCallback(event);
	}

}
