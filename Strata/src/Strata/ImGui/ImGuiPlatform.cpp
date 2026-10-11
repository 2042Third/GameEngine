#include "stpch.h"
#include "Strata/ImGui/ImGuiPlatform.h"

#include "Strata/Core/Timer.h"
#include "Strata/Core/Window.h"
#include "Strata/Events/ApplicationEvent.h"
#include "Strata/Events/KeyEvent.h"
#include "Strata/Events/MouseEvent.h"

#include <algorithm>
#include <cfloat>
#include <iterator>

namespace Strata
{

	namespace
	{

		// The time step of the first frame, which has no frame before it.
		constexpr double c_FirstFrameSeconds = 1.0 / 60.0;
		// ImGui needs a positive time step: two frames within the clock's resolution are this far apart.
		constexpr double c_MinFrameSeconds = 1.0e-6;

		// The modifiers, in the order of ImGuiPlatform's pairs of modifier keys (left, right).
		struct ModifierKeys
		{
			KeyCode Left;
			KeyCode Right;
			ImGuiKey Modifier;
		};
		constexpr ModifierKeys c_Modifiers[] = {
			{ Key::LeftControl, Key::RightControl, ImGuiMod_Ctrl },
			{ Key::LeftShift, Key::RightShift, ImGuiMod_Shift },
			{ Key::LeftAlt, Key::RightAlt, ImGuiMod_Alt },
			{ Key::LeftSuper, Key::RightSuper, ImGuiMod_Super }
		};

		CursorShape ToCursorShape(ImGuiMouseCursor cursor)
		{
			switch (cursor)
			{
				case ImGuiMouseCursor_None:       return CursorShape::Hidden;
				case ImGuiMouseCursor_TextInput:  return CursorShape::TextInput;
				case ImGuiMouseCursor_ResizeAll:  return CursorShape::ResizeAll;
				case ImGuiMouseCursor_ResizeNS:   return CursorShape::ResizeNS;
				case ImGuiMouseCursor_ResizeEW:   return CursorShape::ResizeEW;
				case ImGuiMouseCursor_ResizeNESW: return CursorShape::ResizeNESW;
				case ImGuiMouseCursor_ResizeNWSE: return CursorShape::ResizeNWSE;
				case ImGuiMouseCursor_Hand:       return CursorShape::Hand;
				case ImGuiMouseCursor_NotAllowed: return CursorShape::NotAllowed;
				// The arrow, and Wait and Progress, which windows have no standard cursor for (GLFW).
				default:                          return CursorShape::Arrow;
			}
		}

	}

	ImGuiPlatform::~ImGuiPlatform()
	{
		Shutdown();
	}

	void ImGuiPlatform::Init(Window& window)
	{
		ImGuiIO& io = ImGui::GetIO();
		ST_CORE_ASSERT(!m_Window && !io.BackendPlatformUserData, "ImGui already has a platform backend");
		m_Window = &window;
		m_Time = 0.0;
		m_AppliedCursor.reset();
		m_ModifierKeys = {};
		m_PressedKeys.fill(ImGuiKey_None);

		io.BackendPlatformName = "Strata";
		io.BackendPlatformUserData = this;
		io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
		ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
		platformIO.Platform_GetClipboardTextFn = &ImGuiPlatform::GetClipboardText;
		platformIO.Platform_SetClipboardTextFn = &ImGuiPlatform::SetClipboardText;
		// ImGui's own input method support (Windows) places the candidate list through the main viewport's window.
		ImGui::GetMainViewport()->PlatformHandleRaw = window.GetPlatformHandle();
	}

	void ImGuiPlatform::Shutdown()
	{
		if (!m_Window)
			return;
		if (ImGui::GetCurrentContext() && ImGui::GetIO().BackendPlatformUserData == this)
		{
			ImGuiIO& io = ImGui::GetIO();
			io.BackendPlatformName = nullptr;
			io.BackendPlatformUserData = nullptr;
			io.BackendFlags &= ~ImGuiBackendFlags_HasMouseCursors;
			ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
			platformIO.Platform_GetClipboardTextFn = nullptr;
			platformIO.Platform_SetClipboardTextFn = nullptr;
			ImGui::GetMainViewport()->PlatformHandleRaw = nullptr;
		}
		m_Window = nullptr;
	}

