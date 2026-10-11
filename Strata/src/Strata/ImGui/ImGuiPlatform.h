#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Input/KeyCodes.h"

#include <imgui.h>

#include <array>
#include <optional>
#include <string>

namespace Strata
{

	class Event;
	class Window;

	// Dear ImGui's platform backend: ImGui's input comes from the window's events and its platform services from the
	// window (Window). The window reports what changed through events, so a frame asks the system for nothing: the display
	// size is the window's (kept up to date by its resize events), the mouse position its last move, and the cursor's shape
	// is set only when ImGui wants another one. Single main viewport (no platform windows of ImGui's own).
	//  - Keys: as the keyboard layout names them (Window::GetLayoutKey), so lettered shortcuts work in every layout, with
	//    the modifiers they were pressed with (both keys of a modifier counted). Repeats are ImGui's own. Keys and buttons
	//    pressed while the window loses the focus are released (focus events).
	//  - Text: typed characters (KeyTypedEvent), also those an input method composed. On Windows ImGui's own input method
	//    support places the candidate list by the text cursor, through the window's handle (Window::GetPlatformHandle).
	//  - Mouse: moves, buttons (the first five), the wheel (both axes); leaving the window clears the position, unless a
	//    drag keeps the cursor's moves coming.
	//  - Cursor shapes (ImGuiBackendFlags_HasMouseCursors) while the window's cursor mode is Normal: a game that locked
	//    or hid the cursor keeps it.
	//  - Clipboard: the system's, through the window.
	// Content scale is the ImGuiLayer's (WindowContentScaleEvent).
	class ImGuiPlatform
	{
	public:
		ImGuiPlatform() = default;
		~ImGuiPlatform();
		ImGuiPlatform(const ImGuiPlatform&) = delete;
		ImGuiPlatform& operator=(const ImGuiPlatform&) = delete;

		// Becomes the platform backend of the current ImGui context, for this window (which must outlive Shutdown).
		void Init(Window& window);
		void Shutdown();
		bool IsInitialized() const { return m_Window != nullptr; }

		// Queues what an input event of the window means to ImGui; other events change nothing. Call it for every event,
		// before anything handles it.
		void OnEvent(Event& event);
		// Before ImGui::NewFrame: the display size and framebuffer scale, the time since the last frame, and the cursor shape
		// ImGui asked for in the frame before.
		void NewFrame();

		// The ImGui key a key of the key events stands for (by its name in the US layout), ImGuiKey_None for keys ImGui has
		// none for.
		static ImGuiKey ToImGuiKey(KeyCode key);
	private:
		void OnKey(KeyCode key, bool down);
		void UpdateCursorShape();
		// ImGui's clipboard functions (ImGuiPlatformIO), for the backend of the current context.
		static const char* GetClipboardText(ImGuiContext* context);
		static void SetClipboardText(ImGuiContext* context, const char* text);
	private:
		Window* m_Window = nullptr;
		double m_Time = 0.0;
		// The cursor ImGui last asked for and the window was given; reset while the cursor is not ImGui's (a locked or
		// hidden cursor), so it is given again when it returns.
		std::optional<ImGuiMouseCursor> m_AppliedCursor;
		// Which keys of the modifiers are down (left and right of Ctrl, Shift, Alt and Super).
		std::array<bool, 8> m_ModifierKeys = {};
		// The ImGui key each pressed key was queued as: its release is queued as the same one, even if the layout changed
		// meanwhile.
		std::array<ImGuiKey, c_MaxKeyCode> m_PressedKeys = {};
		// The clipboard's text as last read for ImGui, which keeps the pointer until it asks again.
		std::string m_ClipboardText;
	};

}
