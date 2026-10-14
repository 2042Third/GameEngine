#include "stpch.h"
#include "Strata/Core/Application.h"

#include "Strata/Core/FileSystem.h"
#include "Strata/Core/FrameProfiler.h"
#include "Strata/Core/Platform.h"
#include "Strata/Core/Profiling.h"
#include "Strata/Core/Timer.h"
#include "Strata/Input/Input.h"

#include <chrono>
#include <thread>

namespace Strata
{

	Application* Application::s_Instance = nullptr;

	namespace
	{

		// Set before any application exists, read once CreateApplication returned none (main thread only).
		int s_StartupExitCode = 1;

		// How long a frame that could not run (minimized, nothing to render to) waits, so the loop does not spin a core.
		constexpr std::chrono::milliseconds c_SkippedFrameWait(16);

	}

	void Application::SetStartupExitCode(int exitCode)
	{
		s_StartupExitCode = exitCode;
	}

	int Application::GetStartupExitCode()
	{
		return s_StartupExitCode;
	}

	void Application::ReportFatalError(std::string_view title, std::string_view problem, std::string_view advice)
	{
		ST_CORE_CRITICAL("{}{}{}", problem, advice.empty() ? "" : " ", advice);
		if (!Platform::ShouldShowErrorDialogs())
			return;
		std::string message(problem);
		if (!advice.empty())
			message += fmt::format("\n\n{}", advice);
		if (const std::filesystem::path logFile = Log::GetLogFile(); !logFile.empty())
			message += fmt::format("\n\nThe log has the details: {}", FileSystem::ToUTF8(logFile));
		Log::Flush();
		// Tests read what the dialog would say instead of a dialog nobody could dismiss.
		if (const std::optional<std::string> file = Platform::GetEnvVar("STRATA_ERROR_DIALOG_FILE"); file && !file->empty())
		{
			if (!FileSystem::WriteText(FileSystem::FromUTF8(*file), fmt::format("{}\n{}\n", title, message)))
				ST_CORE_ERROR("Cannot write the error dialog's text to '{}'", *file);
			return;
		}
		if (!Platform::ShowErrorDialog(title, message))
			ST_CORE_WARN("No dialog could show the error: it is only in the log");
	}

	Application::Application(const ApplicationSpecification& specification)
		: m_Specification(specification), m_FramePacer(specification.MaxFrameRate)
	{
		ST_CORE_VERIFY(!s_Instance, "Only one Application may exist at a time");
		s_Instance = this;

		// Editors and games are interactive: they keep their speed while another window has the focus (Platform.h).
		if (!Platform::DisableExecutionSpeedThrottling())
			ST_CORE_WARN("The system did not exempt the process from power throttling: it may run slower while its windows are in the background");

		if (!m_Specification.WorkingDirectory.empty())
		{
			std::error_code error;
			std::filesystem::current_path(m_Specification.WorkingDirectory, error);
			if (error)
				ST_CORE_ERROR("Failed to set working directory to '{}': {}", FileSystem::ToUTF8(m_Specification.WorkingDirectory), error.message());
		}

		JobSystem::Init(m_Specification.Jobs);

		if (!m_Specification.Headless)
		{
			WindowSpecification windowSpecification = m_Specification.Window;
			if (windowSpecification.Title.empty())
				windowSpecification.Title = m_Specification.Name;
			m_Window = Window::Create(windowSpecification);
			m_Window->SetEventCallback(ST_BIND_EVENT_FN(OnEvent));
			Input::SetWindow(m_Window.get());
		}
		Input::Reset();

		if (!StartModules())
		{
			m_Running = false;
			m_ExitCode = 1;
		}
	}

	Application::~Application()
	{
		// Layers may reference engine systems, so they go first.
		m_LayerStack.Clear();
		ShutDownModules();
		m_Modules.Clear();

		Input::SetWindow(nullptr);
		m_Window.reset();
		s_Instance = nullptr;
	}

	bool Application::StartModules()
	{
		std::string error;
		if (!m_Modules.Create(m_Specification.Modules, &error))
		{
			Input::SetWindow(nullptr);
			m_Window.reset();
			ReportFatalError(m_Specification.Name, fmt::format("{} cannot start: {}.", m_Specification.Name, error));
			return false;
		}

		EngineContext context { *this, m_Window.get(), m_Modules };
		for (EngineModule* module : m_Modules.GetAll())
		{
			if (module->OnInit(context))
			{
				m_StartedModules++;
				continue;
			}
			ST_CORE_CRITICAL("Engine module '{}' could not start; the application cannot run", module->GetName());
			// The person learns what the module describes once nothing of the program is left on the screen: the modules that
			// started end in their usual order (no layer exists yet), then the window, which would never show anything, closes.
			const EngineModuleStartFailure failure = module->GetStartFailure();
			ShutDownModules();
			Input::SetWindow(nullptr);
			m_Window.reset();
			if (!failure.Problem.empty())
				ReportFatalError(m_Specification.Name, failure.Problem, failure.Advice);
			return false;
		}
		return true;
	}

	void Application::ShutDownModules()
	{
		// Assets own GPU resources and their manager waits for load jobs: modules release them (OnRelease) while the job
		// system and the graphics device still exist.
		const std::span<EngineModule* const> started = GetStartedModules();
		for (auto it = started.rbegin(); it != started.rend(); ++it)
			(*it)->OnRelease();

		ExecuteMainThreadQueue();
		JobSystem::Shutdown();
		ExecuteMainThreadQueue();

		for (auto it = started.rbegin(); it != started.rend(); ++it)
			(*it)->OnShutdown();
		m_StartedModules = 0;
	}

