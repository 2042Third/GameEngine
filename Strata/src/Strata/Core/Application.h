#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/CommandLine.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/LayerStack.h"
#include "Strata/Core/Timer.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Core/Window.h"
#include "Strata/Events/ApplicationEvent.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace Strata
{

	class GraphicsDevice;
	struct ReadbackImage;
	class ImGuiLayer;

	struct ApplicationSpecification
	{
		std::string Name = "Strata Application";
		std::filesystem::path WorkingDirectory; // Empty keeps the current directory
		CommandLine CommandLineArgs;
		WindowSpecification Window;

		bool Headless = false;     // No window or swapchain; offscreen rendering remains available
		bool EnableRenderer = true;
		bool EnableImGui = false;
		// Initializes the AudioEngine. Headless applications (and machines without an output device) mix without a device,
		// advanced by the frame time so that playback still progresses.
		bool EnableAudio = true;
		std::filesystem::path ImGuiLayoutFile; // Where ImGui persists its layout (empty: not persisted)
		std::optional<bool> GraphicsValidation; // Defaults to enabled in Debug builds

		JobSystemSpecification Jobs;
		std::optional<uint64_t> MaxFrames; // Close automatically after this many frames (automation, tests)
		float MaxTimestep = 0.25f;         // Clamp for long frames (breakpoints, loading hitches)
		// Frames per second the main loop does not exceed (0: unlimited). Headless applications have no vsync to pace
		// them and would otherwise keep a CPU core busy.
		uint32_t MaxFrameRate = 0;
	};

	class Application
	{
	public:
		Application(const ApplicationSpecification& specification);
		virtual ~Application();

		Application(const Application&) = delete;
		Application& operator=(const Application&) = delete;

		void Run();
		void Close();
		bool IsRunning() const { return m_Running; }
		// Process exit code returned from main (0 unless set; 1 when startup failed).
		void SetExitCode(int exitCode) { m_ExitCode = exitCode; }
		int GetExitCode() const { return m_ExitCode; }

		void OnEvent(Event& event);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* overlay);

		// Null in headless mode.
		Window* GetWindow() const { return m_Window.get(); }
		// Null when the renderer is disabled or no GPU is available.
		GraphicsDevice* GetGraphicsDevice() const { return m_GraphicsDevice.get(); }
		ImGuiLayer* GetImGuiLayer() const { return m_ImGuiLayer; }
		const ApplicationSpecification& GetSpecification() const { return m_Specification; }
		// Frames that ran (minimized or skipped frames are not counted).
		uint64_t GetFrameCount() const { return m_FrameCount; }
		Timestep GetLastTimestep() const { return m_LastTimestep; }

		// Frames per second the main loop does not exceed from the next frame on (0: unlimited), on top of vsync. Starts at
		// ApplicationSpecification::MaxFrameRate; the editor lowers it while it is idle. Setting the current rate again
		// keeps the frame schedule, so it may be set every frame. Main thread only.
		void SetMaxFrameRate(uint32_t framesPerSecond) { m_FramePacer.SetMaxFrameRate(framesPerSecond); }
		uint32_t GetMaxFrameRate() const { return m_FramePacer.GetMaxFrameRate(); }

		// Queues a function to run on the main thread at the start of the next frame. Thread-safe.
		void SubmitToMainThread(std::function<void()> function);

		// Reads back the window's back buffer at the end of the next rendered frame (after the UI is drawn, before
		// presenting) and passes it to the callback on the main thread. Used for editor screenshots. The image is empty
		// (Width 0) when there is nothing to capture (no renderer, headless, read failure). Thread-safe.
		void RequestBackBufferCapture(std::function<void(const ReadbackImage&)> callback);

		static Application& Get() { return *s_Instance; }
		static bool IsInitialized() { return s_Instance != nullptr; }
	protected:
		virtual void OnInit() {}
		virtual void OnShutdown() {}
	private:
		bool InitializeGraphics();
		// Returns false when the frame was skipped (minimized, nothing to render to).
		bool RunFrame(Timestep timestep);
		void ProcessBackBufferCaptures(bool frameRendered);
		void RenderImGui();
		bool OnWindowResize(WindowResizeEvent& event);
		void ExecuteMainThreadQueue();
	private:
		ApplicationSpecification m_Specification;
		Scope<Window> m_Window;
		Scope<GraphicsDevice> m_GraphicsDevice;
		ImGuiLayer* m_ImGuiLayer = nullptr;
		LayerStack m_LayerStack;

		bool m_Running = true;
		int m_ExitCode = 0;
		bool m_Minimized = false;
		double m_LastFrameTime = 0.0;
		Timestep m_LastTimestep;
		uint64_t m_FrameCount = 0;
		FramePacer m_FramePacer;

		std::mutex m_MainThreadQueueMutex;
		std::vector<std::function<void()>> m_MainThreadQueue;
		std::mutex m_CaptureMutex;
		std::vector<std::function<void(const ReadbackImage&)>> m_BackBufferCaptures;
	private:
		static Application* s_Instance;
	};

	// Implemented by the client (editor, runtime, sandbox).
	Application* CreateApplication(const CommandLine& commandLine);

}
