#pragma once

#include "EditorHost.h"

#include <Strata/Core/Base.h>
#include <Strata/ImGui/ImGuiLayer.h>

#include <glm/glm.hpp>
#include <imgui.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace Strata
{
	class Event;
	class Layer;
}

namespace Strata::Tests
{

	// An EditorHost without an application, a window or a GPU. The test sets what it reports (time, focus, UI scale)
	// through its state and reads what the layer asked of it (window title, frame rate, exit code, closing). The state is
	// shared, so the test keeps it after handing the host to the layer.
	class FakeEditorHost : public EditorHost
	{
	public:
		struct State
		{
			bool Running = true;
			int ExitCode = 0;
			uint64_t FrameCount = 0;
			double Time = 0.0;
			bool WindowFocused = true;
			float UIScale = 1.0f;
			uint32_t MaxFrameRate = 0;
			uint32_t FrameRateChanges = 0; // SetMaxFrameRate calls that changed the rate
			double FrameWorkTime = 0.0;    // Reported as the last frame's work time
			std::string WindowTitle;
			uint32_t ScreenshotRequests = 0;
			std::optional<double> ProcessUptime = 0.25;
			std::optional<GraphicsDeviceInfo> GraphicsDevice;
		};

		explicit FakeEditorHost(Ref<State> state)
			: m_State(std::move(state))
		{
		}

		bool IsRunning() const override { return m_State->Running; }
		void Close() override { m_State->Running = false; }
		void SetExitCode(int exitCode) override { m_State->ExitCode = exitCode; }
		uint64_t GetFrameCount() const override { return m_State->FrameCount; }
		double GetTime() const override { return m_State->Time; }
		std::optional<double> GetProcessUptime() const override { return m_State->ProcessUptime; }
		// Rendering needs a real device: the fake only describes one (GraphicsDevice), for what the UI shows about it.
		bool HasGraphicsDevice() const override { return false; }
		std::optional<GraphicsDeviceInfo> GetGraphicsDeviceInfo() const override { return m_State->GraphicsDevice; }
		// No window: the layer neither opens native file dialogs nor sets a title.
		bool HasWindow() const override { return false; }
		void SetWindowTitle(const std::string& title) override { m_State->WindowTitle = title; }
		glm::uvec2 GetWindowSize() const override { return glm::uvec2(0); }
		bool IsWindowFocused() const override { return m_State->WindowFocused; }
		float GetUIScale() const override { return m_State->UIScale; }
		void SetMaxFrameRate(uint32_t framesPerSecond) override
		{
			if (framesPerSecond != m_State->MaxFrameRate)
				m_State->FrameRateChanges++;
			m_State->MaxFrameRate = framesPerSecond;
		}
		uint32_t GetMaxFrameRate() const override { return m_State->MaxFrameRate; }
		double GetLastFrameWorkTime() const override { return m_State->FrameWorkTime; }
		void RequestScreenshot(std::function<void(const ReadbackImage&)>) override { m_State->ScreenshotRequests++; }
	private:
		Ref<State> m_State;
	};

	// Draws ImGui without a window or a GPU, for tests of the editor's UI (EditorLayer, panels, widgets):
	// - an ImGui context with the editor's fonts (UI::EditorFonts) and theme, styled for a content scale by an unattached
	//   engine ImGuiLayer, as in the editor;
	// - frames run NewFrame, the drawing and Render; ImGui's texture requests (the font atlas) are honored without a
	//   renderer, marking each texture ready with a dummy id;
	// - mouse and keyboard input is injected through ImGui's input queue and applies at the next frame;
	// - widgets of the kit are found through UI::ItemProbe (ClickItem);
	// - the clipboard is the harness's own (GetClipboard), never the system's.
	// ConfigDebugHighlightIdConflicts is on: GetHoveredItemIdCount tells whether the item hovered in the frame before the
	// last shares its id with another item. One harness at a time (it owns the current ImGui context).
	struct ImGuiHarnessSpecification
	{
		ImVec2 DisplaySize = ImVec2(1600.0f, 900.0f);
		float ContentScale = 1.0f;
	};

	class ImGuiHarness
	{
	public:
		// At namespace scope: GCC cannot use a nested class's member initializers in a default argument of its enclosing class.
		using Specification = ImGuiHarnessSpecification;

		explicit ImGuiHarness(const Specification& specification = {});
		~ImGuiHarness();

		ImGuiHarness(const ImGuiHarness&) = delete;
		ImGuiHarness& operator=(const ImGuiHarness&) = delete;

		bool AreFontsLoaded() const { return m_FontsLoaded; }
		// Styles this context (Bedrock theme); it is not attached, so it has no platform or renderer backend.
		ImGuiLayer& GetImGuiLayer() { return m_ImGuiLayer; }

		// One frame: the drawing runs between NewFrame and Render; then the frame's texture requests are honored.
		void Frame(const std::function<void()>& draw, float deltaTime = c_DeltaTime);
		// One frame of a layer, as Application runs it: OnUpdate, then OnImGuiRender inside the ImGui frame.
		void Frame(Layer& layer, float deltaTime = c_DeltaTime);
		// Dispatches an event as Application does: to the ImGuiLayer (an overlay), then to the layer unless handled.
		void DispatchEvent(Event& event, Layer& layer);

		// Input for the next frame.
		void MoveMouse(const ImVec2& position);
		void SetMouseButton(ImGuiMouseButton button, bool down);
		void SetKey(ImGuiKey key, bool down);
		// Types text into the active text field (as characters, like a keyboard's text input).
		void TypeText(std::string_view text);
		// Clicks the widget the probe recorded under the key in the last frame: moves the mouse onto it, presses and
		// releases the left button over three frames of the layer. False (and no input) when the probe has no unique,
		// enabled widget under that key.
		bool ClickItem(std::string_view probeKey, Layer& layer);

		// How many items were submitted in the last frame with the id of the item hovered in the frame before (at most 1
		// without an id conflict).
		int GetHoveredItemIdCount() const;
		ImGuiID GetPreviouslyHoveredId() const;
		// Texture creations and updates honored so far.
		uint32_t GetTextureRequestCount() const { return m_TextureRequests; }
		// The harness's clipboard: ImGui's copy and paste use it instead of the system's, so tests leave the user's alone.
		const std::string& GetClipboard() const { return m_Clipboard; }

		static constexpr float c_DeltaTime = 1.0f / 60.0f;
	private:
		void HonorTextureRequests();
	private:
		ImGuiContext* m_Context = nullptr;
		ImGuiLayer m_ImGuiLayer;
		bool m_FontsLoaded = false;
		uint32_t m_TextureRequests = 0;
		std::string m_Clipboard;
	};

}