	void ImGuiPlatform::OnEvent(Event& event)
	{
		if (!m_Window)
			return;
		ImGuiIO& io = ImGui::GetIO();
		switch (event.GetEventType())
		{
			case EventType::KeyPressed:
			{
				// ImGui repeats held keys itself.
				const KeyPressedEvent& key = static_cast<const KeyPressedEvent&>(event);
				if (!key.IsRepeat())
					OnKey(key.GetKeyCode(), true);
				break;
			}
			case EventType::KeyReleased:
				OnKey(static_cast<const KeyReleasedEvent&>(event).GetKeyCode(), false);
				break;
			case EventType::KeyTyped:
				io.AddInputCharacter(static_cast<const KeyTypedEvent&>(event).GetCodepoint());
				break;
			case EventType::MouseMoved:
			{
				const MouseMovedEvent& move = static_cast<const MouseMovedEvent&>(event);
				io.AddMousePosEvent(move.GetX(), move.GetY());
				break;
			}
			case EventType::MouseLeave:
				// Nothing of ImGui's is under the cursor.
				io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
				break;
			case EventType::MouseButtonPressed:
			case EventType::MouseButtonReleased:
			{
				const MouseCode button = static_cast<const MouseButtonEvent&>(event).GetMouseButton();
				if (button < ImGuiMouseButton_COUNT)
					io.AddMouseButtonEvent(static_cast<int>(button), event.GetEventType() == EventType::MouseButtonPressed);
				break;
			}
			case EventType::MouseScrolled:
			{
				const MouseScrolledEvent& scroll = static_cast<const MouseScrolledEvent&>(event);
				io.AddMouseWheelEvent(scroll.GetXOffset(), scroll.GetYOffset());
				break;
			}
			case EventType::WindowFocus:
				io.AddFocusEvent(true);
				break;
			case EventType::WindowLostFocus:
				// ImGui releases its keys and buttons; the window reports the releases of the keys it had down, but the keys
				// pressed now are pressed anew.
				m_ModifierKeys = {};
				m_PressedKeys.fill(ImGuiKey_None);
				io.AddFocusEvent(false);
				break;
			default:
				break;
		}
	}

	void ImGuiPlatform::OnKey(KeyCode key, bool down)
	{
		ImGuiIO& io = ImGui::GetIO();
		// The modifier first: ImGui takes a key's chord from the modifiers queued before it.
		for (size_t index = 0; index < std::size(c_Modifiers); index++)
		{
			const ModifierKeys& modifier = c_Modifiers[index];
			if (key != modifier.Left && key != modifier.Right)
				continue;
			m_ModifierKeys[index * 2 + (key == modifier.Right ? 1 : 0)] = down;
			io.AddKeyEvent(modifier.Modifier, m_ModifierKeys[index * 2] || m_ModifierKeys[index * 2 + 1]);
		}
		if (key >= c_MaxKeyCode)
			return;

		// A release is the key its press was queued as, whatever the layout says now.
		ImGuiKey imguiKey = m_PressedKeys[key];
		if (down || imguiKey == ImGuiKey_None)
			imguiKey = ToImGuiKey(m_Window->GetLayoutKey(key));
		m_PressedKeys[key] = down ? imguiKey : ImGuiKey_None;
		if (imguiKey != ImGuiKey_None)
			io.AddKeyEvent(imguiKey, down);
	}

	void ImGuiPlatform::NewFrame()
	{
		ST_CORE_ASSERT(m_Window, "ImGuiPlatform::NewFrame before Init");
		ImGuiIO& io = ImGui::GetIO();
		const float width = static_cast<float>(m_Window->GetWidth());
		const float height = static_cast<float>(m_Window->GetHeight());
		io.DisplaySize = ImVec2(width, height);
		// Pixels per window unit: 1 where windows are measured in pixels (Windows), 2 on a Retina display (macOS).
		if (width > 0.0f && height > 0.0f)
		{
			const glm::uvec2 framebuffer = m_Window->GetFramebufferSize();
			io.DisplayFramebufferScale = ImVec2(static_cast<float>(framebuffer.x) / width, static_cast<float>(framebuffer.y) / height);
		}

		const double now = Time::GetTime();
		io.DeltaTime = static_cast<float>(m_Time > 0.0 ? std::max(now - m_Time, c_MinFrameSeconds) : c_FirstFrameSeconds);
		m_Time = now;
		UpdateCursorShape();
	}

	void ImGuiPlatform::UpdateCursorShape()
	{
		const ImGuiIO& io = ImGui::GetIO();
		if ((io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) || m_Window->GetCursorMode() != CursorMode::Normal)
		{
			// The cursor is not ImGui's now (a game locked or hid it): it gets ImGui's shape again once it is.
			m_AppliedCursor.reset();
			return;
		}
		// While ImGui draws the cursor itself, the system's is hidden.
		const ImGuiMouseCursor cursor = io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
		if (m_AppliedCursor == cursor)
			return;
		m_Window->SetCursorShape(ToCursorShape(cursor));
		m_AppliedCursor = cursor;
	}