	void Application::PushLayer(Layer* layer)
	{
		m_LayerStack.PushLayer(layer);
		layer->OnAttach();
	}

	void Application::PushOverlay(Layer* overlay)
	{
		m_LayerStack.PushOverlay(overlay);
		overlay->OnAttach();
	}

	void Application::Close()
	{
		m_Running = false;
	}

	void Application::SubmitToMainThread(std::function<void()> function)
	{
		std::scoped_lock<std::mutex> lock(m_MainThreadQueueMutex);
		m_MainThreadQueue.emplace_back(std::move(function));
	}

	void Application::OnEvent(Event& event)
	{
		EventDispatcher dispatcher(event);
		dispatcher.Dispatch<WindowResizeEvent>(ST_BIND_EVENT_FN(OnWindowResize));

		for (EngineModule* module : GetStartedModules())
		{
			if (event.Handled)
				break;
			module->OnEvent(event);
		}
		for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it)
		{
			if (event.Handled)
				break;
			(*it)->OnEvent(event);
		}

		// A close request goes through the layers first: one that handles it (to ask about unsaved changes, for
		// example) keeps the application running.
		if (event.GetEventType() == EventType::WindowClose && !event.Handled)
			m_Running = false;
	}

	void Application::Run()
	{
		if (!m_Running)
			return;

		OnInit();

		m_LastFrameTime = Time::GetTime();
		while (m_Running)
		{
			ST_PROFILE_FRAME();
			const double time = Time::GetTime();
			const float elapsed = static_cast<float>(time - m_LastFrameTime);
			m_LastFrameTime = time;
			m_LastTimestep = Timestep(std::min(elapsed, m_Specification.MaxTimestep));

			bool frameRan = false;
			try
			{
				frameRan = RunFrame(m_LastTimestep);
			}
			catch (const std::exception& exception)
			{
				// Engine code does not throw; this catches third-party failures (e.g. the graphics driver reporting a
				// lost device inside NVRHI) so the application still shuts down in order and flushes its logs.
				ST_CORE_CRITICAL("Unhandled exception in the main loop: {}", exception.what());
				m_Running = false;
			}

			if (frameRan)
			{
				m_FrameCount++;
				if (m_Specification.MaxFrames && m_FrameCount >= *m_Specification.MaxFrames)
					m_Running = false;
			}
			// Ctrl+C in the terminal of a Console run, or a termination signal: the frame ran to its end, and the layers
			// shut down as after any other close.
			if (m_Running && Platform::IsInterruptRequested())
			{
				ST_CORE_WARN("Interrupted (Ctrl+C or a request to terminate): closing");
				if (m_ExitCode == 0)
					m_ExitCode = c_InterruptedExitCode;
				m_Running = false;
			}
			if (m_Running)
				m_FramePacer.WaitForNextFrame();
		}

		// What the last frame posted for the main thread (a screenshot answered without the Graphics module, a job's
		// result) runs as the next frame would have run it: while the layers that asked for it still exist, and before
		// main reads the exit code it may set.
		ExecuteMainThreadQueue();

		OnShutdown();
		const std::span<EngineModule* const> started = GetStartedModules();
		for (auto it = started.rbegin(); it != started.rend(); ++it)
			(*it)->OnStop();
	}

	bool Application::RunFrame(Timestep timestep)
	{
		// The frame's work time (GetLastFrameWorkTime) leaves out what the modules wait for and what they do after the
		// frame's work (OnEndFrame). The frame profiler's frame is the same: it is ended with that work time, and a skipped
		// frame is never ended.
		const double frameStart = Time::GetTime();
		FrameProfiler& profiler = FrameProfiler::Get();
		profiler.BeginFrame();
		ExecuteMainThreadQueue();

		{
			ST_PROFILE_STAGE("Input");
			Input::BeginFrame();
			if (m_Window)
			{
				m_Window->ProcessEvents();
				m_Minimized = m_Window->IsMinimized();
			}
		}

		if (m_Minimized)
		{
			// Avoid spinning at 100% CPU while minimized.
			std::this_thread::sleep_for(c_SkippedFrameWait);
			return false;
		}

		const std::span<EngineModule* const> modules = GetStartedModules();
		EngineFrame frame { timestep, 0.0, m_LayerStack };
		for (EngineModule* module : modules)
		{
			switch (module->OnBeginFrame(frame))
			{
				case EngineFrameAction::Continue:
					break;
				case EngineFrameAction::Skip:
					std::this_thread::sleep_for(c_SkippedFrameWait);
					return false;
				case EngineFrameAction::Close:
					Close();
					return false;
			}
		}

		for (Layer* layer : m_LayerStack)
			layer->OnUpdate(timestep);
		for (EngineModule* module : modules)
			module->OnUpdate(frame);
		for (EngineModule* module : modules)
			module->OnRenderUI(frame);
		m_LastFrameWorkTime = Time::GetTime() - frameStart - frame.WaitSeconds;

		for (auto it = modules.rbegin(); it != modules.rend(); ++it)
			(*it)->OnEndFrame(frame);
		profiler.EndFrame(m_LastFrameWorkTime);
		return true;
	}

	bool Application::OnWindowResize(WindowResizeEvent& event)
	{
		m_Minimized = event.GetWidth() == 0 || event.GetHeight() == 0;
		return false;
	}

	void Application::ExecuteMainThreadQueue()
	{
		std::vector<std::function<void()>> queue;
		{
			std::scoped_lock<std::mutex> lock(m_MainThreadQueueMutex);
			queue.swap(m_MainThreadQueue);
		}

		for (std::function<void()>& function : queue)
			function();
	}

}
