#include "stpch.h"
#include "Strata/Core/Application.h"

#include "Strata/Audio/AudioEngine.h"
#include "Strata/Core/FileSystem.h"
#include "Strata/Core/Timer.h"
#include "Strata/ImGui/ImGuiLayer.h"
#include "Strata/Input/Input.h"
#include "Strata/Renderer/GraphicsDevice.h"
#include "Strata/Renderer/Renderer.h"

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

		if (m_Specification.EnableRenderer && !InitializeGraphics())
		{
			ST_CORE_CRITICAL("No usable graphics device; the application cannot run");
			m_Running = false;
			return;
		}

		if (m_Specification.EnableAudio)
		{
			AudioEngineSpecification audioSpecification;
			audioSpecification.NullDevice = m_Specification.Headless;
			if (!AudioEngine::Init(audioSpecification))
				ST_CORE_ERROR("Audio is unavailable; continuing without sound");
		}

		if (m_Specification.EnableImGui && m_Window && m_GraphicsDevice)
		{
			m_ImGuiLayer = new ImGuiLayer(m_Specification.ImGuiLayoutFile);
			PushOverlay(m_ImGuiLayer);
		}
	}

	Application::~Application()
	{
		// Layers may reference engine systems, so they go first.
		m_LayerStack.Clear();
		m_ImGuiLayer = nullptr;

		ExecuteMainThreadQueue();
		JobSystem::Shutdown();
		ExecuteMainThreadQueue();

		AudioEngine::Shutdown();
		Renderer::Shutdown();
		m_GraphicsDevice.reset();

		Input::SetWindow(nullptr);
		m_Window.reset();
		s_Instance = nullptr;
	}

	bool Application::InitializeGraphics()
	{
		GraphicsDeviceSpecification deviceSpecification;
		deviceSpecification.ApplicationName = m_Specification.Name;
		deviceSpecification.Headless = m_Window == nullptr;
#if defined(ST_DEBUG)
		deviceSpecification.EnableValidation = m_Specification.GraphicsValidation.value_or(true);
#else
		deviceSpecification.EnableValidation = m_Specification.GraphicsValidation.value_or(false);
#endif

		m_GraphicsDevice = GraphicsDevice::Create(deviceSpecification);
		if (!m_GraphicsDevice)
			return false;

		if (m_Window && !m_GraphicsDevice->CreateSwapchain(*m_Window, m_Window->IsVSync()))
		{
			m_GraphicsDevice.reset();
			return false;
		}

		if (!Renderer::Init(*m_GraphicsDevice))
		{
			m_GraphicsDevice.reset();
			return false;
		}
		return true;
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
		}
		ProcessBackBufferCaptures(false);

		OnShutdown();
		if (m_GraphicsDevice)
			m_GraphicsDevice->WaitForIdle();
	}

	bool Application::RunFrame(Timestep timestep)
	{
		ExecuteMainThreadQueue();

		Input::BeginFrame();
		if (m_Window)
		{
			m_Window->ProcessEvents();
			m_Minimized = m_Window->IsMinimized();
			if (m_GraphicsDevice && m_Window->IsVSync() != m_GraphicsDevice->IsVSync())
				m_GraphicsDevice->SetVSync(m_Window->IsVSync());
		}

		if (!m_GraphicsDevice)
			ProcessBackBufferCaptures(false); // Nothing will ever be rendered

		if (m_Minimized)
		{
			// Avoid spinning at 100% CPU while minimized.
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
			return false;
		}

		// A device that cannot render this frame (e.g. swapchain mid-resize) skips the whole frame.
		if (m_GraphicsDevice && !m_GraphicsDevice->BeginFrame())
		{
			if (m_GraphicsDevice->IsDeviceLost())
			{
				Close();
				return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
			return false;
		}
		Renderer::BeginFrame();

		for (Layer* layer : m_LayerStack)
			layer->OnUpdate(timestep);

		AudioEngine::Update();
		RenderImGui();

		if (m_GraphicsDevice)
		{
			ProcessBackBufferCaptures(true);
			m_GraphicsDevice->EndFrame();
		}
		return true;
	}

	void Application::RequestBackBufferCapture(std::function<void(const ReadbackImage&)> callback)
	{
		std::scoped_lock<std::mutex> lock(m_CaptureMutex);
		m_BackBufferCaptures.push_back(std::move(callback));
	}

	void Application::ProcessBackBufferCaptures(bool frameRendered)
	{
		std::vector<std::function<void(const ReadbackImage&)>> captures;
		{
			std::scoped_lock<std::mutex> lock(m_CaptureMutex);
			captures.swap(m_BackBufferCaptures);
		}
		if (captures.empty())
			return;

		ReadbackImage image;
		nvrhi::ITexture* backBuffer = frameRendered && m_GraphicsDevice ? m_GraphicsDevice->GetBackBuffer() : nullptr;
		if (backBuffer && !Renderer::ReadTexture(backBuffer, image))
		{
			ST_CORE_ERROR("Back buffer capture failed");
			image = ReadbackImage();
		}
		for (const auto& capture : captures)
			capture(image);
	}

	void Application::RenderImGui()
	{
		if (!m_ImGuiLayer)
			return;

		m_ImGuiLayer->Begin();
		for (Layer* layer : m_LayerStack)
			layer->OnImGuiRender();

		m_ImGuiLayer->End(m_GraphicsDevice->GetBackBufferFramebuffer());
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
