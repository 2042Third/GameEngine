#include "stpch.h"
#include "Strata/Core/Application.h"

#include "Strata/Core/Timer.h"
#include "Strata/Input/Input.h"

namespace Strata
{

	Application* Application::s_Instance = nullptr;

	Application::Application(const ApplicationSpecification& specification)
		: m_Specification(specification)
	{
		ST_CORE_VERIFY(!s_Instance, "Only one Application may exist at a time");
		s_Instance = this;

		if (!m_Specification.WorkingDirectory.empty())
		{
			std::error_code error;
			std::filesystem::current_path(m_Specification.WorkingDirectory, error);
			if (error)
				ST_CORE_ERROR("Failed to set working directory to '{}': {}", m_Specification.WorkingDirectory.string(), error.message());
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
	}

	Application::~Application()
	{
		// Layers may reference engine systems, so they go first.
		m_LayerStack.Clear();

		ExecuteMainThreadQueue();
		JobSystem::Shutdown();
		ExecuteMainThreadQueue();

		Input::SetWindow(nullptr);
		m_Window.reset();
		s_Instance = nullptr;
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
		dispatcher.Dispatch<WindowCloseEvent>(ST_BIND_EVENT_FN(OnWindowClose));
		dispatcher.Dispatch<WindowResizeEvent>(ST_BIND_EVENT_FN(OnWindowResize));

		for (auto it = m_LayerStack.rbegin(); it != m_LayerStack.rend(); ++it)
		{
			if (event.Handled)
				break;
			(*it)->OnEvent(event);
		}
	}

	void Application::Run()
	{
		OnInit();

		m_LastFrameTime = Time::GetTime();
		while (m_Running)
		{
			const double time = Time::GetTime();
			const float elapsed = static_cast<float>(time - m_LastFrameTime);
			m_LastFrameTime = time;
			m_LastTimestep = Timestep(std::min(elapsed, m_Specification.MaxTimestep));

			RunFrame(m_LastTimestep);

			m_FrameCount++;
			if (m_Specification.MaxFrames && m_FrameCount >= *m_Specification.MaxFrames)
				m_Running = false;
		}

		OnShutdown();
	}

	void Application::RunFrame(Timestep timestep)
	{
		ExecuteMainThreadQueue();

		Input::BeginFrame();
		if (m_Window)
		{
			m_Window->ProcessEvents();
			m_Minimized = m_Window->IsMinimized();
		}

		if (m_Minimized)
		{
			// Avoid spinning at 100% CPU while minimized.
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
			return;
		}

		for (Layer* layer : m_LayerStack)
			layer->OnUpdate(timestep);
	}

	bool Application::OnWindowClose(WindowCloseEvent&)
	{
		m_Running = false;
		return false;
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
