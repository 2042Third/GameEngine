#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/CommandLine.h"
#include "Strata/Core/EngineModule.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/LayerStack.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Timer.h"
#include "Strata/Core/Timestep.h"
#include "Strata/Core/Window.h"
#include "Strata/Events/ApplicationEvent.h"

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Strata
{

	struct ApplicationSpecification
	{
		std::string Name = "Strata Application";
		std::filesystem::path WorkingDirectory; // Empty keeps the current directory
		CommandLine CommandLineArgs;
		WindowSpecification Window;

		bool Headless = false; // No window; modules that need one (a swapchain, ImGui) do without or refuse to start
		// The engine modules the program runs (graphics, audio, ImGui, the project...), in the order they start where their
		// dependencies leave a choice: Application calls nothing else of the engine.
		std::vector<EngineModuleFactory> Modules;

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

		// The exit code of a process whose CreateApplication made no application: 1 (it could not start) unless set, e.g. 0
		// after printing what --help asked for, or 3 for a command line it does not understand.
		static void SetStartupExitCode(int exitCode);
		static int GetStartupExitCode();

		// The exit code of a run that quit because it was interrupted (Ctrl+C, SIGINT, SIGTERM: Platform::InstallInterruptHandler),
		// unless it had failed before: 128 + SIGINT, as shells report an interrupted program.
		static constexpr int c_InterruptedExitCode = 130;

		// A failure that ends the program before it could do what it was started for (no graphics device, no game to run):
		// logged as critical, and, for a person who would not see the log (Platform::ShouldShowErrorDialogs), also shown in a
		// dialog titled `title` with the advice and the log file's path. Main thread; blocks until the dialog is dismissed.
		static void ReportFatalError(std::string_view title, std::string_view problem, std::string_view advice = {});

		void OnEvent(Event& event);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* overlay);

		// Null in headless mode.
		Window* GetWindow() const { return m_Window.get(); }
		// The program's engine modules (Find<GraphicsModule>() and so on), in dependency order.
		EngineModuleRegistry& GetModules() { return m_Modules; }
		const EngineModuleRegistry& GetModules() const { return m_Modules; }
		const ApplicationSpecification& GetSpecification() const { return m_Specification; }
		// Frames that ran (minimized or skipped frames are not counted).
		uint64_t GetFrameCount() const { return m_FrameCount; }
		Timestep GetLastTimestep() const { return m_LastTimestep; }
		// CPU time of the last frame that ran, in seconds: its layers' updates, the modules' updates, the UI and recording and
		// submitting its GPU work. What modules wait for (EngineFrame::WaitSeconds: the GPU and the display), what they do
		// after the frame's work (OnEndFrame: presenting, back buffer captures) and the frame-rate cap are not counted, so it
		// measures what a frame costs whatever the display's refresh rate.
		double GetLastFrameWorkTime() const { return m_LastFrameWorkTime; }

		// Frames per second the main loop does not exceed from the next frame on (0: unlimited), on top of vsync. Starts at
		// ApplicationSpecification::MaxFrameRate; the editor lowers it while it is idle. Setting the current rate again
		// keeps the frame schedule, so it may be set every frame. Main thread only.
		void SetMaxFrameRate(uint32_t framesPerSecond) { m_FramePacer.SetMaxFrameRate(framesPerSecond); }
		uint32_t GetMaxFrameRate() const { return m_FramePacer.GetMaxFrameRate(); }

		// Queues a function to run on the main thread at the start of the next frame, or, after the last frame, before the
		// layers are destroyed and the modules shut down. Thread-safe.
		void SubmitToMainThread(std::function<void()> function);

		static Application& Get() { return *s_Instance; }
		static bool IsInitialized() { return s_Instance != nullptr; }
	protected:
		virtual void OnInit() {}
		virtual void OnShutdown() {}
	private:
		// Makes the modules and starts them; false when one could not start (the program cannot run).
		bool StartModules();
		// The end of the started modules after the layers are gone: OnRelease, the job system's shutdown, OnShutdown.
		void ShutDownModules();
		// Returns false when the frame was skipped (minimized, a module skipped it, nothing to render to).
		bool RunFrame(Timestep timestep);
		bool OnWindowResize(WindowResizeEvent& event);
		void ExecuteMainThreadQueue();
		std::span<EngineModule* const> GetStartedModules() const { return m_Modules.GetAll().first(m_StartedModules); }
	private:
		ApplicationSpecification m_Specification;
		Scope<Window> m_Window;
		LayerStack m_LayerStack;
		EngineModuleRegistry m_Modules;
		size_t m_StartedModules = 0; // The first ones of m_Modules, which got OnInit and succeeded

		bool m_Running = true;
		int m_ExitCode = 0;
		bool m_Minimized = false;
		double m_LastFrameTime = 0.0;
		Timestep m_LastTimestep;
		double m_LastFrameWorkTime = 0.0;
		uint64_t m_FrameCount = 0;
		FramePacer m_FramePacer;

		std::mutex m_MainThreadQueueMutex;
		std::vector<std::function<void()>> m_MainThreadQueue;
	private:
		static Application* s_Instance;
	};

}