	const char* ImGuiPlatform::GetClipboardText(ImGuiContext*)
	{
		ImGuiPlatform* platform = static_cast<ImGuiPlatform*>(ImGui::GetIO().BackendPlatformUserData);
		if (!platform || !platform->m_Window)
			return nullptr;
		platform->m_ClipboardText = platform->m_Window->GetClipboardText();
		return platform->m_ClipboardText.c_str();
	}

	void ImGuiPlatform::SetClipboardText(ImGuiContext*, const char* text)
	{
		ImGuiPlatform* platform = static_cast<ImGuiPlatform*>(ImGui::GetIO().BackendPlatformUserData);
		if (platform && platform->m_Window && text)
			platform->m_Window->SetClipboardText(text);
	}

	ImGuiKey ImGuiPlatform::ToImGuiKey(KeyCode key)
	{
		if (key >= Key::A && key <= Key::Z)
			return static_cast<ImGuiKey>(ImGuiKey_A + (key - Key::A));
		if (key >= Key::D0 && key <= Key::D9)
			return static_cast<ImGuiKey>(ImGuiKey_0 + (key - Key::D0));
		if (key >= Key::F1 && key <= Key::F24)
			return static_cast<ImGuiKey>(ImGuiKey_F1 + (key - Key::F1));
		if (key >= Key::KP0 && key <= Key::KP9)
			return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (key - Key::KP0));
		switch (key)
		{
			case Key::Space:        return ImGuiKey_Space;
			case Key::Apostrophe:   return ImGuiKey_Apostrophe;
			case Key::Comma:        return ImGuiKey_Comma;
			case Key::Minus:        return ImGuiKey_Minus;
			case Key::Period:       return ImGuiKey_Period;
			case Key::Slash:        return ImGuiKey_Slash;
			case Key::Semicolon:    return ImGuiKey_Semicolon;
			case Key::Equal:        return ImGuiKey_Equal;
			case Key::LeftBracket:  return ImGuiKey_LeftBracket;
			case Key::Backslash:    return ImGuiKey_Backslash;
			case Key::RightBracket: return ImGuiKey_RightBracket;
			case Key::GraveAccent:  return ImGuiKey_GraveAccent;
			case Key::World2:       return ImGuiKey_Oem102; // The key between the left Shift and Z of ISO keyboards
			case Key::Escape:       return ImGuiKey_Escape;
			case Key::Enter:        return ImGuiKey_Enter;
			case Key::Tab:          return ImGuiKey_Tab;
			case Key::Backspace:    return ImGuiKey_Backspace;
			case Key::Insert:       return ImGuiKey_Insert;
			case Key::Delete:       return ImGuiKey_Delete;
			case Key::Right:        return ImGuiKey_RightArrow;
			case Key::Left:         return ImGuiKey_LeftArrow;
			case Key::Down:         return ImGuiKey_DownArrow;
			case Key::Up:           return ImGuiKey_UpArrow;
			case Key::PageUp:       return ImGuiKey_PageUp;
			case Key::PageDown:     return ImGuiKey_PageDown;
			case Key::Home:         return ImGuiKey_Home;
			case Key::End:          return ImGuiKey_End;
			case Key::CapsLock:     return ImGuiKey_CapsLock;
			case Key::ScrollLock:   return ImGuiKey_ScrollLock;
			case Key::NumLock:      return ImGuiKey_NumLock;
			case Key::PrintScreen:  return ImGuiKey_PrintScreen;
			case Key::Pause:        return ImGuiKey_Pause;
			case Key::KPDecimal:    return ImGuiKey_KeypadDecimal;
			case Key::KPDivide:     return ImGuiKey_KeypadDivide;
			case Key::KPMultiply:   return ImGuiKey_KeypadMultiply;
			case Key::KPSubtract:   return ImGuiKey_KeypadSubtract;
			case Key::KPAdd:        return ImGuiKey_KeypadAdd;
			case Key::KPEnter:      return ImGuiKey_KeypadEnter;
			case Key::KPEqual:      return ImGuiKey_KeypadEqual;
			case Key::LeftShift:    return ImGuiKey_LeftShift;
			case Key::LeftControl:  return ImGuiKey_LeftCtrl;
			case Key::LeftAlt:      return ImGuiKey_LeftAlt;
			case Key::LeftSuper:    return ImGuiKey_LeftSuper;
			case Key::RightShift:   return ImGuiKey_RightShift;
			case Key::RightControl: return ImGuiKey_RightCtrl;
			case Key::RightAlt:     return ImGuiKey_RightAlt;
			case Key::RightSuper:   return ImGuiKey_RightSuper;
			case Key::Menu:         return ImGuiKey_Menu;
			default:                return ImGuiKey_None;
		}
	}

}
