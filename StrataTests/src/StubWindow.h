#pragma once

#include "Strata/Core/Window.h"
#include "Strata/Input/GamepadCodes.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Strata::Tests
{

	// A Window without a window system: it records what is asked of it in public members, and ProcessEvents delivers what a
	// test queued, in order, as GLFWWindow does: into Input (Input::Process*) and then through the event callback. Screen
	// coordinates are pixels unless ContentScale is set. It cannot present (GetVulkanPresentation is null), so a device made
	// for it renders offscreen. Replaces the tests' own fake windows.
	class StubWindow final : public Window
	{
	public:
		void QueueKey(KeyCode key, bool down, bool repeat = false); // KeyPressed (repeat: no Input change) or KeyReleased
		void QueueText(uint32_t codepoint);                          // KeyTyped
		void QueueMouseButton(MouseCode button, bool down);
		void QueueMouseMove(glm::vec2 position);
		void QueueScroll(glm::vec2 offset);
		void QueueMouseLeave();
		void QueueResize(uint32_t width, uint32_t height);           // Width, Height and Framebuffer; WindowResize
		void QueueFocus(bool focused);                               // Focused; WindowFocus or WindowLostFocus
		void QueueContentScale(float scale);                         // ContentScale; WindowContentScale
		void QueueFileDrop(std::vector<std::filesystem::path> paths);
		void QueueClose();                                           // WindowClose

		// What every ProcessEvents reports for a gamepad (Input::ProcessGamepad for each of c_MaxGamepads, as GLFW's backend
		// polls them); nullopt, the default, is disconnected.
		struct GamepadState
		{
			std::array<float, c_GamepadAxisCount> Axes {};
			std::array<bool, c_GamepadButtonCount> Buttons {};
		};
		void SetGamepad(uint32_t gamepad, std::optional<GamepadState> state);

		// Called at the start of every ProcessEvents with its index from 0, before the queue is delivered: per-frame scripts
		// (minimize for some frames, queue a close at the third).
		std::function<void(StubWindow&, uint32_t call)> OnProcessEvents;

		// Window: every pure virtual, reading and writing the members below. Every setter counts every call, also one that
		// sets the value it already has, as the fakes it replaced did: tests prove that their code calls the window only on a
		// change. SetResolution(w, h) with both non-zero sets Width, Height and Framebuffer (scaled by ContentScale).
		void ProcessEvents() override;
		uint32_t GetWidth() const override { return Width; }
		uint32_t GetHeight() const override { return Height; }
		glm::uvec2 GetFramebufferSize() const override { return Framebuffer; }
		float GetContentScale() const override { return ContentScale; }
		void SetEventCallback(const EventCallbackFn& callback) override { m_EventCallback = callback; }
		void SetVSync(bool enabled) override;
		bool IsVSync() const override { return VSync; }
		void SetTitle(const std::string& title) override { Title = title; }
		void SetMode(WindowMode mode) override;
		WindowMode GetMode() const override { return Mode; }
		void SetResolution(uint32_t width, uint32_t height) override;
		bool IsMinimized() const override { return Minimized; }
		bool IsFocused() const override { return Focused; }
		void Maximize() override { MaximizeCalls++; }
		void SetCursorMode(CursorMode mode) override { Cursor = mode; }
		CursorMode GetCursorMode() const override { return Cursor; }
		void SetCursorShape(CursorShape shape) override;
		std::string GetClipboardText() const override { return Clipboard; }
		void SetClipboardText(const std::string& text) override { Clipboard = text; }
		KeyCode GetLayoutKey(KeyCode key) const override;
		void* GetPlatformHandle() const override { return Handle; }
		void SetIcon(std::span<const WindowIconImage> images) override;

		uint32_t Width = 1280;
		uint32_t Height = 720;
		glm::uvec2 Framebuffer = glm::uvec2(1280, 720);
		float ContentScale = 1.0f;
		bool Minimized = false;
		bool Focused = true;
		bool VSync = true;
		int VSyncChanges = 0;
		std::string Title;
		WindowMode Mode = WindowMode::Windowed;
		int ModeChanges = 0;
		int ResolutionChanges = 0;
		CursorMode Cursor = CursorMode::Normal;
		CursorShape Shape = CursorShape::Arrow;
		int ShapeChanges = 0;
		std::string Clipboard;
		std::map<KeyCode, KeyCode> Layout; // Keys the layout names otherwise (by position -> by what it types)
		void* Handle = nullptr;            // GetPlatformHandle
		int MaximizeCalls = 0;
		int IconSets = 0;
		uint32_t ProcessEventsCalls = 0;
	private:
		void Dispatch(Event& event);
	private:
		EventCallbackFn m_EventCallback;
		std::vector<std::function<void()>> m_Queue;
		std::array<std::optional<GamepadState>, c_MaxGamepads> m_Gamepads;
	};

}
