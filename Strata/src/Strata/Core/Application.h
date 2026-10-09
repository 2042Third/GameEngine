#pragma once

#include "Strata/Core/Base.h"
#include "Strata/Core/CommandLine.h"
#include "Strata/Core/JobSystem.h"
#include "Strata/Core/LayerStack.h"
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

	struct ApplicationSpecification
	{
		std::string Name = "Strata Application";
		std::filesystem::path WorkingDirectory; // Empty keeps the current directory
		CommandLine CommandLineArgs;
		WindowSpecification Window;

		bool Headless = false;     // No window or swapchain; offscreen rendering remains available
		bool EnableRenderer = true;
		bool EnableImGui = false;
		bool EnableAudio = true;
		std::optional<bool> GraphicsValidation; // Defaults to enabled in Debug builds

		JobSystemSpecification Jobs;
		std::optional<uint64_t> MaxFrames; // Close automatically after this many frames (automation, tests)
		float MaxTimestep = 0.25f;         // Clamp for long frames (breakpoints, loading hitches)
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

		void OnEvent(Event& event);

		void PushLayer(Layer* layer);
		void PushOverlay(Layer* overlay);

		// Null in headless mode.
		Window* GetWindow() const { return m_Window.get(); }
		const ApplicationSpecification& GetSpecification() const { return m_Specification; }
		uint64_t GetFrameCount() const { return m_FrameCount; }
		Timestep GetLastTimestep() const { return m_LastTimestep; }

		// Queues a function to run on the main thread at the start of the next frame. Thread-safe.
		void SubmitToMainThread(std::function<void()> function);

		static Application& Get() { return *s_Instance; }
		static bool IsInitialized() { return s_Instance != nullptr; }
	protected:
		virtual void OnInit() {}
		virtual void OnShutdown() {}
	private:
		void RunFrame(Timestep timestep);
		bool OnWindowClose(WindowCloseEvent& event);
		bool OnWindowResize(WindowResizeEvent& event);
		void ExecuteMainThreadQueue();
	private:
		ApplicationSpecification m_Specification;
		Scope<Window> m_Window;
		LayerStack m_LayerStack;

		bool m_Running = true;
		bool m_Minimized = false;
		double m_LastFrameTime = 0.0;
		Timestep m_LastTimestep;
		uint64_t m_FrameCount = 0;

		std::mutex m_MainThreadQueueMutex;
		std::vector<std::function<void()>> m_MainThreadQueue;
	private:
		static Application* s_Instance;
	};

	// Implemented by the client (editor, runtime, sandbox).
	Application* CreateApplication(const CommandLine& commandLine);

}
